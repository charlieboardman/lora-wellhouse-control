#!/usr/bin/env bash
# One entry point. No activation, toolchain paths, or PlatformIO flags required.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$root"
if (( $# == 0 )) && [[ -t 0 ]]; then
    while true; do
        echo 'LoRa wellhouse controller'
        echo '  1) Flash sender (Pico 2 W)'
        echo '  2) Flash receiver (ESP32-S3)'
        echo '  3) Flash both boards'
        echo '  4) Test and build both boards (no flashing)'
        echo '  5) Run host tests only'
        echo '  6) Exit'
        printf 'Select [1-6]: '
        if ! read -r choice; then
            echo
            exit 0
        fi
        case "$choice" in
            1) command_name=flash; board=sender; break ;;
            2) command_name=flash; board=receiver; break ;;
            3) command_name=flash; board=all; break ;;
            4) command_name=build; board=all; break ;;
            5) command_name=test; board=all; break ;;
            6|q|Q|'') exit 0 ;;
            *) echo "Invalid selection: $choice" ;;
        esac
    done
else
    command_name="${1:-build}"
    board="${2:-all}"
fi
case "$command_name" in
  build|flash|test) ;;
  help|--help|-h)
    echo 'Usage: bash deploy.sh [build|flash|test] [sender|receiver|all]'
    echo 'With no arguments in a terminal, opens an interactive menu.'
    echo 'Non-interactive default: set up, test, and build both boards.'
    exit 0 ;;
  *) echo "Unknown command: $command_name" >&2; exit 2 ;;
esac
case "$board" in sender|receiver|all) ;; *) echo "Unknown board: $board" >&2; exit 2 ;; esac
if (( $# > 2 )); then echo 'Too many arguments. Run bash deploy.sh help.' >&2; exit 2; fi
mkdir -p .build
exec 9>.build/deploy.lock
flock 9
python="$root/.venv/bin/python"
if [[ ! -x "$python" ]]; then python3 -m venv "$root/.venv"; fi
if ! "$python" - <<'PY'
from importlib.metadata import version, PackageNotFoundError
try:
    assert all(version(name) == expected for name, expected in (
        ('platformio', '6.2.0'), ('esptool', '5.6.0'), ('ziglang', '0.17.1'), ('pymodbus', '3.16.2')))
except (AssertionError, PackageNotFoundError):
    raise SystemExit(1)
PY
then
    "$python" -m pip install platformio==6.2.0 esptool==5.6.0 ziglang==0.17.1 pymodbus==3.16.2
fi
export ZIG_GLOBAL_CACHE_DIR="$root/.build/zig-global"
export ZIG_LOCAL_CACHE_DIR="$root/.build/zig-local"

# Compare source/tool metadata and every required output; timestamps don't matter.
# Save only after a successful build, including packages installed during it.
build_stamp() {
    "$python" - "$1" "$2" <<'PY'
import hashlib
import json
import os
from pathlib import Path
import sys

mode, board = sys.argv[1:]
root = Path.cwd()
core = Path(os.environ.get('PLATFORMIO_CORE_DIR', Path.home() / '.platformio')).expanduser()
inputs = [root / 'deploy.sh', root / 'platformio.ini', root / 'firmware/config.h',
          root / 'firmware/wifi.example.h', root / '.build/link_id.h']
if board == 'sender':
    inputs += [root / '.build/wifi.h', root / 'firmware/wifi.local.h']
inputs += sorted((root / 'firmware' / board).rglob('*'))
inputs += sorted((root / 'firmware/gateway').rglob('*'))
if board == 'receiver':
    inputs += [root / 'CMakeLists.txt', root / 'firmware/CMakeLists.txt']
def fingerprint(paths):
    sha = hashlib.sha256(sys.version.encode())
    for path in paths:
        if path.is_file() and '__pycache__' not in path.parts and not path.name.endswith('.old'):
            sha.update(str(path).encode() + b'\0' + path.read_bytes() + b'\0')
    return sha.hexdigest()

if mode == 'sources':
    print(fingerprint(inputs))
    raise SystemExit(0)

# Package/platform changes invalidate the cache even if application source did not.
for parent in (core / 'packages', core / 'platforms'):
    if parent.is_dir():
        for package in sorted(parent.iterdir()):
            inputs += [package / 'package.json', package / 'platform.json', package / '.piopm']
            if parent.name == 'platforms':
                inputs += [package / 'platform.py']
                inputs += sorted((package / 'builder').rglob('*.py'))
                inputs += sorted((package / 'boards').glob('*.json'))
input_hash = fingerprint(inputs)
build = root / '.pio/build' / board
names = ['firmware.elf', 'firmware.bin']
names += ['firmware.uf2'] if board == 'sender' else ['bootloader.bin', 'partitions.bin', 'flasher_args.json']
outputs = {}
for name in names:
    path = build / name
    if not path.is_file():
        if mode == 'save':
            raise SystemExit(f'Build did not produce {path}; leaving it uncached')
        raise SystemExit(1)
    outputs[name] = hashlib.sha256(path.read_bytes()).hexdigest()
state = {'inputs': input_hash, 'outputs': outputs}
stamp = root / '.build' / (board + '.json')
if mode == 'check':
    try:
        valid = json.loads(stamp.read_text()) == state
    except (OSError, ValueError):
        valid = False
    raise SystemExit(0 if valid else 1)
temp = stamp.with_suffix('.tmp')
temp.write_text(json.dumps(state, indent=2) + '\n')
temp.replace(stamp)
PY
}

echo 'Checking firmware and upload behavior...'
mkdir -p .build/tests
for suite in control sender_runtime gateway radio; do
    "$python" -m ziglang c++ -std=c++17 -Wall -Wextra -Werror \
        -DGW_LINK_ID='UINT64_C(0x1)' -fsanitize=undefined "tests/${suite}_test.cpp" \
        -o ".build/tests/${suite}_test"
    ".build/tests/${suite}_test"
done
"$python" tests/upload_test.py
"$python" -m ziglang c++ -std=c++17 -Wall -Wextra -Werror -DGW_LINK_ID='UINT64_C(0x1)' \
    -fsanitize=undefined tests/interop_host.cpp -o .build/tests/interop_host
"$python" tests/interop_test.py
[[ "$command_name" != test ]] || exit 0

"$python" -c '
import hashlib
import os
from pathlib import Path
import re
import secrets
import sys

command = sys.argv[1]
target = Path(".build/link_id.h")
raw = os.environ.get("GW_LINK_ID", "").strip()

def saved_link_id():
    if not target.is_file():
        return None
    text = target.read_text()
    if "build-check only" in text:
        return None
    m = re.search(r"UINT64_C\(0x([0-9a-fA-F]+)\)", text)
    return int(m.group(1), 16) if m else None

def parse_link_id(text):
    cleaned = text.strip()
    if not cleaned:
        raise SystemExit("Invalid GW_LINK_ID: cannot be empty.")
    low = cleaned.lower()
    if low.startswith("0x") and 1 <= len(low[2:]) <= 16 and all(c in "0123456789abcdef" for c in low[2:]):
        value = int(low[2:], 16)
        if value == 0:
            raise SystemExit("Invalid GW_LINK_ID: must be non-zero.")
        return value
    digest = hashlib.sha256(cleaned.encode("utf-8")).digest()[:8]
    return int.from_bytes(digest, "big") or 1

if command == "flash":
    prev = saved_link_id()
    if not raw:
        if not sys.stdin.isatty():
            raise SystemExit("GW_LINK_ID is required for non-interactive flash (e.g. GW_LINK_ID=my-well bash deploy.sh flash).")
        if prev is not None:
            raw = input(f"Enter shared link ID [Enter to reuse 0x{prev:016x}]: ").strip()
            if not raw:
                value = prev
                print(f"Reusing saved GW_LINK_ID=0x{value:016x}.")
            else:
                value = parse_link_id(raw)
                print(f"Using GW_LINK_ID=0x{value:016x} (from \"{raw}\").")
        else:
            raw = input("Enter shared link ID (any text, or press Enter to generate one): ").strip()
            if not raw:
                value = secrets.randbits(64) or 1
                print(f"Generated GW_LINK_ID=0x{value:016x} (saved locally for the other board).")
            else:
                value = parse_link_id(raw)
                print(f"Using GW_LINK_ID=0x{value:016x} (from \"{raw}\"; saved locally for the other board).")
    else:
        value = parse_link_id(raw)
    content = f"#pragma once\n#define GW_LINK_ID UINT64_C(0x{value:016x})\n"
elif raw:
    value = parse_link_id(raw)
    content = f"#pragma once\n#define GW_LINK_ID UINT64_C(0x{value:016x})\n"
elif target.is_file():
    raise SystemExit(0)
else:
    print("Note: using build-check link ID only; flash will prompt for your shared link ID.")
    content = "#pragma once\n#define GW_LINK_ID UINT64_C(0x0000000000000001) /* build-check only */\n"

if not target.is_file() or target.read_text() != content:
    target.write_text(content)
' "$command_name"

if [[ "$command_name" == flash && ( "$board" == sender || "$board" == all ) ]]; then
"$python" -c '
import getpass
import os
from pathlib import Path
import re
import sys

def c_escape(s):
    return s.replace("\\", "\\\\").replace("\"", "\\\"").replace("\n", "").replace("\r", "")

target = Path(".build/wifi.h")
saved_ssid = ""
if target.is_file():
    m = re.search(r"#define\s+WIFI_SSID\s+\"([^\"]*)\"", target.read_text())
    if m:
        saved_ssid = m.group(1)

if "WIFI_SSID" in os.environ:
    ssid = os.environ["WIFI_SSID"]
    password = os.environ.get("WIFI_PASSWORD", "")
    ntp = os.environ.get("NTP_SERVER", "pool.ntp.org")
elif sys.stdin.isatty():
    if saved_ssid:
        ans = input(f"Enter Wi-Fi SSID [Enter to reuse \"{saved_ssid}\", or \"-\" to skip Wi-Fi]: ").strip()
        if not ans:
            print(f"Reusing saved Wi-Fi credentials for \"{saved_ssid}\".")
            raise SystemExit(0)
        ssid = "" if ans == "-" else ans
    else:
        ssid = input("Enter Wi-Fi SSID (leave blank to skip Wi-Fi): ").strip()
    if ssid:
        password = getpass.getpass("Enter Wi-Fi password (leave blank for open network): ")
    else:
        password = ""
    ntp = os.environ.get("NTP_SERVER", "pool.ntp.org")
else:
    raise SystemExit(0)

content = (
    "#pragma once\n"
    f"#define WIFI_SSID \"{c_escape(ssid)}\"\n"
    f"#define WIFI_PASSWORD \"{c_escape(password)}\"\n"
    f"#define NTP_SERVER \"{c_escape(ntp)}\"\n"
)
if not target.is_file() or target.read_text() != content:
    target.write_text(content)
    try:
        os.chmod(target, 0o600)
    except OSError:
        pass
'
fi

boards=(sender receiver)
if [[ "$board" != all ]]; then boards=("$board"); fi
for target_board in "${boards[@]}"; do
    if build_stamp check "$target_board"; then
        echo "$target_board: unchanged; using verified build."
    else
        # Invalidate first: a failed/interrupted build must never retain a receipt.
        "$python" -c 'import pathlib,sys; pathlib.Path(sys.argv[1]).unlink(missing_ok=True)' \
            "$root/.build/$target_board.json"
        sources_before="$(build_stamp sources "$target_board")"
        echo "$target_board: building..."
        "$python" -m platformio run -e "$target_board"
        if [[ "$sources_before" != "$(build_stamp sources "$target_board")" ]]; then
            echo "$target_board: inputs changed during the build; rerun bash deploy.sh." >&2
            exit 1
        fi
        build_stamp save "$target_board"
    fi
done
if [[ "$command_name" == flash ]]; then
    for target_board in "${boards[@]}"; do
        # nobuild uses the verified images above, without invoking compilation again.
        echo "$target_board: flashing..."
        "$python" -m platformio run -e "$target_board" -t nobuild -t upload
        if [[ "$target_board" == sender && "${SKIP_WIFI_VERIFY:-0}" != 1 ]]; then
            "$python" -c '
import json
import re
import socket
import sys
import time
import urllib.request
from pathlib import Path

def configured_ssid():
    for path in (Path(".build/wifi.h"), Path("firmware/wifi.local.h")):
        if path.is_file():
            m = re.search(r"#define\s+WIFI_SSID\s+\"([^\"]*)\"", path.read_text())
            if m:
                return m.group(1)
    return ""

ssid = configured_ssid()
if not ssid:
    print("sender: Wi-Fi skipped (no SSID configured); skipping connectivity check.")
    raise SystemExit(0)

cfg = Path("firmware/config.h").read_text()
m = re.search(r"#define\s+DEVICE_HOSTNAME\s+\"([^\"]+)\"", cfg)
host = (m.group(1) if m else "wellhouse-modbus-bridge") + ".local"
url = f"http://{host}/pump/status"

def wait_for_wifi(timeout_s):
    print(f"sender: waiting up to {timeout_s}s for Wi-Fi connectivity at {url} ...", flush=True)
    deadline = time.monotonic() + timeout_s
    last_err = ""
    while time.monotonic() < deadline:
        try:
            ip = socket.gethostbyname(host)
            with urllib.request.urlopen(url, timeout=3) as resp:
                data = json.loads(resp.read().decode())
            if resp.status == 200 and data.get("wifi"):
                print(f"sender: Wi-Fi verified! http://{host}/ (IP {ip})")
                return True
            last_err = f"unexpected status/payload: {resp.status} {data}"
        except Exception as exc:
            last_err = str(exc)
        time.sleep(2)
    print(f"sender: could not verify Wi-Fi at {url} ({last_err}).", file=sys.stderr)
    return False

timeout = 45
while not wait_for_wifi(timeout):
    if not sys.stdin.isatty():
        raise SystemExit(1)
    ans = input("Check SSID/password or USB serial log. [r]etry (30s), [c]ontinue anyway, or [A]bort? ").strip().lower()
    if ans in ("r", "retry"):
        timeout = 30
        continue
    if ans in ("c", "continue"):
        break
    raise SystemExit(1)
'
        fi
    done
fi
