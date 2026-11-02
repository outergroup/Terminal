#!/usr/bin/env python3
"""Exercises TerminalBackend over HTTP and WebSockets on a Unix socket.

Starts a backend with its own socket, creates a session, types into it,
disconnects, resumes from the last output position, checks that only the
missed output arrives, and closes the session. It also starts sessions in a
container, served by a stand-in Outer Shell registry and a stand-in `docker`
that runs the container's commands on this computer.

    python3 Scripts/test_backend.py [path/to/TerminalBackend]

To test an installed Terminal instead, set TERMINAL_SOCKET to its socket and
TERMINAL_RESTART to a shell command that restarts it.
"""
import base64
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def build():
    out = os.path.join(tempfile.mkdtemp(prefix="terminal-test."), "TerminalBackend")
    sources = [os.path.join(ROOT, "Backend", name) for name in ("main.c", "Session.c", "Keeper.c", "Places.c", "Registry.c")]
    subprocess.run(["cc", "-std=gnu17", "-O1", *sources, "-o", out, "-lpthread"], check=True)
    return out


INSTALLED = os.environ.get("TERMINAL_SOCKET")
BACKEND = None if INSTALLED else (sys.argv[1] if len(sys.argv) > 1 else build())
WORK = tempfile.mkdtemp(prefix="tt.", dir="/tmp")
SOCKET = INSTALLED or os.path.join(WORK, "terminal.sock")
os.makedirs(os.path.join(WORK, "web"))
with open(os.path.join(WORK, "web", "index.html"), "w") as f:
    f.write("<!doctype html><title>Terminal</title>")


def check(name, condition):
    print(("ok   " if condition else "FAIL ") + name)
    if not condition:
        sys.exit(1)


def connect():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCKET)
    s.settimeout(10)
    return s


def http(method, path, headers=""):
    s = connect()
    s.sendall(f"{method} {path} HTTP/1.1\r\nHost: localhost\r\n{headers}Content-Length: 0\r\n\r\n".encode())
    data = b""
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        data += chunk
    s.close()
    head, _, body = data.partition(b"\r\n\r\n")
    return int(head.split(b" ")[1]), head.decode(), body


class WebSocket:
    def __init__(self, path, origin=None):
        self.sock = connect()
        key = base64.b64encode(os.urandom(16)).decode()
        extra = f"Origin: {origin}\r\n" if origin else ""
        self.sock.sendall((f"GET {path} HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                           f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n{extra}\r\n").encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self.sock.recv(1)
            if not chunk:
                break
            head += chunk
        self.status = int(head.split(b" ")[1]) if head else 0
        self.buffer = b""
        self.output = b""
        self.position = None
        self.ended = False

    def send(self, payload):
        mask = os.urandom(4)
        header = bytes([0x82])
        length = len(payload)
        header += bytes([0x80 | length]) if length < 126 else bytes([0x80 | 126]) + struct.pack(">H", length)
        self.sock.sendall(header + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

    def type(self, text):
        self.send(b"I" + text.encode())

    def read_until(self, needle, timeout=10):
        end = time.time() + timeout
        while needle.encode() not in self.output and time.time() < end and not self.ended:
            self.sock.settimeout(max(0.1, end - time.time()))
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                break
            self.buffer += chunk
            while len(self.buffer) >= 2:
                length = self.buffer[1] & 0x7f
                offset = 2
                if length == 126:
                    length, offset = struct.unpack(">H", self.buffer[2:4])[0], 4
                elif length == 127:
                    length, offset = struct.unpack(">Q", self.buffer[2:10])[0], 10
                if len(self.buffer) < offset + length:
                    break
                opcode, payload = self.buffer[0] & 0x0f, self.buffer[offset:offset + length]
                self.buffer = self.buffer[offset + length:]
                if opcode == 0x2 and payload[:1] == b"O":
                    self.output += payload[1:]
                    if self.position is not None:
                        self.position += len(payload) - 1
                elif opcode == 0x2 and payload[:1] == b"P":
                    self.position = struct.unpack("<Q", payload[1:9])[0]
                elif opcode == 0x2 and payload[:1] == b"X":
                    self.ended = True
        return needle.encode() in self.output

    def close(self):
        self.sock.close()


def sessions():
    status, _, body = http("GET", "/api/sessions")
    rows = body.decode().strip().split("\n")[1:]
    return [row.split("\t") for row in rows if row]


env = dict(os.environ, SHELL="/bin/sh")
outer_shell_home = os.path.join(WORK, "outershell")
os.makedirs(os.path.join(outer_shell_home, "bin"))
with open(os.path.join(outer_shell_home, "bin", "outerctl"), "w") as f:
    f.write("#!/bin/sh\necho outerctl-$((6*7))\n")
os.chmod(os.path.join(outer_shell_home, "bin", "outerctl"), 0o755)
env.update(OUTERSHELL_HOME=outer_shell_home)
CONTAINER = "c0ffee" * 10 + "c0de"


def serve_registry(path):
    """Answers Outer Shell's environment query with one running Docker container."""
    fields = ["test-container", "Test Container", "docker", CONTAINER, "running", "alpine", "", "", ""]
    strings = b"".join(field.encode() for field in fields)
    variable = 48 + 72
    payload = bytearray(b"OSED" + struct.pack("<IIIIIII", 1, variable + len(strings), 1, 72, 48, variable, 0))
    payload += struct.pack("<IIQ", variable, 0, 0)
    offset = variable
    record = b""
    for field in fields:
        record += struct.pack("<II", offset, len(field.encode()))
        offset += len(field.encode())
    payload += record + strings
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(path)
    listener.listen(8)

    def loop():
        while True:
            client, _ = listener.accept()
            client.recv(6)
            client.sendall(struct.pack("<I", len(payload)) + bytes(payload))
            client.close()
    threading.Thread(target=loop, daemon=True).start()


if not INSTALLED:
    bin_directory = os.path.join(WORK, "bin")
    commands = os.path.join(WORK, "commands.d")
    os.makedirs(bin_directory)
    os.makedirs(commands)
    with open(os.path.join(commands, "greet.outercommand"), "w") as f:
        f.write("[Command]\nFormat=1\nID=greet\nName=Greeter\nWorkingDirectory=/tmp\nUser=\nIconPath=\n"
                "Executable=/bin/sh\nArgument=-c\nArgument=echo greeting-$((6*7)) in $(pwd); exec sh\n")
    with open(os.path.join(bin_directory, "docker"), "w") as f:
        f.write(f"""#!/bin/sh
# Stands in for `docker exec`, running the command on this computer.
[ "$1" = exec ] || exit 2
shift
directory=
while [ $# -gt 0 ]; do
    case "$1" in
        -i|-it) shift ;;
        -e) export "$2"; shift 2 ;;
        -u) shift 2 ;;
        -w) directory=$2; shift 2 ;;
        *) break ;;
    esac
done
[ "$1" = {CONTAINER} ] || exit 3
shift
if [ "$1" = cat ]; then exec cat "{commands}/$(basename "$2")"; fi
[ -n "$directory" ] && cd "$directory"
exec "$@"
""")
    os.chmod(os.path.join(bin_directory, "docker"), 0o755)
    registry = os.path.join(WORK, "registry.sock")
    serve_registry(registry)
    env.update(PATH=bin_directory + ":" + os.environ.get("PATH", ""), OUTERSHELLD_API_SOCKET=registry)


def start_backend():
    if INSTALLED:
        return None
    return subprocess.Popen([BACKEND, "--socket-path", SOCKET, "--web-root", os.path.join(WORK, "web")], env=env)


def restart_backend(process):
    if INSTALLED:
        subprocess.run(os.environ["TERMINAL_RESTART"], shell=True, check=True)
        return None
    process.terminate()
    process.wait()
    return start_backend()


backend = start_backend()
try:
    for _ in range(50):
        if os.path.exists(SOCKET):
            break
        time.sleep(0.1)
    status, head, body = http("GET", "/")
    check("serves the page", status == 200 and b"Terminal" in body)
    if INSTALLED:
        status, _, script = http("GET", "/web/app.js")
        check("serves the app script", status == 200 and b"xterm" in script)
    status, head, body = http("GET", "/api/sessions")
    check("lists sessions as TSV with the home directory", status == 200 and "Terminal-Home:" in head and body.startswith(b"id\tcommand"))
    before = len(sessions())
    status, _, body = http("POST", "/api/sessions?columns=100&rows=30")
    session_id = body.decode().strip()
    check("starts a session", status == 201 and len(session_id) == 16)
    check("lists the new session", len(sessions()) == before + 1)
    status, _, _ = http("POST", "/api/sessions", "Origin: http://evil.example\r\n")
    check("rejects cross-origin requests", status == 403)
    rejected = WebSocket(f"/api/sessions/{session_id}/attach?columns=100&rows=30", origin="http://evil.example")
    check("rejects cross-origin terminals", rejected.status == 403)

    ws = WebSocket(f"/api/sessions/{session_id}/attach?columns=100&rows=30")
    check("attaches", ws.status == 101)
    time.sleep(1)
    ws.type("echo first-$((40+2))\r")
    check("runs commands", ws.read_until("first-42"))
    if not INSTALLED:
        ws.type("outerctl\r")
        check("finds outerctl on PATH", ws.read_until("outerctl-42"))
    ws.read_until("never", timeout=0.5)
    check("reports its output position", ws.position is not None)
    position = ws.position
    ws.type("sleep 1; echo missed-$((2+3))\r")
    ws.read_until("never", timeout=0.3)
    position = ws.position
    ws.close()
    time.sleep(2)

    resumed = WebSocket(f"/api/sessions/{session_id}/attach?columns=100&rows=30&from={position}")
    check("resume delivers only missed output", resumed.read_until("missed-5") and b"first-42" not in resumed.output)
    check("resume does not reset the screen", b"\x1bc" not in resumed.output)
    resumed.close()

    replay = WebSocket(f"/api/sessions/{session_id}/attach?columns=100&rows=30")
    check("a fresh page gets a replay", replay.read_until("missed-5") and b"first-42" in replay.output)
    replay.close()

    backend = restart_backend(backend)
    for _ in range(50):
        try:
            http("GET", "/")
            break
        except OSError:
            time.sleep(0.1)
    check("sessions outlive the backend", any(row[0] == session_id for row in sessions()))

    ending = WebSocket(f"/api/sessions/{session_id}/attach?columns=100&rows=30")
    ending.read_until("missed-5")
    ending.type("exit\r")
    ending.read_until("never", timeout=3)
    check("exit ends the session", ending.ended and not any(row[0] == session_id for row in sessions()))

    status, _, body = http("POST", "/api/sessions?columns=80&rows=24")
    other = body.decode().strip()
    status, _, _ = http("POST", f"/api/sessions/{other}/close")
    time.sleep(0.5)
    check("close ends a session", status == 204 and not any(row[0] == other for row in sessions()))

    if not INSTALLED:
        status, _, body = http("GET", "/api/places")
        check("lists running containers as places", status == 200 and "test-container\tTest Container" in body.decode())
        status, _, body = http("POST", f"/api/sessions?columns=80&rows=24&place=test-container")
        in_container = body.decode().strip()
        check("starts a session in a container", status == 201 and len(in_container) == 16)
        row = next((row for row in sessions() if row[0] == in_container), None)
        check("labels the session with its container", row is not None and row[7] == "Test Container")
        ws = WebSocket(f"/api/sessions/{in_container}/attach?columns=80&rows=24")
        time.sleep(1)
        ws.type("echo inside-$TERM\r")
        check("the container session runs with a terminal", ws.read_until("inside-xterm-256color"))
        ws.close()
        http("POST", f"/api/sessions/{in_container}/close")
        status, _, body = http("POST", f"/api/sessions?columns=80&rows=24&place=test-container&command=greet")
        command_session = body.decode().strip()
        check("starts a command registered in the container", status == 201)
        ws = WebSocket(f"/api/sessions/{command_session}/attach?columns=80&rows=24")
        check("runs it in its working directory", ws.read_until("greeting-42 in /tmp"))
        ws.close()
        row = next((row for row in sessions() if row[0] == command_session), None)
        check("names the session after the command", row is not None and row[1] == "Greeter")
        http("POST", f"/api/sessions/{command_session}/close")
        status, _, _ = http("POST", f"/api/sessions?columns=80&rows=24&place=test-container&command=missing")
        check("refuses commands the container does not register", status == 404)
        status, _, _ = http("POST", "/api/sessions?columns=80&rows=24&place=unknown")
        check("refuses unknown containers", status == 404)
    print("all passed")
finally:
    if backend:
        backend.terminate()
