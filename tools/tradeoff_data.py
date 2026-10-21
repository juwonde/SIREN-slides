"""Every board run with both a GOPS and a 100 Hz peak power, per model, for the trade-off scatter.

    python tools/tradeoff_data.py RESULTS_JSONL WEEKLY_DIR HARNESS_DIR OUT_JSON

RESULTS_JSONL is the power campaign log (dnn+can sessions, phases.{BUS,PD,Comp}.max_w and models.*.gops),
WEEKLY_DIR holds the weekly NN-window session records (weekly-*.json: phases model/hybrid/grubatch with max_w,
models.{bus,pd}.gops, gru_batch.run_gops_min), HARNESS_DIR the bitstream and bundle folders (a bundle's
bitstream_record_sha256 is the sha256 of bpg-*/bitstream.json). Diagnostic power experiments that change the model (inv-*, ext*, zd*: weights or inputs
zeroed) are left out.
Peak = highest 100 Hz single sample of the model's phase (12 V board input, INA228). Comp GOPS of a weekly run is
the slowest of its engine runs. Only passed, byte-exact model runs are kept. Writes one row per (run, model).
"""
import glob
import hashlib
import json
import os
import re
import sys


def records(harness):
    out = {}
    for f in glob.glob(os.path.join(harness, "bpg-*", "bitstream.json")):
        out[hashlib.sha256(open(f, "rb").read()).hexdigest()] = os.path.basename(os.path.dirname(f))
    return out


def bitstream_of(bundle, harness, recs):
    m = re.match(r"(bpg-r\d+[a-z]*)", bundle or "")
    if m or not bundle:
        return m.group(1) if m else "?"
    try:
        b = json.load(open(os.path.join(harness, bundle, "bundle.json"), encoding="utf-8"))
    except OSError:
        return "?"
    return recs.get(b.get("bitstream_record_sha256", ""), "?")


def main():
    results, weekly, harness, out = sys.argv[1:5]
    rows, recs = [], records(harness)
    for line in open(results, encoding="utf-8"):
        r = json.loads(line) if line.strip() else {}
        m, ph = r.get("models") or {}, r.get("phases") or {}
        if not m.get("bus"):
            continue
        bs = bitstream_of(r.get("bundle"), harness, recs)
        for model, key, phase in (("AI #2", "bus", "BUS"), ("AI #1", "pd", "PD"), ("AI #3", "grug4", "Comp"), ("AI #3", "gru", "Comp")):
            g, p = (m.get(key) or {}), ph.get(phase) or {}
            if g.get("gops") and g.get("byte_exact") and g.get("passed") and p.get("max_w"):
                rows.append(dict(model=model, gops=round(g["gops"], 3), peak_w=round(p["max_w"], 4), bitstream=bs,
                                 run=r.get("bundle"), utc=r.get("utc", "")[:16], source="session"))
    for f in sorted(glob.glob(os.path.join(weekly, "weekly-*.json"))):
        r = json.load(open(f, encoding="utf-8"))
        label = os.path.basename(f)[7:-5]
        if not r.get("passed") or re.match(r"(inv|ext)", r.get("bundle", "") + " " + label) or "-zd" in label:
            continue
        bs = bitstream_of(r.get("bundle"), harness, recs)
        mx = {}
        for p in r.get("phases", []):
            k = {"model": "AI #2", "hybrid": "AI #1", "grubatch": "AI #3"}.get(p.get("kind"))
            if k and p.get("max_w"):
                mx[k] = max(mx.get(k, 0), p["max_w"])
        m = r.get("models") or {}
        gb = r.get("gru_batch") or {}
        gops = {"AI #2": (m.get("bus") or {}).get("gops"), "AI #1": (m.get("pd") or {}).get("gops"),
                "AI #3": gb.get("run_gops_min") if gb.get("passed") else None}
        for model in ("AI #2", "AI #1", "AI #3"):
            if gops[model] and model in mx:
                rows.append(dict(model=model, gops=round(gops[model], 3), peak_w=round(mx[model], 4), bitstream=bs,
                                 run=label, utc="", source="weekly"))
    json.dump(rows, open(out, "w", encoding="utf-8"), indent=0)
    by = {}
    for r in rows:
        by.setdefault(r["bitstream"], set()).add(r["run"])
    print(len(rows), "points,", len({r["run"] for r in rows}), "runs on", len(by), "bitstreams:",
          ", ".join(f"{k} ({len(v)})" for k, v in sorted(by.items())))


if __name__ == "__main__":
    main()
