"""Boccia Hub command line: talks to a running hub's control API (design/hub/DESIGN.md)."""
import argparse
import importlib.util
import json
import subprocess
import sys
import time
import unicodedata
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

DEFAULT_HUB = "127.0.0.1:8790"
QUOTE_SAFE = "/?=&%@-._~:"


class Exit(Exception):
    def __init__(self, code, text=""):
        super().__init__(text)
        self.code = code


class Api:
    def __init__(self, address):
        self.address = address

    def call(self, method, path, body=None, raw=False):
        data = json.dumps(body, ensure_ascii=False).encode("utf-8") if body is not None else None
        request = urllib.request.Request(f"http://{self.address}{urllib.parse.quote(path, safe=QUOTE_SAFE)}", data=data, method=method)
        if data is not None:
            request.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(request, timeout=15) as response:
                payload = response.read()
        except urllib.error.HTTPError as error:
            try:
                text = json.loads(error.read()).get("error", "")
            except ValueError:
                text = ""
            raise Exit(1, f"오류 {error.code} · {text}".rstrip(" ·")) from None
        except (urllib.error.URLError, OSError):
            raise Exit(1, f"허브에 연결할 수 없음 · {self.address}") from None
        if raw:
            return payload
        return json.loads(payload) if payload else None

    def worker(self, text):
        """The worker named by an id or a bare name; a bare name shared by several PCs lists them and exits 2."""
        workers = self.call("GET", "/api/workers")
        exact = [worker for worker in workers if worker["id"] == text]
        if exact:
            return exact[0]
        same = [worker for worker in workers if worker["worker"] == text]
        if len(same) == 1:
            return same[0]
        if not same:
            raise Exit(1, f"작업자 없음 · {text}")
        raise Exit(2, "\n".join(worker["id"] for worker in same))


# ---- formatting

def width(text):
    return sum(2 if unicodedata.east_asian_width(char) in "WF" else 1 for char in text)


def table(header, rows):
    widths = [max(width(row[i]) for row in [header] + rows) for i in range(len(header))]
    for row in [header] + rows:
        print("  ".join(cell + " " * (widths[i] - width(cell)) for i, cell in enumerate(row)).rstrip())


def clock(stamp, seconds=False):
    """HH:MM(:SS) out of '2026-10-19T14:02:33.000' or '2026-10-19 14:02:33.000'."""
    return stamp[11:19] if seconds else stamp[11:16]


def doing(status):
    text = status.get("text") if isinstance(status, dict) else ""
    return text if isinstance(text, str) else ""


# ---- commands

def status(api):
    summary = api.call("GET", "/api/status")
    print(f"허브 {api.address} · 붙음 {summary['connected']} · 전체 {summary['workers']} · 저장소 {summary['storage']}")


def workers(api):
    rows = []
    for worker in api.call("GET", "/api/workers"):
        state = "붙음" if worker["connected"] else f"떨어짐 {clock(worker['last_seen'])}".rstrip()
        rows.append([worker["worker"], worker["pc"], worker["app"], worker["version"], state, clock(worker["since"], True), doing(worker["status"])])
    table(["이름", "PC", "앱", "버전", "상태", "마지막", "지금"], rows)


def events(api, text, limit, follow):
    worker = api.worker(text)
    path = f"/api/workers/{worker['id']}/events"
    last = ""
    for event in api.call("GET", f"{path}?limit={limit}"):
        print(f"{clock(event['time'], True)}  {event['text']}", flush=True)
        last = event["received"]
    while follow:
        time.sleep(2)
        for event in api.call("GET", f"{path}?since={last}&limit=0"):
            print(f"{clock(event['time'], True)}  {event['text']}", flush=True)
            last = event["received"]


def send(api, text, name, value, wait):
    worker = api.worker(text)
    path = f"/api/workers/{worker['id']}/commands"
    cid = api.call("POST", path, {"name": name, "value": value})["id"]
    print(f"{cid} 보냄", flush=True)
    deadline = time.monotonic() + wait
    while time.monotonic() < deadline:
        time.sleep(0.5)
        for command in api.call("GET", path):
            if command["id"] == cid and command["state"] in ("done", "failed"):
                print(f"{'완료' if command['state'] == 'done' else '실패'} · {command['result']}".rstrip(" ·"))
                return
    if wait > 0:
        print(f"아직 응답 없음 · {wait}초")


def stop(api):
    api.call("POST", "/api/shutdown", {})
    print("허브 끔")


def invite(api, worker):
    print(api.call("POST", "/api/invites", {"worker": worker})["token"])


def logs(api, text):
    worker = api.worker(text)
    files = api.call("GET", f"/api/workers/{worker['id']}/files/log")
    if not files:
        raise Exit(1, f"로그 없음 · {worker['id']}")
    latest = max(files, key=lambda file: (file["time"], file["name"]))
    sys.stdout.write(api.call("GET", f"/api/workers/{worker['id']}/files/log/{latest['name']}", raw=True).decode("utf-8", "replace"))
    sys.stdout.flush()


def release(app, root, storage, version):
    """Registers a release folder as the version the hub sends (design/hub/LAUNCHER.md); the hub need not run."""
    spec = importlib.util.spec_from_file_location("boccia_hub_release", Path(__file__).with_name("release.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    try:
        result = module.register(Path(storage), app, Path(root), version)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        raise Exit(1, f"등록 실패 · {error}") from None
    manifest = result["manifest"]
    total = sum(entry["size"] for entry in manifest["files"])
    print(f"{app} {manifest['version']} · 파일 {len(manifest['files'])}개 · {total / 1024 ** 3:.1f} GB · 새로 {result['added_bytes'] / 1024 ** 2:.1f} MB")


def serve(argv):
    spec = importlib.util.spec_from_file_location("boccia_hub_server", Path(__file__).with_name("hub.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.main(argv)


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    if argv and argv[0] == "serve":
        return serve(argv)
    parser = argparse.ArgumentParser(prog="boccia-hub")
    parser.add_argument("--hub", default=DEFAULT_HUB, help="hub address (default 127.0.0.1:8790)")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("serve", help="run the hub (python hub/hub.py serve …)")
    commands.add_parser("status")
    commands.add_parser("stop")
    commands.add_parser("workers")
    sub = commands.add_parser("events")
    sub.add_argument("worker")
    sub.add_argument("--follow", action="store_true")
    sub.add_argument("--limit", type=int, default=50)
    sub = commands.add_parser("send")
    sub.add_argument("worker")
    sub.add_argument("name")
    sub.add_argument("value", nargs="*")
    sub.add_argument("--wait", type=int, default=10)
    commands.add_parser("invite").add_argument("worker")
    commands.add_parser("logs").add_argument("worker")
    sub = commands.add_parser("release", help="register a release folder as the version the hub sends (no hub needed)")
    sub.add_argument("app")
    sub.add_argument("root")
    sub.add_argument("--storage", default=r"D:\Boccia Hub")
    sub.add_argument("--version")
    args = parser.parse_args(argv)
    api = Api(args.hub)
    try:
        if args.command == "status":
            status(api)
        elif args.command == "stop":
            stop(api)
        elif args.command == "workers":
            workers(api)
        elif args.command == "events":
            events(api, args.worker, args.limit, args.follow)
        elif args.command == "send":
            send(api, args.worker, args.name, " ".join(args.value), args.wait)
        elif args.command == "invite":
            invite(api, args.worker)
        elif args.command == "logs":
            logs(api, args.worker)
        elif args.command == "release":
            release(args.app, args.root, args.storage, args.version)
    except Exit as exit_:
        if str(exit_):
            print(exit_)
        return exit_.code
    except KeyboardInterrupt:
        return 130
    return 0


if __name__ == "__main__":
    sys.exit(main())
