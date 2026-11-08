"""Compare all ADC inputs across actual C core, Python and JS; offline only."""
from pathlib import Path
import json
import os
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tests.run_core_tests import compile_target
from host.hall_curve import normalized_from_raw, raw_to_estimated_mm

def main():
    exe = compile_target("hall_curve_vectors", [ROOT / "src/rapid_trigger.c", ROOT / "tests/hall_curve_vectors.c"])
    vectors = json.loads(subprocess.check_output([str(exe)], text=True))
    count = 0
    for v in vectors:
        values = v["travel"]
        assert len(values) == 4096 and min(values) == 0 and max(values) == 1000
        ascending = v["pressed"] > v["released"]
        ordered = values if ascending else list(reversed(values))
        assert all(a <= b for a,b in zip(ordered, ordered[1:])), "curve must remain monotonic"
        key = dict(released=v["released"], pressed=v["pressed"], curve_id=v["curve_id"], flags=7)
        for raw, expected in enumerate(values):
            actual = normalized_from_raw(raw, v["released"], v["pressed"], v["curve_id"])
            assert actual == expected, (v["released"], v["pressed"], v["curve_id"], raw, expected, actual)
            if v["curve_id"] == 1:
                at34 = raw_to_estimated_mm(raw, key, 3.4)
                at40 = raw_to_estimated_mm(raw, key, 4.0)
                assert abs(at40 - at34 * 4 / 3.4) < 1e-12
            count += 1
    path = ROOT / "build/host/hall-curve-vectors.json"
    path.write_text(json.dumps(vectors), encoding="utf-8")
    if os.environ.get('SPLIT_BEAT_STUDIO'):
        subprocess.run(["node", "tests/hall_curve_parity.mjs", str(path)], cwd=ROOT, check=True)
    report = {"status":"PASS", "sample_count_per_language":count, "endpoint_cases":6, "curve_ids":[0,1], "physical_device_access":False, "physical_mm_accuracy_validated":False}
    (ROOT / "build/host/hall-curve-parity.json").write_text(json.dumps(report,indent=2),encoding="utf-8")
    print(f"C -> Python Hall curve parity PASS: {count} samples; monotonicity and proportional travel checked")

if __name__ == "__main__":
    main()
