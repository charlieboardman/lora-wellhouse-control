"""PlatformIO upload actions, preserving board identity checks and receiver backup."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


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


def upload_sender(build, port):
    if not port:
        raise RuntimeError(
            'Sender upload port is not configured; set SENDER_UPLOAD_PORT or upload.local.json'
        )
    mount = Path(port)
    if 'Board-ID: RP2350' not in (mount / 'INFO_UF2.TXT').read_text():
        raise RuntimeError('Not an RP2350 BOOTSEL drive; refusing to flash')
    firmware = build / 'firmware.uf2'
    print('Installing', firmware, 'SHA256', hashlib.sha256(firmware.read_bytes()).hexdigest())
    with firmware.open('rb') as src, (mount / 'well_sender.uf2').open('wb') as dst:
        shutil.copyfileobj(src, dst)
        dst.flush()
        os.fsync(dst.fileno())


def upload_receiver(root, build, port, expected_mac=None):
    if not port:
        raise RuntimeError(
            'Receiver upload port is not configured; set RECEIVER_UPLOAD_PORT or upload.local.json'
        )
    mac = (
        expected_mac.strip().lower()
        if expected_mac is not None
        else resolve_setting(root, 'RECEIVER_MAC', 'receiver_mac').lower()
    )
    if not mac or mac == 'xx:xx:xx:xx:xx:xx':
        raise RuntimeError(
            'RECEIVER_MAC is not configured; set RECEIVER_MAC or upload.local.json'
        )
    files = {'0x0': 'bootloader.bin', '0x8000': 'partitions.bin', '0x10000': 'firmware.bin'}
    flash = json.loads((build / 'flasher_args.json').read_text())
    if set(flash['flash_files']) != set(files):
        raise RuntimeError('Unexpected flash offsets; refusing to flash')
    for name in files.values():
        if not (build / name).is_file():
            raise RuntimeError('Missing artifact: ' + name)
    base = [sys.executable, '-m', 'esptool', '--chip', 'esp32s3', '--port', port,
            '--before', 'default-reset']
    identity = subprocess.run(base + ['--after', 'no-reset', 'read-mac'], check=True,
                              capture_output=True, text=True, timeout=45)
    if mac not in identity.stdout.lower():
        raise RuntimeError('Wrong receiver; refusing to flash: ' + identity.stdout)
    backups = root / '.backups'
    backups.mkdir(exist_ok=True)
    (backups / 'receiver-identity.txt').write_text(identity.stdout)
    backup = backups / 'receiver-before.bin'
    if not backup.exists():
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
