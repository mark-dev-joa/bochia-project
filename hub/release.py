"""Labeler versions for the launcher (design/hub/LAUNCHER.md): a release folder registered as one version, every file's
content kept once under its SHA-256 name, the newest registration the one the hub sends (`current`).

releases/files/<sha256>          the contents, each once
releases/<app>/<id>.json         one version: {"app", "id", "version", "registered", "files": [{"path", "size", "sha256"}]}
releases/<app>/index.json        {"current": id, "versions": [{"id", "version", "registered", "files", "bytes"}]}
releases/hash-cache.json         source path -> size, mtime_ns, sha256, so an unchanged file is not read again
"""
import hashlib
import json
import os
import re
import subprocess
from datetime import datetime
from pathlib import Path

HEX64 = re.compile(r"^[0-9a-f]{64}$")
CHUNK = 1 << 20
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)


def version_id(version):
    """The version as a file name: "2026-10-21 10:12 · 2039bd5" -> "2026-10-21-10-12-2039bd5"."""
    return re.sub(r"[^0-9A-Za-z]+", "-", version).strip("-")


def release_files(root, app):
    """(path in the manifest, source file), by path. A worker's own files (labeler/data, labeler/logs), the copy a running
    labeler left (*-previous.exe) and the launcher inside labeler/ stay out; the launcher goes in at the top."""
    if app != "labeler":
        raise ValueError(f"unknown app: {app}")
    root = Path(root)
    labeler = root / "labeler"
    found = []
    if labeler.is_dir():
        for path in labeler.rglob("*"):
            if not path.is_file():
                continue
            relative = path.relative_to(labeler).as_posix()
            parts = relative.lower().split("/")
            if (len(parts) > 1 and parts[0] in ("data", "logs")) or parts[-1].endswith("-previous.exe") or relative.lower() == "boccialauncher.exe":
                continue
            found.append(("labeler/" + relative, path))
    for folder in ("models", "cuda"):
        base = root / folder
        if base.is_dir():
            found.extend((f"{folder}/{path.relative_to(base).as_posix()}", path) for path in base.rglob("*") if path.is_file())
    launcher = labeler / "BocciaLauncher.exe"
    if launcher.is_file():
        found.append(("BocciaLauncher.exe", launcher))
    return sorted(found, key=lambda item: item[0])


def labeler_version(root):
    """What `labeler/boccia_labeler.exe --version` prints."""
    program = Path(root) / "labeler" / "boccia_labeler.exe"
    if not program.is_file():
        raise ValueError(f"no labeler in {root}")
    result = subprocess.run([str(program), "--version"], capture_output=True, text=True, encoding="utf-8", errors="replace",
                            timeout=60, creationflags=NO_WINDOW)
    version = result.stdout.strip()
    if result.returncode != 0 or not version:
        raise ValueError(f"the labeler did not say its version ({result.returncode})")
    return version


def file_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as reader:
        while chunk := reader.read(CHUNK):
            digest.update(chunk)
    return digest.hexdigest()


def read_json(path, default):
    try:
        return json.loads(Path(path).read_text("utf-8"))
    except (OSError, ValueError):
        return default


def write_json(path, value):
    """A temporary file beside it, then a rename: a reader sees the old file or the new one."""
    path = Path(path)
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=1), "utf-8")
    os.replace(temporary, path)


def store_blob(store, source):
    """Copies the file into the store under the SHA-256 of the bytes copied, which it returns."""
    temporary = store / f".{os.getpid()}.{Path(source).name}.tmp"
    digest = hashlib.sha256()
    with open(source, "rb") as reader, open(temporary, "wb") as writer:
        while chunk := reader.read(CHUNK):
            digest.update(chunk)
            writer.write(chunk)
    name = digest.hexdigest()
    if (store / name).is_file():
        temporary.unlink()
    else:
        os.replace(temporary, store / name)
    return name


def register(storage, app, root, version=None):
    """Registers the release folder as one version of the app and makes it current.
    {"manifest", "added_bytes" (new contents copied in), "hashed" (files read for their hash)}."""
    storage, root = Path(storage), Path(root)
    files = release_files(root, app)
    if not files:
        raise ValueError(f"nothing to register in {root}")
    version = labeler_version(root) if version is None else version.strip()
    ident = version_id(version)
    if not ident:
        raise ValueError(f"bad version: {version!r}")
    releases = storage / "releases"
    store = releases / "files"
    store.mkdir(parents=True, exist_ok=True)
    cache_path = releases / "hash-cache.json"
    cache = read_json(cache_path, {})
    hashed = added = 0
    entries = []
    for path, source in files:
        stat = source.stat()
        key = str(source.resolve())
        known = cache.get(key) or {}
        digest = known.get("sha256", "")
        if known.get("size") != stat.st_size or known.get("mtime_ns") != stat.st_mtime_ns or not HEX64.match(digest):
            digest = file_sha256(source)
            hashed += 1
        if not (store / digest).is_file():
            digest = store_blob(store, source)   # the name is what was copied, should the file have changed meanwhile
            added += stat.st_size
        cache[key] = {"size": stat.st_size, "mtime_ns": stat.st_mtime_ns, "sha256": digest}
        entries.append({"path": path, "size": stat.st_size, "sha256": digest})
    write_json(cache_path, cache)
    manifest = {"app": app, "id": ident, "version": version, "registered": datetime.now().isoformat(timespec="seconds"), "files": entries}
    folder = releases / app
    folder.mkdir(parents=True, exist_ok=True)
    write_json(folder / f"{ident}.json", manifest)
    index = read_json(folder / "index.json", {})
    versions = [entry for entry in index.get("versions", []) if entry.get("id") != ident]
    versions.append({"id": ident, "version": version, "registered": manifest["registered"], "files": len(entries),
                     "bytes": sum(entry["size"] for entry in entries)})
    write_json(folder / "index.json", {"current": ident, "versions": versions})
    return {"manifest": manifest, "added_bytes": added, "hashed": hashed}


def current(storage, app):
    """The version the hub sends, or None before the first registration."""
    folder = Path(storage) / "releases" / app
    ident = read_json(folder / "index.json", {}).get("current", "")
    if not ident or version_id(ident) != ident:
        return None
    manifest = read_json(folder / f"{ident}.json", None)
    return manifest if isinstance(manifest, dict) else None


def blob(storage, sha256):
    """The stored content with that SHA-256, or None (an unknown or malformed name)."""
    if not isinstance(sha256, str) or not HEX64.match(sha256):
        return None
    path = Path(storage) / "releases" / "files" / sha256
    return path if path.is_file() else None
