# A WebSocket client in the standard library, for binding-crossing-test.sh: opens the
# connection, sends the given messages as text frames and prints what comes back, one line per
# message, then either closes cleanly (prints the close reply) or drops the TCP connection.
# Usage: ws-client.py <port> <path> clean|abrupt <message>...
import base64
import os
import socket
import struct
import sys

port, path, mode = int(sys.argv[1]), sys.argv[2], sys.argv[3]
messages = sys.argv[4:]
s = socket.create_connection(("127.0.0.1", port), timeout=30)
key = base64.b64encode(os.urandom(16)).decode()
s.sendall(
    (
        f"GET {path} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\n"
        f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n"
    ).encode()
)
buf = b""
while b"\r\n\r\n" not in buf:
    chunk = s.recv(4096)
    if not chunk:
        print("handshake: connection closed")
        sys.exit(1)
    buf += chunk
head, buf = buf.split(b"\r\n\r\n", 1)
status = head.split(b"\r\n", 1)[0].decode()
if " 101 " not in status:
    print(f"handshake: {status}")
    sys.exit(1)


def recv_exact(n):
    global buf
    while len(buf) < n:
        chunk = s.recv(65536)
        if not chunk:
            raise EOFError("connection closed")
        buf += chunk
    out, buf = buf[:n], buf[n:]
    return out


def send_frame(opcode, payload):
    mask = os.urandom(4)
    head = bytes([0x80 | opcode])
    n = len(payload)
    if n < 126:
        head += bytes([0x80 | n])
    elif n < 65536:
        head += bytes([0x80 | 126]) + struct.pack(">H", n)
    else:
        head += bytes([0x80 | 127]) + struct.pack(">Q", n)
    s.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))


def recv_frame():
    b0, b1 = recv_exact(2)
    opcode = b0 & 0x0F
    n = b1 & 0x7F
    if n == 126:
        (n,) = struct.unpack(">H", recv_exact(2))
    elif n == 127:
        (n,) = struct.unpack(">Q", recv_exact(8))
    if b1 & 0x80:
        mask = recv_exact(4)
        payload = bytes(b ^ mask[i % 4] for i, b in enumerate(recv_exact(n)))
    else:
        payload = recv_exact(n)
    return opcode, payload


for m in messages:
    send_frame(1, m.encode())
    opcode, payload = recv_frame()
    print(f"{opcode}:{payload.decode(errors='replace')}")
if mode == "clean":
    send_frame(8, struct.pack(">H", 1000) + b"done")
    opcode, payload = recv_frame()
    code = struct.unpack(">H", payload[:2])[0]
    print(f"close:{code}:{payload[2:].decode()}")
    s.close()
else:
    # Gone without a close frame: the server's pump of this connection ends on its own.
    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    s.close()
    print("dropped")
