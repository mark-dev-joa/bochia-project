"""Boccia Hub server.

Apps (labeler, tracker) register with a per-worker token, then send status,
events and files and poll for commands. The control API under /api/ has no
token: the hub only listens on localhost and the Tailscale address.
See design/hub/DESIGN.md.
"""
import argparse
import base64
import hashlib
import json
import os
import re
import secrets
import subprocess
import sys
import threading
import time
from datetime import datetime, timedelta
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlsplit

CONNECTED_SECONDS = 15
BODY_LIMIT = 50 * 1024 * 1024
LONGPOLL_MAX = 60
FILE_KINDS = ("log", "screen")
FILE_NAME = re.compile(r"^[A-Za-z0-9._-]+$")
FOLDER_UNSAFE = re.compile(r'[<>:"/\\|?*\x00-\x1f]')
CONTENT_TYPES = {".png": "image/png", ".jpg": "image/jpeg", ".jpeg": "image/jpeg",
                 ".log": "text/plain; charset=utf-8", ".txt": "text/plain; charset=utf-8",
                 ".json": "application/json; charset=utf-8"}


def now():
    return datetime.now().isoformat(timespec="milliseconds")


def token_hash(token):
    return hashlib.sha256(token.encode("utf-8")).hexdigest()


def folder_name(worker_id):
    return FOLDER_UNSAFE.sub("_", worker_id)


def valid_name(text):
    return isinstance(text, str) and 0 < len(text) <= 64 and text == text.strip() and "@" not in text and not FOLDER_UNSAFE.search(text)


class HubError(Exception):
    def __init__(self, status, text):
        super().__init__(text)
        self.status = status


class Hub:
    def __init__(self, storage, say=None):
        self.storage = Path(storage)
        self.say = say or (lambda text: None)
        self.lock = threading.RLock()
        self.wake = threading.Condition(self.lock)
        self.closing = False
        self.started = now()
        self.servers = []
        for folder in ("status", "events") + FILE_KINDS:
            (self.storage / folder).mkdir(parents=True, exist_ok=True)
        self.workers = self._read_json("workers.json", {})
        self.tokens = self._read_json("tokens.json", {})
        self.sessions = {}      # token hash -> id of the latest registration with that token
        self.status = {}
        self.seen = {}          # worker id -> {(time, text)} already stored, loaded on first use
        self.clock = {}         # worker id -> last received stamp handed out
        self.polling = {}       # worker id -> long polls in progress
        self.commands = {}      # command id -> record, in creation order
        for worker_id, entry in sorted(self.workers.items(), key=lambda item: item[1].get("registered", "")):
            if entry.get("token_hash"):
                self.sessions[entry["token_hash"]] = worker_id
            try:
                self.status[worker_id] = json.loads((self.storage / "status" / (folder_name(worker_id) + ".json")).read_text("utf-8"))
            except (OSError, ValueError):
                pass
        self._load_commands()

    # ---- storage

    def _read_json(self, name, default):
        try:
            return json.loads((self.storage / name).read_text("utf-8"))
        except FileNotFoundError:
            return default

    def _write_json(self, name, data):
        path = self.storage / name
        temp = path.with_name(path.name + ".tmp")
        temp.write_text(json.dumps(data, ensure_ascii=False, indent=1), "utf-8")
        os.replace(temp, path)

    def _load_commands(self):
        path = self.storage / "commands.jsonl"
        if not path.exists():
            return
        with path.open("r", encoding="utf-8") as file:
            for line in file:
                try:
                    record = json.loads(line)
                except ValueError:
                    continue
                if isinstance(record, dict) and "id" in record:
                    self.commands[record["id"]] = record    # the last line of an id is its latest state

    def _store_command(self, record):
        self.commands[record["id"]] = record
        with (self.storage / "commands.jsonl").open("a", encoding="utf-8") as file:
            file.write(json.dumps(record, ensure_ascii=False) + "\n")

    def _read_events(self, worker_id):
        folder = self.storage / "events" / folder_name(worker_id)
        records = []
        if folder.is_dir():
            for path in sorted(folder.glob("*.jsonl")):
                with path.open("r", encoding="utf-8") as file:
                    for line in file:
                        try:
                            record = json.loads(line)
                        except ValueError:
                            continue
                        if isinstance(record, dict):
                            records.append(record)
        return records

    def _seen(self, worker_id):
        seen = self.seen.get(worker_id)
        if seen is None:
            seen = set()
            for record in self._read_events(worker_id):
                seen.add((record.get("time", ""), record.get("text", "")))
                try:
                    received = datetime.fromisoformat(record.get("received", ""))
                except ValueError:
                    continue
                if worker_id not in self.clock or received > self.clock[worker_id]:
                    self.clock[worker_id] = received
            self.seen[worker_id] = seen
        return seen

    def _stamp(self, worker_id):
        """A received time that is unique and increasing per worker, so paging with `since` never skips or repeats a line."""
        stamp = datetime.now()
        stamp = stamp.replace(microsecond=stamp.microsecond // 1000 * 1000)
        last = self.clock.get(worker_id)
        if last is not None and stamp <= last:
            stamp = last + timedelta(milliseconds=1)
        self.clock[worker_id] = stamp
        return stamp.isoformat(timespec="milliseconds")

    # ---- workers and tokens

    def invite(self, worker):
        if not valid_name(worker):
            raise HubError(400, "worker name is required")
        token = base64.urlsafe_b64encode(secrets.token_bytes(32)).decode("ascii").rstrip("=")
        with self.lock:
            self.tokens = {hash_: name for hash_, name in self.tokens.items() if name != worker}
            self.tokens[token_hash(token)] = worker
            self._write_json("tokens.json", self.tokens)
        return token

    def register(self, hash_, body):
        worker, pc = body.get("worker"), body.get("pc")
        if not (valid_name(worker) and valid_name(pc)):
            raise HubError(400, "worker and pc are required")
        with self.lock:
            owner = self.tokens.get(hash_)
            if owner is None:
                raise HubError(401, "unknown token")
            if owner != worker:
                raise HubError(403, "token belongs to another worker")
            worker_id = f"{worker}@{pc}"
            entry = self.workers.setdefault(worker_id, {"worker": worker, "pc": pc, "since": ""})
            for key in ("app", "version", "account", "device", "summary"):
                if key in body:
                    entry[key] = body[key]
            entry.update({"token_hash": hash_, "registered": now(), "last_seen": now(), "session": str(body.get("session", ""))})
            self.sessions[hash_] = worker_id
            self._write_json("workers.json", self.workers)
            self.wake.notify_all()      # a poll of an older session of this worker ends now
        self.say(f"{worker_id} 접속 · {entry.get('app', '')} {entry.get('version', '')}".rstrip())
        return {"ok": True, "id": worker_id, "since": entry.get("since", "")}

    def resolve(self, hash_, pc=""):
        """The worker id behind a token: the PC named by the request, else the PC that registered last with this token."""
        with self.lock:
            owner = self.tokens.get(hash_)
            if owner is None:
                raise HubError(401, "unknown token")
            worker_id = f"{owner}@{pc}" if pc else self.sessions.get(hash_)
            entry = self.workers.get(worker_id)
            if entry is None or entry.get("worker") != owner:
                raise HubError(403, "register first")
            entry["last_seen"] = now()
            return worker_id

    def touch(self, worker_id):
        with self.lock:
            if worker_id in self.workers:
                self.workers[worker_id]["last_seen"] = now()

    def worker_list(self):
        threshold = datetime.now() - timedelta(seconds=CONNECTED_SECONDS)
        with self.lock:
            result = []
            for worker_id, entry in self.workers.items():
                try:
                    recent = datetime.fromisoformat(entry.get("last_seen", "")) >= threshold
                except ValueError:
                    recent = False
                result.append({"id": worker_id, "worker": entry.get("worker", ""), "pc": entry.get("pc", ""),
                               "app": entry.get("app", ""), "version": entry.get("version", ""), "account": entry.get("account", ""),
                               "connected": recent or self.polling.get(worker_id, 0) > 0,
                               "last_seen": entry.get("last_seen", ""), "since": entry.get("since", ""),
                               "device": entry.get("device"), "summary": entry.get("summary"),
                               "status": self.status.get(worker_id, {})})
            return result

    def summary(self):
        workers = self.worker_list()
        return {"workers": len(workers), "connected": sum(1 for worker in workers if worker["connected"]),
                "storage": str(self.storage), "started": self.started}

    # ---- status and events

    def set_status(self, worker_id, body):
        record = dict(body)
        record["received"] = now()
        with self.lock:
            self.status[worker_id] = record
            self._write_json(Path("status") / (folder_name(worker_id) + ".json"), record)

    def add_events(self, worker_id, events):
        if not isinstance(events, list):
            raise HubError(400, "events must be a list")
        with self.lock:
            seen = self._seen(worker_id)
            folder = self.storage / "events" / folder_name(worker_id)
            folder.mkdir(parents=True, exist_ok=True)
            stored, latest = 0, self.workers[worker_id].get("since", "")
            with (folder / (now()[:10] + ".jsonl")).open("a", encoding="utf-8") as file:
                for event in events:
                    if not isinstance(event, dict):
                        continue
                    time_, text = str(event.get("time", "")), str(event.get("text", ""))
                    if not text or (time_, text) in seen:
                        continue
                    seen.add((time_, text))
                    file.write(json.dumps({"time": time_, "received": self._stamp(worker_id), "text": text}, ensure_ascii=False) + "\n")
                    stored += 1
                    latest = max(latest, time_)
            if latest != self.workers[worker_id].get("since", ""):
                self.workers[worker_id]["since"] = latest
                self._write_json("workers.json", self.workers)
        return stored

    def events(self, worker_id, since="", limit=200):
        """Events in received order: after `since` the first `limit`, without it the last `limit` (`limit` <= 0 means all)."""
        with self.lock:
            records = self._read_events(worker_id)
        if since:
            records = [record for record in records if record.get("received", "") > since]
            return records[:limit] if limit > 0 else records
        return records[-limit:] if limit > 0 else records

    # ---- commands

    def queue_command(self, worker_id, name, value=""):
        name = str(name).strip()
        if not name:
            raise HubError(400, "command name is required")
        with self.lock:
            if worker_id not in self.workers:
                raise HubError(404, "unknown worker")
            cid = "c-" + secrets.token_hex(3)
            while cid in self.commands:
                cid = "c-" + secrets.token_hex(3)
            self._store_command({"id": cid, "worker": worker_id, "name": name, "value": value,
                                 "state": "queued", "result": "", "time": now(), "updated": now()})
            self.wake.notify_all()
        self.say(f"{worker_id} ← {name} {value}".rstrip())
        return cid

    def take_commands(self, worker_id, wait, session=""):
        """Queued commands for the worker, waiting up to `wait` seconds; a poll from a session the worker has since replaced gets 409."""
        deadline = time.monotonic() + wait
        with self.lock:
            self.polling[worker_id] = self.polling.get(worker_id, 0) + 1
            try:
                while True:
                    current = self.workers.get(worker_id, {}).get("session", "")
                    if session and current and session != current:
                        raise HubError(409, "another session registered")
                    pending = [record for record in self.commands.values() if record["worker"] == worker_id and record["state"] == "queued"]
                    remaining = deadline - time.monotonic()
                    if pending or remaining <= 0 or self.closing:
                        break
                    self.wake.wait(remaining)
                for record in pending:
                    record.update({"state": "sent", "updated": now()})
                    self._store_command(record)
            finally:
                self.polling[worker_id] -= 1
        self.touch(worker_id)
        return [{"id": record["id"], "name": record["name"], "value": record["value"]} for record in pending]

    def finish_command(self, worker_id, cid, body):
        with self.lock:
            record = self.commands.get(cid)
            if record is None or record["worker"] != worker_id:
                raise HubError(404, "unknown command")
            record.update({"state": "done" if body.get("ok") else "failed", "result": str(body.get("text", "")), "updated": now()})
            self._store_command(record)
        self.say(f"{worker_id} → {record['name']} {record['state']} {record['result']}".rstrip())

    def commands_of(self, worker_id):
        with self.lock:
            return [{key: record.get(key, "") for key in ("id", "name", "value", "state", "result", "time", "updated")}
                    for record in self.commands.values() if record["worker"] == worker_id]

    # ---- files

    def _file_folder(self, worker_id, kind):
        if kind not in FILE_KINDS:
            raise HubError(400, "kind must be log or screen")
        return self.storage / kind / folder_name(worker_id)

    def save_file(self, worker_id, kind, name, data):
        folder = self._file_folder(worker_id, kind)
        if not FILE_NAME.match(name) or not name.strip("."):
            raise HubError(400, "bad file name")
        folder.mkdir(parents=True, exist_ok=True)
        path = folder / name
        temp = folder / ("~" + name)     # no uploaded name starts with ~, so a part file is never listed
        temp.write_bytes(data)
        os.replace(temp, path)
        self.say(f"{worker_id} · {kind} {name} {len(data):,} B")
        return str(path)

    def files_of(self, worker_id, kind):
        folder = self._file_folder(worker_id, kind)
        if not folder.is_dir():
            return []
        result = []
        for path in sorted(folder.iterdir()):
            if path.is_file() and not path.name.startswith("~"):
                stat = path.stat()
                result.append({"name": path.name, "size": stat.st_size,
                               "time": datetime.fromtimestamp(stat.st_mtime).isoformat(timespec="milliseconds")})
        return result

    def file_path(self, worker_id, kind, name):
        path = self._file_folder(worker_id, kind) / name
        if not FILE_NAME.match(name) or not path.is_file():
            raise HubError(404, "no such file")
        return path

    # ---- serving

    def serve(self, bind, port):
        first = None
        for address in bind:
            server = Server((address, first.server_port if first else port), Handler)
            server.hub = self
            threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.5}, daemon=True, name=f"hub {address}").start()
            self.servers.append(server)
            first = first or server
        return first

    def shutdown(self):
        with self.lock:
            self.closing = True
            self.wake.notify_all()
        for server in self.servers:
            server.shutdown()
            server.server_close()
        self.servers = []


class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = sys.platform != "win32"     # on Windows SO_REUSEADDR would let a second hub bind the same port silently
    hub = None

    def handle_error(self, request, client_address):
        if not isinstance(sys.exc_info()[1], (ConnectionError, TimeoutError)):
            super().handle_error(request, client_address)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "BocciaHub/1"

    def log_message(self, format, *args):
        pass

    def do_GET(self):
        self._handle("GET")

    def do_POST(self):
        self._handle("POST")

    def _handle(self, method):
        hub = self.server.hub
        try:
            url = urlsplit(self.path)
            parts = [unquote(part) for part in url.path.split("/") if part]
            query = parse_qs(url.query)
            body = self._read_body() if method == "POST" else b""
            if parts and parts[0] == "api":
                self._control(hub, method, parts[1:], query, body)
            else:
                self._app(hub, method, parts, query, body)
        except HubError as error:
            self._json({"error": str(error)}, error.status)
        except Exception as error:      # noqa: BLE001 — keep serving, tell the caller what broke
            try:
                self._json({"error": f"{type(error).__name__}: {error}"}, 500)
            except OSError:
                pass

    def _app(self, hub, method, parts, query, body):
        if parts == ["ping"] and method == "GET":
            return self._json({"hub": "boccia", "time": now()})
        hash_ = self._token_hash()
        if parts == ["register"] and method == "POST":
            return self._json(hub.register(hash_, self._parse(body)))
        worker_id = hub.resolve(hash_, unquote(self.headers.get("X-Boccia-Pc", "")).strip())   # the PC name travels percent-encoded
        if parts == ["status"] and method == "POST":
            hub.set_status(worker_id, self._parse(body))
            return self._empty()
        if parts == ["events"] and method == "POST":
            return self._json({"stored": hub.add_events(worker_id, self._parse(body).get("events"))})
        if parts == ["commands"] and method == "GET":
            return self._json({"commands": hub.take_commands(worker_id, self._number(query, "wait", 0, LONGPOLL_MAX), self.headers.get("X-Boccia-Session", "").strip())})
        if len(parts) == 3 and parts[0] == "commands" and parts[2] == "result" and method == "POST":
            hub.finish_command(worker_id, parts[1], self._parse(body))
            return self._empty()
        if len(parts) == 3 and parts[0] == "files" and method == "POST":
            return self._json({"path": hub.save_file(worker_id, parts[1], parts[2], body)})
        raise HubError(404, "no such route")

    def _control(self, hub, method, parts, query, body):
        if parts == ["status"] and method == "GET":
            return self._json(hub.summary())
        if parts == ["workers"] and method == "GET":
            return self._json(hub.worker_list())
        if parts == ["invites"] and method == "POST":
            return self._json({"token": hub.invite(self._parse(body).get("worker"))})
        if len(parts) >= 3 and parts[0] == "workers":
            worker_id, rest = parts[1], parts[2:]
            if worker_id not in hub.workers:
                raise HubError(404, "unknown worker")
            if rest == ["events"] and method == "GET":
                return self._json(hub.events(worker_id, query.get("since", [""])[0], self._number(query, "limit", 200)))
            if rest == ["commands"] and method == "GET":
                return self._json(hub.commands_of(worker_id))
            if rest == ["commands"] and method == "POST":
                request = self._parse(body)
                return self._json({"id": hub.queue_command(worker_id, request.get("name", ""), request.get("value", ""))})
            if len(rest) == 2 and rest[0] == "files" and method == "GET":
                return self._json(hub.files_of(worker_id, rest[1]))
            if len(rest) == 3 and rest[0] == "files" and method == "GET":
                return self._file(hub.file_path(worker_id, rest[1], rest[2]))
        raise HubError(404, "no such route")

    # ---- request pieces

    def _read_body(self):
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            raise HubError(400, "bad Content-Length") from None
        if length > BODY_LIMIT:
            self.close_connection = True
            raise HubError(413, "body too large")
        return self.rfile.read(length) if length > 0 else b""

    def _token_hash(self):
        header = self.headers.get("Authorization", "")
        if not header.startswith("Bearer ") or not header[7:].strip():
            raise HubError(401, "token required")
        return token_hash(header[7:].strip())

    @staticmethod
    def _parse(body):
        try:
            data = json.loads(body.decode("utf-8")) if body else {}
        except ValueError:
            raise HubError(400, "body is not JSON") from None
        if not isinstance(data, dict):
            raise HubError(400, "body must be a JSON object")
        return data

    @staticmethod
    def _number(query, key, default, maximum=None):
        try:
            value = int(query.get(key, [default])[0])
        except ValueError:
            raise HubError(400, f"{key} must be a number") from None
        return min(value, maximum) if maximum is not None else value

    # ---- responses

    def _json(self, data, status=200):
        raw = json.dumps(data, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def _empty(self):
        self.send_response(204)
        self.end_headers()

    def _file(self, path):
        data = path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", CONTENT_TYPES.get(path.suffix.lower(), "application/octet-stream"))
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


# ---- command line

def tailscale_ip():
    for exe in ("tailscale", r"C:\Program Files\Tailscale\tailscale.exe"):
        try:
            result = subprocess.run([exe, "ip", "-4"], capture_output=True, text=True, timeout=5)
        except (OSError, subprocess.SubprocessError):
            continue
        if result.returncode == 0 and result.stdout.split():
            return result.stdout.split()[0]
    return ""


def bind_addresses(choice):
    if choice == "all":
        return ["0.0.0.0"]
    if choice == "auto":
        addresses = ["127.0.0.1"]
        ip = tailscale_ip()
        if ip:
            addresses.append(ip)
        return addresses
    return [address.strip() for address in choice.split(",") if address.strip()]


def main(argv=None):
    parser = argparse.ArgumentParser(prog="hub.py")
    commands = parser.add_subparsers(dest="command", required=True)
    serve = commands.add_parser("serve", help="run the hub")
    serve.add_argument("--port", type=int, default=8790)
    serve.add_argument("--storage", default=r"D:\Boccia Hub")
    serve.add_argument("--bind", default="auto", help="auto (localhost + Tailscale), all, or addresses separated by commas")
    serve.add_argument("--print-port", action="store_true", help="print 'port <n>' first")
    args = parser.parse_args(argv)
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    hub = Hub(args.storage, say=lambda text: print(f"{datetime.now():%H:%M:%S} {text}", flush=True))
    first = None
    for address in bind_addresses(args.bind):
        try:
            server = hub.serve([address], first.server_port if first else args.port)
        except OSError as error:
            print(f"{address}:{args.port} 묶기 실패 · {error}", file=sys.stderr, flush=True)
            if first is None:
                return 1
            continue
        if first is None:
            first = server
            if args.print_port:
                print(f"port {server.server_port}", flush=True)
        print(f"허브 http://{address}:{server.server_port} · 저장소 {hub.storage}", flush=True)
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass
    finally:
        hub.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())
