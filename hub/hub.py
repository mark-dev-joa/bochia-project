"""Boccia Hub server.

Apps (labeler, tracker) register with a per-worker token, then send status,
events and files and poll for commands. The control API under /api/ has no
token: the hub only listens on localhost and the Tailscale address.
See design/hub/DESIGN.md.
"""
import argparse
import base64
import bisect
import hashlib
import json
import os
import re
import secrets
import socket
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
                 ".json": "application/json; charset=utf-8", ".html": "text/html; charset=utf-8",
                 ".js": "text/javascript; charset=utf-8", ".css": "text/css; charset=utf-8", ".ttf": "font/ttf",
                 ".woff2": "font/woff2", ".svg": "image/svg+xml", ".ico": "image/x-icon"}
CACHE_LINES = 20_000      # activity kept in memory per worker; the files keep everything
# The kind of an activity line, by the words before its first " · " (design/hub/PLAN-2.md, Global Constraints).
ACTIONS = ("장면 확인", "확인", "보류", "공 추가", "공 수정", "공 삭제", "되돌리기", "이전 라벨 가져오기", "이전 적용", "전파 →")   # 이전 적용: its name in lines written before 2026-10-20
ERRORS = ("오류", "자동 초안 오류", "초안 실패", "임포트 복사 실패", "GPU 못 씀")
NOTICES = ("알림",)


def classify(text):
    """"action", "error", "notice" (the status bar's words, left out of the activity list) or "job"."""
    def starts(prefixes):
        return any(text == prefix or text.startswith(prefix + " ") for prefix in prefixes)
    if starts(ERRORS):
        return "error"
    if starts(NOTICES):
        return "notice"
    if starts(ACTIONS):
        return "action"
    return "job"


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
    def __init__(self, storage, say=None, control_from=(), ui=None):
        self.storage = Path(storage)
        self._echo = say or (lambda text: None)
        self.control_from = set(control_from)   # addresses allowed to use /api besides this PC (--control-from)
        self.ui = Path(ui) if ui else Path(__file__).with_name("ui")
        self.lock = threading.RLock()
        self.log_lock = threading.Lock()
        self.wake = threading.Condition(self.lock)
        self.closing = False
        self.stopped = threading.Event()
        self.started = now()
        self.public_name = ""   # this PC's Tailscale name, set by main(); the address labelers use
        self.local_names = set()   # this PC's own names and addresses when the hub serves 0.0.0.0 (--bind all)
        self.servers = []
        self.cache = {}         # worker id -> {"records", "received", "seen"}: the latest CACHE_LINES activity lines
        for folder in ("status", "events") + FILE_KINDS:
            (self.storage / folder).mkdir(parents=True, exist_ok=True)
        self.workers = self._read_json("workers.json", {})
        self.tokens = self._read_json("tokens.json", {})
        self.sessions = {}      # token hash -> id of the latest registration with that token
        self.status = {}
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

    def say(self, text):
        """One line of what the hub did: into <storage>/hub.log, and to the console when there is one."""
        try:
            with self.log_lock, (self.storage / "hub.log").open("a", encoding="utf-8") as file:
                file.write(f"{datetime.now():%Y-%m-%d %H:%M:%S} {text}\n")
        except OSError:
            pass
        self._echo(text)

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

    def _cache(self, worker_id):
        """The worker's latest activity in received order, read from the files once; add_events keeps it current."""
        cache = self.cache.get(worker_id)
        if cache is None:
            records = self._read_events(worker_id)[-CACHE_LINES:]
            for record in records:
                record["kind"] = classify(record.get("text", ""))
            cache = {"records": records, "received": [record.get("received", "") for record in records],
                     "seen": {(record.get("time", ""), record.get("text", "")) for record in records}}
            if records:
                try:
                    last = datetime.fromisoformat(records[-1].get("received", ""))
                    if worker_id not in self.clock or last > self.clock[worker_id]:
                        self.clock[worker_id] = last
                except ValueError:
                    pass
            self.cache[worker_id] = cache
        return cache

    def _trim(self, cache):
        if len(cache["records"]) > CACHE_LINES + 1000:   # cut in batches, not one line at a time
            cut = len(cache["records"]) - CACHE_LINES
            for record in cache["records"][:cut]:
                cache["seen"].discard((record.get("time", ""), record.get("text", "")))
            del cache["records"][:cut]
            del cache["received"][:cut]

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

    def control_allowed(self, address):
        """The control API answers this PC and the addresses named by --control-from; worker PCs on the same tailnet get 403."""
        return address in ("127.0.0.1", "::1") or address in self.control_from

    def host_allowed(self, host):
        """The Host a browser sent for the screen or the control API: one of this hub's own names. A page whose DNS name
        was rebound to 127.0.0.1 (DNS rebinding) carries its own name and is refused. No Host is no browser."""
        name = host.strip().lower()
        if not name:
            return True
        if name.startswith("["):
            name = name[1:name.find("]")] if "]" in name else name
        elif name.count(":") == 1:
            name = name.split(":")[0]
        with self.lock:
            allowed = {"127.0.0.1", "localhost", "::1"} | {address.lower() for address in self.control_from} | self.local_names
            allowed |= {server.server_address[0].lower() for server in self.servers}
        if self.public_name:
            allowed.add(self.public_name.lower())
        return name in allowed

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
            entry.pop("left", None)     # started again after a bye
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

    def leave(self, worker_id, session=""):
        """POST /bye: the labeler closed, so it is not connected from now on (not after its command wait and CONNECTED_SECONDS),
        until it registers again. A bye from a session the worker has since replaced changes nothing."""
        with self.lock:
            entry = self.workers.get(worker_id)
            current = entry.get("session", "") if entry else ""
            if entry is None or (session and current and session != current):
                return
            entry["left"] = now()
            self.wake.notify_all()      # its command wait ends now
        self.say(f"{worker_id} 종료")

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
                               "connected": not entry.get("left") and (recent or self.polling.get(worker_id, 0) > 0),
                               "last_seen": entry.get("last_seen", ""), "since": entry.get("since", ""),
                               "registered": entry.get("registered", ""), "last_action": self._last_action(worker_id),
                               "device": entry.get("device"), "summary": entry.get("summary"),
                               "status": self.status.get(worker_id, {})})
            return result

    def _last_action(self, worker_id):
        """The latest activity line that is not a status-bar notice, or None."""
        records = self._cache(worker_id)["records"]
        for record in reversed(records[-500:]):
            if record["kind"] != "notice":
                return {"time": record.get("time", ""), "text": record.get("text", ""), "kind": record["kind"]}
        return None

    def summary(self):
        workers = self.worker_list()
        with self.lock:
            port = self.servers[0].server_port if self.servers else 0
        return {"workers": len(workers), "connected": sum(1 for worker in workers if worker["connected"]),
                "storage": str(self.storage), "started": self.started,
                "address": f"{self.public_name}:{port}" if self.public_name and port else ""}

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
            cache = self._cache(worker_id)
            seen = cache["seen"]
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
                    record = {"time": time_, "received": self._stamp(worker_id), "text": text}
                    file.write(json.dumps(record, ensure_ascii=False) + "\n")
                    record["kind"] = classify(text)
                    cache["records"].append(record)
                    cache["received"].append(record["received"])
                    stored += 1
                    latest = max(latest, time_)
            self._trim(cache)
            if latest != self.workers[worker_id].get("since", ""):
                self.workers[worker_id]["since"] = latest
                self._write_json("workers.json", self.workers)
        return stored

    def events(self, worker_id, since="", limit=200):
        """Events in received order: after `since` the first `limit`, without it the last `limit` (`limit` <= 0 means all)."""
        with self.lock:
            cache = self._cache(worker_id)
            records = cache["records"]
            if since:
                start = bisect.bisect_right(cache["received"], since)
                chosen = records[start:start + limit] if limit > 0 else records[start:]
            else:
                chosen = records[-limit:] if limit > 0 else records[:]
            return [dict(record) for record in chosen]

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
                    left = bool(self.workers.get(worker_id, {}).get("left"))   # said bye: nobody is there to take the commands
                    pending = [] if left else [record for record in self.commands.values() if record["worker"] == worker_id and record["state"] == "queued"]
                    remaining = deadline - time.monotonic()
                    if left or pending or remaining <= 0 or self.closing:
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
            if address == "0.0.0.0":   # the screen may then be asked for by any of this PC's names
                try:
                    host, aliases, addresses = socket.gethostbyname_ex(socket.gethostname())
                    self.local_names |= {name.lower() for name in [host, *aliases, *addresses]}
                except OSError:
                    pass
            server = Server((address, first.server_port if first else port), Handler)
            server.hub = self
            threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.5}, daemon=True, name=f"hub {address}").start()
            self.servers.append(server)
            first = first or server
        return first

    def shutdown(self):
        """Stops serving; may be called more than once and from any thread but a server's own loop."""
        with self.lock:
            self.closing = True
            self.wake.notify_all()
            servers, self.servers = self.servers, []
        for server in servers:
            server.shutdown()
            server.server_close()
        self.stopped.set()

    # ---- the screen (hub/ui)

    def ui_file(self, name):
        if not FILE_NAME.match(name) or not name.strip("."):
            raise HubError(400, "bad file name")
        path = self.ui / name
        if not path.is_file():
            raise HubError(404, "no such file")
        return path

    def font_file(self, name):
        """A font of the screen: hub/ui/fonts, else the repository's assets/fonts (a hub run from the repository)."""
        if not FILE_NAME.match(name) or not name.strip("."):
            raise HubError(400, "bad file name")
        for folder in (self.ui / "fonts", Path(__file__).resolve().parent.parent / "assets" / "fonts"):
            if (folder / name).is_file():
                return folder / name
        raise HubError(404, "no such file")


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
    timeout = 120   # an idle or vanished client's connection is closed (command waits are at most LONGPOLL_MAX)

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
            elif not parts or parts[0] == "ui":
                self._screen(hub, method, parts)
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
        if parts == ["bye"] and method == "POST":
            hub.leave(worker_id, self.headers.get("X-Boccia-Session", "").strip())
            return self._empty()
        if parts == ["commands"] and method == "GET":
            return self._json({"commands": hub.take_commands(worker_id, self._number(query, "wait", 0, LONGPOLL_MAX), self.headers.get("X-Boccia-Session", "").strip())})
        if len(parts) == 3 and parts[0] == "commands" and parts[2] == "result" and method == "POST":
            hub.finish_command(worker_id, parts[1], self._parse(body))
            return self._empty()
        if len(parts) == 3 and parts[0] == "files" and method == "POST":
            return self._json({"path": hub.save_file(worker_id, parts[1], parts[2], body)})
        raise HubError(404, "no such route")

    def _screen(self, hub, method, parts):
        """The hub's screen (hub/ui): for the same addresses as the control API."""
        if not hub.control_allowed(self.client_address[0]):
            raise HubError(403, "screen: only this PC and --control-from addresses")
        if not hub.host_allowed(self.headers.get("Host", "")):
            raise HubError(403, "screen: unknown host name")
        if method != "GET":
            raise HubError(405, "GET only")
        if not parts:
            return self._file(hub.ui_file("index.html"), no_store=True)
        if len(parts) == 2:
            return self._file(hub.ui_file(parts[1]), no_store=True)
        if len(parts) == 3 and parts[1] == "fonts":
            return self._file(hub.font_file(parts[2]), no_store=True)
        raise HubError(404, "no such file")

    def _control(self, hub, method, parts, query, body):
        if not hub.control_allowed(self.client_address[0]):
            raise HubError(403, "control API: only this PC and --control-from addresses")
        if not hub.host_allowed(self.headers.get("Host", "")):
            raise HubError(403, "control API: unknown host name")
        if method == "POST" and not self.headers.get("Content-Type", "").startswith("application/json"):
            raise HubError(415, "control API: send JSON (Content-Type: application/json)")   # also keeps a browser's no-cors POST out
        if parts == ["shutdown"] and method == "POST":
            self._json({"ok": True})
            hub.say("허브 끔")
            threading.Thread(target=hub.shutdown, daemon=True, name="hub shutdown").start()   # after the answer has gone out
            return None
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

    def _file(self, path, no_store=False):
        data = path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", CONTENT_TYPES.get(path.suffix.lower(), "application/octet-stream"))
        self.send_header("Content-Length", str(len(data)))
        if no_store:
            self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)


# ---- command line

NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)   # under pythonw every console child would open a window of its own


def tailscale_ip():
    for exe in ("tailscale", r"C:\Program Files\Tailscale\tailscale.exe"):
        try:
            result = subprocess.run([exe, "ip", "-4"], capture_output=True, text=True, timeout=5, creationflags=NO_WINDOW)
        except (OSError, subprocess.SubprocessError):
            continue
        if result.returncode == 0 and result.stdout.split():
            return result.stdout.split()[0]
    return ""


def tailscale_name():
    """This PC's MagicDNS name (mark-hong-pc.tail68c059.ts.net), or "" without Tailscale."""
    for exe in ("tailscale", r"C:\Program Files\Tailscale\tailscale.exe"):
        try:
            result = subprocess.run([exe, "status", "--json"], capture_output=True, text=True, encoding="utf-8", timeout=5, creationflags=NO_WINDOW)
            name = json.loads(result.stdout).get("Self", {}).get("DNSName", "") if result.returncode == 0 else ""
        except (OSError, subprocess.SubprocessError, ValueError, AttributeError):
            continue
        if name:
            return name.rstrip(".")
    return ""


def rebind(hub, port):
    """--bind auto: Tailscale may come up after the hub (a login start); once it has an address, serve that too and learn the name."""
    ip = tailscale_ip()
    if not ip:
        return
    with hub.lock:
        bound = {server.server_address[0] for server in hub.servers}
    if ip not in bound:
        try:
            hub.serve([ip], port)
        except OSError as error:
            hub.say(f"{ip}:{port} 묶기 실패 · {error}")
            return
        hub.say(f"허브 http://{ip}:{port} · 저장소 {hub.storage}")
    if not hub.public_name:
        hub.public_name = tailscale_name()


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
    serve.add_argument("--control-from", default="", help="addresses allowed to use /api besides this PC, separated by commas")
    serve.add_argument("--print-port", action="store_true", help="print 'port <n>' first")
    args = parser.parse_args(argv)
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    hub = Hub(args.storage, say=lambda text: print(f"{datetime.now():%H:%M:%S} {text}", flush=True),
              control_from=[address.strip() for address in args.control_from.split(",") if address.strip()])
    first = None
    for address in bind_addresses(args.bind):
        try:
            server = hub.serve([address], first.server_port if first else args.port)
        except OSError as error:
            hub.say(f"{address}:{args.port} 묶기 실패 · {error}")   # pythonw has no stderr: hub.log keeps it
            print(f"{address}:{args.port} 묶기 실패 · {error}", file=sys.stderr, flush=True)
            if first is None:
                return 1
            continue
        if first is None:
            first = server
            if args.print_port:
                print(f"port {server.server_port}", flush=True)
        hub.say(f"허브 http://{address}:{server.server_port} · 저장소 {hub.storage}")
    if args.bind in ("auto", "all"):
        hub.public_name = tailscale_name()
    try:
        while not hub.stopped.wait(30):   # until /api/shutdown (or Ctrl+C)
            if args.bind == "auto":
                rebind(hub, first.server_port)
    except KeyboardInterrupt:
        pass
    finally:
        hub.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())
