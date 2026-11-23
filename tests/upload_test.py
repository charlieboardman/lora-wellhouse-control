"""Exercise upload guards without connecting to hardware."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('upload', Path(__file__).resolve().parents[1] / 'scripts/upload.py')
upload = importlib.util.module_from_spec(spec)
spec.loader.exec_module(upload)

TEST_MAC = 'aa:bb:cc:dd:ee:ff'


class UploadGuards(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.build = self.root / 'build'
        self.build.mkdir()
        for name in ('bootloader.bin', 'partitions.bin', 'firmware.bin', 'firmware.uf2'):
            (self.build / name).write_bytes(b'firmware')
        (self.build / 'flasher_args.json').write_text(json.dumps({'flash_files': {
            '0x0': 'bootloader.bin', '0x8000': 'partitions.bin', '0x10000': 'firmware.bin'}}))

    def test_pico_rejects_other_drive_without_writing(self):
        (self.root / 'INFO_UF2.TXT').write_text('Board-ID: RP2040')
        with self.assertRaisesRegex(RuntimeError, 'Not an RP2350'):
            upload.upload_sender(self.build, str(self.root))
        self.assertFalse((self.root / 'well_sender.uf2').exists())

    def test_pico_copies_built_firmware(self):
        (self.root / 'INFO_UF2.TXT').write_text('Board-ID: RP2350')
        upload.upload_sender(self.build, str(self.root))
        self.assertEqual((self.root / 'well_sender.uf2').read_bytes(), b'firmware')

    @patch.dict(os.environ, {}, clear=True)
    @patch.object(upload.subprocess, 'run')
    def test_missing_receiver_mac_refuses_to_flash(self, run):
        with self.assertRaisesRegex(RuntimeError, 'RECEIVER_MAC is not configured'):
            upload.upload_receiver(self.root, self.build, 'test-port')
        run.assert_not_called()

    @patch.object(upload.subprocess, 'run')
    def test_wrong_receiver_never_reads_or_writes_flash(self, run):
        run.return_value = subprocess.CompletedProcess([], 0, stdout='MAC: 00:00:00:00:00:00')
        with self.assertRaisesRegex(RuntimeError, 'Wrong receiver'):
            upload.upload_receiver(self.root, self.build, 'test-port', expected_mac=TEST_MAC)
        self.assertEqual(run.call_count, 1)

    @patch.object(upload.subprocess, 'run')
    def test_incomplete_backup_prevents_flash_write(self, run):
        (self.root / 'upload.local.json').write_text(json.dumps({'receiver_mac': TEST_MAC}))
        run.return_value = subprocess.CompletedProcess([], 0, stdout='MAC: ' + TEST_MAC)
        backups = self.root / '.backups'
        backups.mkdir()
        (backups / 'receiver-before.bin').write_bytes(b'incomplete')
        with self.assertRaisesRegex(RuntimeError, 'Incomplete receiver backup'):
            upload.upload_receiver(self.root, self.build, 'test-port')
        self.assertEqual(run.call_count, 1)

    @patch.object(upload.subprocess, 'run')
    def test_unexpected_offsets_prevent_device_access(self, run):
        (self.build / 'flasher_args.json').write_text('{"flash_files": {"0x20000": "firmware.bin"}}')
        with self.assertRaisesRegex(RuntimeError, 'Unexpected flash offsets'):
            upload.upload_receiver(self.root, self.build, 'test-port', expected_mac=TEST_MAC)
        run.assert_not_called()


if __name__ == '__main__':
    unittest.main()
