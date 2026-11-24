"""PlatformIO upload actions, preserving board identity checks and receiver backup."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

ESPRESSIF_VID = 0x303A


def load_local_config(root):
    path = Path(root) / 'upload.local.json'
    if not path.is_file():
        return {}
    return json.loads(path.read_text())


def resolve_setting(root, env_key, json_key, cli_value=''):
    if cli_value and cli_value.strip():
        return cli_value.strip()
    env_value = os.environ.get(env_key, '').strip()
    if env_value:
        return env_value
    return str(load_local_config(root).get(json_key, '')).strip()


def _fat_mount_points():
    mounts = []
    proc_mounts = Path('/proc/mounts')
    if not proc_mounts.is_file():
        return mounts
    for line in proc_mounts.read_text().splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[2] in ('vfat', 'msdos'):
            mount_path = parts[1].replace('\\040', ' ')
            mounts.append(Path(mount_path))
    return mounts


def _find_rp2350_mounts(candidates):
    matches = []
    for mount in candidates:
        info = mount / 'INFO_UF2.TXT'
        try:
            if info.is_file() and 'Board-ID: RP2350' in info.read_text():
                matches.append(str(mount))
        except OSError:
            continue
    return matches


def _try_mount_rp2350():
    dev = Path('/dev/disk/by-label/RP2350')
    if not dev.exists():
        return
    try:
        subprocess.run(['udisksctl', 'mount', '-b', str(dev)],
                       capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.SubprocessError):
        pass


def _trigger_pico_bootsel(by_id_dir=Path('/dev/serial/by-id')):
    if not by_id_dir.is_dir():
        return False
    picos = [str(e) for e in sorted(by_id_dir.iterdir())
             if 'raspberry_pi_pico' in e.name.lower()]
    if len(picos) != 1:
        return False
    try:
        import serial
    except ImportError:
        return False
    print('Found running Pico at', picos[0], '- requesting BOOTSEL via 1200 baud...')
    try:
        ser = serial.Serial(picos[0], 1200)
        try:
            ser.dtr = False
            time.sleep(0.1)
        finally:
            try:
                ser.close()
            except Exception:
                pass
    except Exception:
        # Device often disconnects immediately on 1200-baud reboot, which raises I/O error.
        pass
    return True


def detect_sender_port(mounts=None):
    if mounts is not None:
        matches = _find_rp2350_mounts([Path(m) for m in mounts])
    else:
        matches = _find_rp2350_mounts(_fat_mount_points())
        if not matches:
            _try_mount_rp2350()
            matches = _find_rp2350_mounts(_fat_mount_points())
        if not matches and _trigger_pico_bootsel():
            deadline = time.monotonic() + 8.0
            while time.monotonic() < deadline and not matches:
                time.sleep(0.5)
                _try_mount_rp2350()
                matches = _find_rp2350_mounts(_fat_mount_points())
    if len(matches) == 1:
        print('Auto-detected RP2350 sender at', matches[0])
        return matches[0]
    if not matches:
        raise RuntimeError(
            'No RP2350 BOOTSEL drive found; hold BOOTSEL while connecting the Pico 2 W, '
            'or set SENDER_UPLOAD_PORT / upload.local.json'
        )
    raise RuntimeError(
        'Multiple RP2350 BOOTSEL drives found (' + ', '.join(matches) +
        '); unplug extras or set SENDER_UPLOAD_PORT / upload.local.json'
    )


def detect_receiver_port(by_id_dir=Path('/dev/serial/by-id')):
    matches = []
    if by_id_dir.is_dir():
        for entry in sorted(by_id_dir.iterdir()):
            if 'espressif' in entry.name.lower():
                matches.append(str(entry))
    if not matches:
        try:
            from serial.tools import list_ports
            for info in sorted(list_ports.comports(), key=lambda p: p.device):
                if info.vid == ESPRESSIF_VID:
                    matches.append(info.device)
        except ImportError:
            pass
    if len(matches) == 1:
        print('Auto-detected Espressif receiver at', matches[0])
        return matches[0]
    if not matches:
        raise RuntimeError(
            'No Espressif ESP32-S3 USB device found; connect the receiver '
            'or set RECEIVER_UPLOAD_PORT / upload.local.json'
        )
    raise RuntimeError(
        'Multiple Espressif USB devices found (' + ', '.join(matches) +
        '); unplug extras or set RECEIVER_UPLOAD_PORT / upload.local.json'
    )


def upload_sender(build, port=''):
    port = port.strip() if port else detect_sender_port()
    mount = Path(port)
    if 'Board-ID: RP2350' not in (mount / 'INFO_UF2.TXT').read_text():
        raise RuntimeError('Not an RP2350 BOOTSEL drive; refusing to flash')
    firmware = build / 'firmware.uf2'
    print('Installing', firmware, 'SHA256', hashlib.sha256(firmware.read_bytes()).hexdigest())
    with firmware.open('rb') as src, (mount / 'well_sender.uf2').open('wb') as dst:
        shutil.copyfileobj(src, dst)
        dst.flush()
        os.fsync(dst.fileno())


def upload_receiver(root, build, port='', expected_mac=None):
    files = {'0x0': 'bootloader.bin', '0x8000': 'partitions.bin', '0x10000': 'firmware.bin'}
    flash = json.loads((build / 'flasher_args.json').read_text())
    if set(flash['flash_files']) != set(files):
        raise RuntimeError('Unexpected flash offsets; refusing to flash')
    for name in files.values():
        if not (build / name).is_file():
            raise RuntimeError('Missing artifact: ' + name)
    mac = (
        expected_mac.strip().lower()
        if expected_mac is not None
        else resolve_setting(root, 'RECEIVER_MAC', 'receiver_mac').lower()
    )
    if mac == 'xx:xx:xx:xx:xx:xx':
        mac = ''
    if not mac and not sys.stdin.isatty():
        raise RuntimeError(
            'RECEIVER_MAC is required for non-interactive receiver flash; '
            'set RECEIVER_MAC or upload.local.json'
        )
    port = port.strip() if port else detect_receiver_port()
    base = [sys.executable, '-m', 'esptool', '--chip', 'esp32s3', '--port', port,
            '--before', 'default-reset']
    identity = subprocess.run(base + ['--after', 'no-reset', 'read-mac'], check=True,
                              capture_output=True, text=True, timeout=45)
    if mac:
        if mac not in identity.stdout.lower():
            raise RuntimeError('Wrong receiver; refusing to flash: ' + identity.stdout)
    else:
        mac_lines = [line.strip() for line in identity.stdout.splitlines() if 'mac:' in line.lower()]
        summary = mac_lines[0] if mac_lines else identity.stdout.strip()
        print(f'\nDetected ESP32-S3 receiver on {port} ({summary})', flush=True)
        print('>>> Type y and press Enter to flash this receiver (anything else cancels): ', end='', flush=True)
        answer = input().strip().lower()
        if answer not in ('y', 'yes'):
            raise RuntimeError('Receiver flash cancelled by user')
    backups = root / '.backups'
    backups.mkdir(exist_ok=True)
    (backups / 'receiver-identity.txt').write_text(identity.stdout)
    backup = backups / 'receiver-before.bin'
    if not backup.exists():
        print('Backing up 16 MiB receiver flash to .backups/receiver-before.bin (one-time, ~1-2 min)...', flush=True)
        subprocess.run(base + ['--after', 'no-reset', '--baud', '921600', 'read-flash',
                               '0', 'ALL', str(backup)], check=True, timeout=180)
    if backup.stat().st_size != 16777216:
        raise RuntimeError('Incomplete receiver backup; refusing to flash')
    backup.with_suffix('.sha256').write_text(hashlib.sha256(backup.read_bytes()).hexdigest() + '\n')
    command = base + ['--after', 'watchdog-reset', '--baud', '921600', 'write-flash']
    for offset, name in files.items():
        command += [offset, str(build / name)]
    subprocess.run(command, check=True, timeout=120)


def upload(source, target, env):
    root = Path(env.subst('$PROJECT_DIR'))
    build = Path(env.subst('$BUILD_DIR'))
    cli_port = env.subst('$UPLOAD_PORT')
    if env.subst('$PIOENV') == 'sender':
        port = resolve_setting(root, 'SENDER_UPLOAD_PORT', 'sender_port', cli_port)
        upload_sender(build, port)
    else:
        port = resolve_setting(root, 'RECEIVER_UPLOAD_PORT', 'receiver_port', cli_port)
        upload_receiver(root, build, port)


if 'Import' in globals():
    Import('env')
    env.Replace(UPLOAD_PROTOCOL="custom", UPLOADCMD=upload)
    if env.subst('$PIOENV') == 'sender':
        picosdk_dir = env.PioPlatform().get_package_dir('framework-picosdk')
        mdns_src = os.path.join(picosdk_dir, 'lib', 'lwip', 'src', 'apps', 'mdns')
        env.BuildSources(os.path.join('$BUILD_DIR', 'PicoSDKLwIPMDNS'), mdns_src)
