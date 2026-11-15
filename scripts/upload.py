"""PlatformIO upload actions, preserving board identity checks and receiver backup."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

RECEIVER_MAC = 'xx:xx:xx:xx:xx:xx'


def upload_sender(build, port):
    mount = Path(port)
    if 'Board-ID: RP2350' not in (mount / 'INFO_UF2.TXT').read_text():
        raise RuntimeError('Not an RP2350 BOOTSEL drive; refusing to flash')
    firmware = build / 'firmware.uf2'
    print('Installing', firmware, 'SHA256', hashlib.sha256(firmware.read_bytes()).hexdigest())
    with firmware.open('rb') as src, (mount / 'well_sender.uf2').open('wb') as dst:
        shutil.copyfileobj(src, dst)
        dst.flush()
        os.fsync(dst.fileno())


def upload_receiver(root, build, port):
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
    if RECEIVER_MAC not in identity.stdout.lower():
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
    port = env.subst('$UPLOAD_PORT')
    if env.subst('$PIOENV') == 'sender':
        upload_sender(build, port)
    else:
        upload_receiver(root, build, port)


if 'Import' in globals():
    Import('env')
    env.Replace(UPLOAD_PROTOCOL="custom", UPLOADCMD=upload)
