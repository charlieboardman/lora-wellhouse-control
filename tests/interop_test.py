"""PyModbus over loopback TCP through the production gateway engines and a fake RTU bus."""
from pathlib import Path
import socket
import struct
import subprocess
import threading
import unittest
from pymodbus.client import ModbusTcpClient

ROOT = Path(__file__).resolve().parents[1]


class Interoperability(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.host = subprocess.Popen([str(ROOT / '.build/tests/interop_host')],
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        cls.listener = socket.socket()
        cls.listener.bind(('127.0.0.1', 0))
        cls.listener.listen()
        cls.listener.settimeout(.2)
        cls.port = cls.listener.getsockname()[1]
        cls.stopping = threading.Event()
        cls.errors = []
        cls.thread = threading.Thread(target=cls.serve, daemon=True)
        cls.thread.start()

    @classmethod
    def serve(cls):
        try:
            while not cls.stopping.is_set():
                try:
                    conn, _ = cls.listener.accept()
                except socket.timeout:
                    continue
                with conn:
                    conn.settimeout(2)
                    buffer = b''
                    while True:
                        chunk = conn.recv(1024)
                        if not chunk:
                            break
                        buffer += chunk
                        while len(buffer) >= 6:
                            size = 6 + int.from_bytes(buffer[4:6], 'big')
                            if len(buffer) < size:
                                break
                            cls.host.stdin.write(buffer[:size].hex() + '\n')
                            cls.host.stdin.flush()
                            answer = cls.host.stdout.readline()
                            if not answer:
                                raise RuntimeError('Native gateway exited unexpectedly')
                            conn.sendall(bytes.fromhex(answer.strip()))
                            buffer = buffer[size:]
        except Exception as exc:
            cls.errors.append(exc)

    @classmethod
    def tearDownClass(cls):
        cls.stopping.set()
        cls.thread.join(3)
        cls.listener.close()
        cls.host.stdin.close()
        cls.host.wait(timeout=3)
        cls.host.stdout.close()
        if cls.errors:
            raise cls.errors[0]
        if cls.host.returncode:
            raise RuntimeError(f'Native gateway exited with {cls.host.returncode}')

    def setUp(self):
        self.client = ModbusTcpClient('127.0.0.1', port=self.port, timeout=3, retries=0)
        self.assertTrue(self.client.connect())
        self.addCleanup(self.client.close)

    def test_registers_and_device_routing(self):
        for unit in (1, 2):
            result = self.client.read_holding_registers(0, count=1, device_id=unit)
            self.assertFalse(result.isError())
            self.assertEqual(result.registers, [unit * 1000])
        self.assertFalse(self.client.write_register(5, 321, device_id=1).isError())
        self.assertEqual(self.client.read_holding_registers(5, count=1, device_id=1).registers, [321])

    def test_multi_write_and_multi_piece_read(self):
        values = [12, 34, 56]
        self.assertFalse(self.client.write_registers(10, values, device_id=2).isError())
        self.assertEqual(self.client.read_holding_registers(10, count=3, device_id=2).registers, values)
        result = self.client.read_holding_registers(0, count=125, device_id=1)
        self.assertFalse(result.isError())
        self.assertEqual(len(result.registers), 125)

    def test_rejected_oversize_and_broadcast(self):
        result = self.client.write_registers(0, [1] * 114, device_id=1)
        self.assertTrue(result.isError())
        self.assertEqual(result.exception_code, 3)
        result = self.client.read_holding_registers(0, count=1, device_id=0)
        self.assertTrue(result.isError())
        self.assertEqual(result.exception_code, 3)

    def test_device_exception_and_missing_device(self):
        result = self.client.read_holding_registers(1000, count=1, device_id=1)
        self.assertTrue(result.isError())
        self.assertEqual(result.exception_code, 2)
        result = self.client.read_holding_registers(0, count=1, device_id=3)
        self.assertTrue(result.isError())
        self.assertEqual(result.exception_code, 11)

    def test_vendor_function_and_split_tcp_input(self):
        # Close PyModbus's connection: the test server intentionally serializes clients.
        self.client.close()
        request = struct.pack('>HHHBB', 42, 0, 2, 1, 0x41)
        with socket.create_connection(('127.0.0.1', self.port), timeout=3) as conn:
            conn.sendall(request[:3])
            conn.sendall(request[3:])
            response = b''
            while len(response) < 260:
                response += conn.recv(260 - len(response))
        self.assertEqual(response[:6], struct.pack('>HHH', 42, 0, 254))
        self.assertEqual(response[6:8], bytes([1, 0x41]))
        self.assertEqual(response[8:], bytes(range(2, 254)))


if __name__ == '__main__':
    unittest.main()
