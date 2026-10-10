"""Per-op kernel time and execution provider of the last run in ONNX Runtime profile files (BOCCIA_ORT_PROFILE=<prefix>).
python analyze_ort_profile.py <profile.json> [...]
"""
import collections
import json
import sys

for path in sys.argv[1:]:
    events = json.load(open(path, encoding="utf-8"))
    runs = [e for e in events if e.get("cat") == "Session" and e.get("name") == "model_run"]
    if not runs:
        print(path, ": no model_run"); continue
    last = runs[-1]
    nodes = [e for e in events if e.get("cat") == "Node" and e["name"].endswith("_kernel_time") and last["ts"] <= e["ts"] <= last["ts"] + last["dur"]]
    by_op = collections.defaultdict(float); by_provider = collections.Counter(); time_provider = collections.defaultdict(float)
    for e in nodes:
        op = e["args"].get("op_name", "?"); provider = e["args"].get("provider", "?")
        by_op[op] += e["dur"] / 1000; by_provider[provider] += 1; time_provider[provider] += e["dur"] / 1000
    print(f"{path}: last run {last['dur'] / 1000:.1f} ms, {len(nodes)} kernels; nodes per provider {dict(by_provider)}; ms per provider { {k: round(v, 1) for k, v in time_provider.items()} }")
    for ms, op in sorted(((v, k) for k, v in by_op.items()), reverse=True)[:12]:
        print(f"   {op:22s} {ms:7.1f} ms")
    slow = sorted(nodes, key=lambda e: -e["dur"])[:6]
    for e in slow:
        print(f"   slowest: {e['args'].get('op_name')} {e['name'][:-12]} {e['dur'] / 1000:.1f} ms on {e['args'].get('provider')}")
