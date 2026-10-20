"""Headless Microsoft Edge driven over the DevTools protocol, for the screen tests and captures.

On this PC `msedge.exe` hands the work to another process and returns at once, so --dump-dom prints nothing; the
DevTools port (written to DevToolsActivePort in the profile) works. Standard library only: a minimal WebSocket client.
"""
import base64
import json
import os
import shutil
import socket
import struct
import subprocess
import tempfile
import time
import urllib.request
from pathlib import Path

EDGE = next((path for path in (Path(r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"),
                               Path(r"C:\Program Files\Microsoft\Edge\Application\msedge.exe")) if path.is_file()), None)


class WebSocket:
    def __init__(self, url, timeout=30):
        rest = url.split("://", 1)[1]
        host_port, path = rest.split("/", 1)
        host, port = host_port.split(":")
        self.sock = socket.create_connection((host, int(port)), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        self.sock.sendall((f"GET /{path} HTTP/1.1\r\nHost: {host_port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                           f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode("ascii"))
        answer = b""
        while b"\r\n\r\n" not in answer:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("websocket handshake failed")
            answer += chunk
        if b" 101 " not in answer.split(b"\r\n", 1)[0]:
            raise ConnectionError(answer.split(b"\r\n", 1)[0].decode("latin-1"))
        self.buffer = answer.split(b"\r\n\r\n", 1)[1]

    def send(self, text):
        payload = text.encode("utf-8")
        header = bytearray([0x81])
        if len(payload) < 126:
            header.append(0x80 | len(payload))
        elif len(payload) < 65536:
            header += bytes([0x80 | 126]) + struct.pack(">H", len(payload))
        else:
            header += bytes([0x80 | 127]) + struct.pack(">Q", len(payload))
        mask = os.urandom(4)
        self.sock.sendall(bytes(header) + mask + bytes(byte ^ mask[i % 4] for i, byte in enumerate(payload)))

    def _read(self, count):
        while len(self.buffer) < count:
            chunk = self.sock.recv(1 << 16)
            if not chunk:
                raise ConnectionError("websocket closed")
            self.buffer += chunk
        data, self.buffer = self.buffer[:count], self.buffer[count:]
        return data

    def receive(self):
        message = b""
        while True:
            first, second = self._read(2)
            length = second & 0x7F
            if length == 126:
                length = struct.unpack(">H", self._read(2))[0]
            elif length == 127:
                length = struct.unpack(">Q", self._read(8))[0]
            payload = self._read(length)
            opcode = first & 0x0F
            if opcode in (0x1, 0x0, 0x2):
                message += payload
                if first & 0x80:
                    return message.decode("utf-8", "replace")
            elif opcode == 0x8:
                raise ConnectionError("websocket closed")

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


class HeadlessEdge:
    """One headless Edge with its own profile; page(url) gives a tab to read text from or capture."""

    def __init__(self, width=1440, height=900):
        if EDGE is None:
            raise FileNotFoundError("Microsoft Edge is not installed")
        self.profile = Path(tempfile.mkdtemp(prefix="boccia-edge-"))
        subprocess.Popen([str(EDGE), "--headless=new", "--disable-gpu", "--no-first-run", "--no-default-browser-check",
                          f"--user-data-dir={self.profile}", "--remote-debugging-port=0", f"--window-size={width},{height}",
                          "about:blank"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        port_file = self.profile / "DevToolsActivePort"
        deadline = time.monotonic() + 30
        while not (port_file.exists() and port_file.read_text().strip()):
            if time.monotonic() > deadline:
                raise TimeoutError("headless Edge did not start")
            time.sleep(0.1)
        self.port = int(port_file.read_text().split()[0])
        self.version = json.loads(urllib.request.urlopen(f"http://127.0.0.1:{self.port}/json/version", timeout=10).read())
        self.browser = WebSocket(self.version["webSocketDebuggerUrl"])
        self.next_id = 0

    def _call(self, socket_, method, params=None, session=None):
        self.next_id += 1
        message = {"id": self.next_id, "method": method, "params": params or {}}
        if session:
            message["sessionId"] = session
        socket_.send(json.dumps(message))
        while True:
            reply = json.loads(socket_.receive())
            if reply.get("id") == self.next_id:
                if "error" in reply:
                    raise RuntimeError(f"{method}: {reply['error']}")
                return reply.get("result", {})

    def open(self, url, width=1440, height=900):
        """A new tab at url; returns its session id."""
        target = self._call(self.browser, "Target.createTarget", {"url": "about:blank", "newWindow": False})["targetId"]
        session = self._call(self.browser, "Target.attachToTarget", {"targetId": target, "flatten": True})["sessionId"]
        self._call(self.browser, "Emulation.setDeviceMetricsOverride", {"width": width, "height": height, "deviceScaleFactor": 1, "mobile": False}, session)
        self._call(self.browser, "Page.enable", {}, session)
        self._call(self.browser, "Page.navigate", {"url": url}, session)
        return session

    def evaluate(self, session, expression):
        result = self._call(self.browser, "Runtime.evaluate", {"expression": expression, "returnByValue": True}, session)
        return result.get("result", {}).get("value")

    def text(self, url, wait_for="", timeout=15):
        """The page's text once it contains wait_for (or after timeout)."""
        session = self.open(url)
        deadline = time.monotonic() + timeout
        text = ""
        while time.monotonic() < deadline:
            text = self.evaluate(session, "document.documentElement ? document.documentElement.innerText : ''") or ""
            if wait_for and wait_for in text:
                break
            time.sleep(0.25)
        return text

    def capture(self, url, path, wait_for="", timeout=15, width=1440, height=900, transparent=False):
        session = self.open(url, width, height)
        if transparent:   # an icon: the page's empty background stays see-through
            self._call(self.browser, "Emulation.setDefaultBackgroundColorOverride", {"color": {"r": 0, "g": 0, "b": 0, "a": 0}}, session)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            text = self.evaluate(session, "document.documentElement ? document.documentElement.innerText : ''") or ""
            if not wait_for or wait_for in text:
                break
            time.sleep(0.25)
        time.sleep(0.5)   # fonts and the last paint
        data = self._call(self.browser, "Page.captureScreenshot", {"format": "png"}, session)["data"]
        Path(path).write_bytes(base64.b64decode(data))

    def close(self):
        try:
            self._call(self.browser, "Browser.close")
        except (OSError, ConnectionError, RuntimeError):
            pass
        self.browser.close()
        for _ in range(50):
            try:
                shutil.rmtree(self.profile)
                break
            except OSError:
                time.sleep(0.1)
