"""Download the official, pinned EfficientSAM-Ti ONNX conversion for local use."""
import argparse
import hashlib
import json
from pathlib import Path
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parents[1]
REVISION = "d8dbb1eee73bfb3392aa6f6e8944aeb13f3f4036"
BASE = f"https://huggingface.co/spaces/yunyangx/EfficientSAM/resolve/{REVISION}/"
FILES = {
    "efficientsam_ti_encoder.onnx": "84ed466ffcc5c1f8d08409bc34a23bb364ab2c15e402cb12d4335a42be0e0951",
    "efficientsam_ti_decoder.onnx": "a62f8fa5ea080447c0689418d69e58f1e83e0b7adf9c142e2bd9bcc8045c0b11",
}


def setup(destination: Path):
    destination.mkdir(parents=True, exist_ok=True)
    for name, expected in FILES.items():
        path = destination / name
        if path.exists() and hashlib.sha256(path.read_bytes()).hexdigest() == expected:
            print(f"Already verified: {name}", flush=True)
            continue
        temporary = path.with_suffix(".download")
        digest = hashlib.sha256()
        with urlopen(BASE + name, timeout=60) as response, temporary.open("wb") as handle:
            while chunk := response.read(1024 * 1024):
                handle.write(chunk)
                digest.update(chunk)
        if digest.hexdigest() != expected:
            temporary.unlink(missing_ok=True)
            raise ValueError(f"Checksum mismatch: {name}")
        temporary.replace(path)
        print(f"Verified: {name} ({path.stat().st_size:,} bytes)", flush=True)
    with urlopen("https://raw.githubusercontent.com/yformer/EfficientSAM/main/LICENSE", timeout=30) as response:
        (destination / "LICENSE-EfficientSAM.txt").write_bytes(response.read())
    (destination / "provenance.json").write_text(json.dumps({"model": "EfficientSAM-Ti", "license": "Apache-2.0",
        "upstream": "https://github.com/yformer/EfficientSAM", "conversion_repository": "https://huggingface.co/spaces/yunyangx/EfficientSAM",
        "revision": REVISION, "files": FILES}, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--destination", type=Path, default=ROOT / "data" / "models" / "efficientsam")
    setup(parser.parse_args().destination)
