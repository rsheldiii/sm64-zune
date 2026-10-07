"""tools/receive_logs.py against a client that talks like the game (platform/zune_upload.cpp)."""
import http.client
from http.server import HTTPServer
import io
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from contextlib import redirect_stdout

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import receive_logs

PIECE = receive_logs.PIECE


class ReceiverTest(unittest.TestCase):
    def setUp(self):
        self.work = Path(tempfile.mkdtemp())
        self.receiver = receive_logs.Receiver(self.work / 'logs/zune', self.work)
        self.server = HTTPServer(('127.0.0.1', 0), receive_logs.handler(self.receiver))
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.printed = io.StringIO()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()

    def post(self, path, body=b'', length=None):
        connection = http.client.HTTPConnection('127.0.0.1', self.server.server_address[1], timeout=10)
        with redirect_stdout(self.printed):   # what the receiver prints while it answers
            connection.putrequest('POST', path)
            connection.putheader('Content-Type', 'application/octet-stream')
            connection.putheader('Content-Length', str(len(body) if length is None else length))
            connection.endheaders()
            connection.send(body)
            status = connection.getresponse().status
        connection.close()
        return status

    def send(self, run, name, data):
        for offset in range(0, len(data), PIECE):
            self.assertEqual(self.post(f'/sm64zune/{run}/{name}/{offset:x}', data[offset:offset + PIECE]), 200)

    def saved(self, suffix):
        files = sorted((self.work / 'logs/zune').glob('*' + suffix))
        return [file.read_bytes() for file in files]

    def test_a_run_arrives_whole(self):
        log = b'    1.000 build 20260101-000000; settings: ZUNE_LOG=1\r\n' + bytes(range(256)) * 700   # three pieces
        profile = b'\x01\x02\x03\x04' * 3 * 5000
        self.send('0000abcd', 'log.txt', log)
        self.send('0000abcd', 'profile.bin', profile)
        self.assertEqual(self.post('/sm64zune/0000abcd/done'), 200)
        self.assertEqual(self.saved('-log.txt'), [log])
        self.assertEqual(self.saved('-profile.bin'), [profile])
        printed = self.printed.getvalue()
        self.assertIn('Received build/logs/zune/', printed)
        self.assertIn('from build 20260101-000000', printed)
        self.assertIn('No crash in it.', printed)

    def test_a_repeated_piece_is_stored_once(self):
        data = b'a' * PIECE + b'b' * 100
        self.assertEqual(self.post('/sm64zune/00000001/log.txt/0', data[:PIECE]), 200)
        self.assertEqual(self.post('/sm64zune/00000001/log.txt/0', data[:PIECE]), 200)   # its answer was lost
        self.assertEqual(self.post(f'/sm64zune/00000001/log.txt/{PIECE:x}', data[PIECE:]), 200)
        self.assertEqual(self.post(f'/sm64zune/00000001/log.txt/{PIECE:x}', data[PIECE:]), 200)
        self.assertEqual(self.post('/sm64zune/00000001/done'), 200)
        self.assertEqual(self.saved('-log.txt'), [data])

    def test_two_runs_stay_apart(self):
        self.send('00000001', 'log.txt', b'first')
        self.send('00000002', 'log.txt', b'second')
        self.assertEqual(self.post('/sm64zune/00000001/done'), 200)
        self.assertEqual(self.post('/sm64zune/00000002/done'), 200)
        self.assertEqual(sorted(self.saved('-log.txt')), [b'first', b'second'])

    def test_a_crash_is_reported_even_without_its_map(self):
        log = (b'    1.000 build nosuchbuild; settings: ZUNE_LOG=1\r\n'
               b'    2.000 native exception 0xc0000005 during "game logic" (frame 7): pc 00012345 lr 00012300\r\n')
        self.send('00000003', 'log.txt', log)
        self.assertEqual(self.post('/sm64zune/00000003/done'), 200)
        self.assertIn('The game crashed in this log', self.printed.getvalue())

    def test_anything_else_is_refused(self):
        refused = {
            '/sm64zune/00000001/passwd/0': 404,
            '/sm64zune/00000001/../../log.txt/0': 404,
            '/sm64zune/00000001/log.txt/0/extra': 404,
            '/sm64zune/0000000g/log.txt/0': 404,
            '/other': 404,
            '/sm64zune/00000001/log.txt/10': 400,                 # not on a piece boundary
            f'/sm64zune/00000001/log.txt/{PIECE:x}': 409,         # the middle of a file nobody started
            '/sm64zune/00000009/done': 409,                       # a run nobody started
            f'/sm64zune/00000001/log.txt/{receive_logs.FILE_LIMIT:x}': 400,
        }
        for path, status in refused.items():
            self.assertEqual(self.post(path, b'data'), status, path)
        self.assertEqual(self.post('/sm64zune/00000001/log.txt/0', b''), 400)
        self.assertEqual(self.post('/sm64zune/00000001/log.txt/0', b'', length=PIECE + 1), 413)
        self.assertFalse((self.work / 'logs/zune').exists())

    def test_the_total_is_bounded(self):
        self.receiver.accepted = receive_logs.TOTAL_LIMIT - 10
        self.assertEqual(self.post('/sm64zune/00000001/log.txt/0', b'x' * 11), 507)
        self.assertEqual(self.post('/sm64zune/00000001/log.txt/0', b'x' * 10), 200)


if __name__ == '__main__':
    unittest.main()
