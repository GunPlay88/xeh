import json
import os
import socket
import struct
import tempfile
import threading
import unittest

import xeh


HEADER = struct.Struct("!IHHHHIII")
MAGIC = 0x58454831


def recv_exact(sock, size):
    result = b""
    while len(result) < size:
        chunk = sock.recv(size - len(result))
        if not chunk:
            raise EOFError("peer disconnected")
        result += chunk
    return result


def recv_frame(sock):
    magic, major, minor, opcode, flags, seq, obj, length = HEADER.unpack(
        recv_exact(sock, HEADER.size))
    assert (magic, major, minor, flags) == (MAGIC, 1, 0, 0)
    assert length <= 1024
    return opcode, seq, obj, recv_exact(sock, length)


def send_frame(sock, opcode, sequence, obj, payload=b""):
    sock.sendall(HEADER.pack(MAGIC, 1, 0, opcode, 0, sequence, obj,
                             len(payload)) + payload)


class Hello(xeh.Extension):
    name = "PYTHON-TEST"
    version = (1, 0)
    event_count = 1

    @xeh.request(1)
    def greeting(self, request):
        name = request.string()
        self.connection.event(1, request.client_id, b"greeted")
        return {"message": "Hello, " + name}


class PythonTest(unittest.TestCase):
    def test_fake_host(self):
        errors = []
        handled = threading.Event()
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "socket")
            listener = socket.socket(socket.AF_UNIX)
            listener.bind(path)
            listener.listen(1)

            def host():
                try:
                    with listener.accept()[0] as peer:
                        peer.settimeout(5)
                        opcode, seq, obj, _ = recv_frame(peer)
                        self.assertEqual((opcode, seq, obj), (1, 1, 0))
                        send_frame(peer, 2, 1, 0, struct.pack("!HHHHI", 1, 0, 1, 0, 1048576))
                        opcode, seq, obj, payload = recv_frame(peer)
                        self.assertEqual((opcode, obj), (0x100, 0))
                        self.assertEqual(payload[24:], b"PYTHON-TEST")
                        self.assertEqual(struct.unpack("!H", payload[:2])[0], 11)
                        send_frame(peer, 0x101, seq, 42, struct.pack("!IHHQ", 42, 0, 0, 0))
                        send_frame(peer, 0x200, 100, 42,
                                   struct.pack("!HHII", 1, 0, 7, 0) + b"Ada\0")
                        frames = [recv_frame(peer), recv_frame(peer)]
                        reply = next(item for item in frames if item[0] == 0x201)
                        event = next(item for item in frames if item[0] == 0x203)
                        self.assertEqual(reply[1:3], (100, 42))
                        self.assertEqual(json.loads(reply[3]), {"message": "Hello, Ada"})
                        self.assertEqual(event[3][:12], struct.pack("!HHII", 1, 0, 7, 0))
                        self.assertEqual(event[3][12:], b"greeted")
                        send_frame(peer, 0x200, 101, 42,
                                   struct.pack("!HHII", 1, 0, 7, 0) + b"\xff")
                        opcode, seq, obj, payload = recv_frame(peer)
                        self.assertEqual((opcode, seq, obj), (0x202, 101, 42))
                        self.assertEqual(struct.unpack("!HHI", payload), (1, 0, 0))
                        handled.set()
                        opcode, seq, obj, payload = recv_frame(peer)
                        self.assertEqual((opcode, obj, payload), (0x102, 42, b""))
                        send_frame(peer, 0x102, seq, 0)
                except BaseException as error:
                    errors.append(error)
                finally:
                    listener.close()

            thread = threading.Thread(target=host, daemon=True)
            thread.start()
            with xeh.Connection(path) as connection:
                extension = Hello()
                connection.register(extension)
                connection.wait_registered()
                self.assertEqual(connection.extension_id, 42)
                self.assertEqual(connection.capabilities, 0)
                for _ in range(20):
                    try:
                        connection.step(0.1)
                    except xeh.XEHError:
                        if errors:
                            break
                        raise
                    if handled.is_set() or errors:
                        break
                if errors:
                    raise errors[0]
                self.assertTrue(handled.is_set())
            thread.join(5)
            self.assertFalse(thread.is_alive())
            if errors:
                raise errors[0]


if __name__ == "__main__":
    unittest.main()
