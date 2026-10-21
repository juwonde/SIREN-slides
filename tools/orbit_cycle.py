"""One power-off orbit cycle (cold boot, download over CAN, preprocessing, store, shutdown) as phases + trace.

    python tools/orbit_cycle.py GUARD_POWER_CSV CYCLE_OUT ORBIT_RESULTS_JSONL LABEL OUT_PREFIX

GUARD_POWER_CSV is the INA228 guard log that held the relay during the cycle (only the cycle window is read).
CYCLE_OUT is the cycle runner's output (its JSON block has relay_on_ns, the boot / orbit window / shutdown phases
and mission_s); ORBIT_RESULTS_JSONL has the orbit window's sub-phases (flight idle, rx, rx-tail, prep, store,
post-idle) for LABEL. Writes OUT_PREFIX.json (phases with kind, begin_s, end_s from power on; mission_s, mean_w,
max_w) and OUT_PREFIX-trace.csv (t_s,power_w) for tools/ops_windows.py.
- F-ASTRA 6.144.0 cycle oc3 (10/21 15:35 UTC, guard37, image r14, flight idle knobs, 3.82 MB at 6.68 KB/s)
"""
import json
import sys

from pd_peak_trace import read_window

KIND = {"flight": "idle", "rx": "download", "rx-tail": "download", "prep": "dprep", "store": "store", "post-idle": "idle"}


def main():
    guard, cycle_out, results, label, out = sys.argv[1:6]
    text = open(cycle_out, encoding="utf-8").read().splitlines()
    a = text.index("{"); b = next(i for i in range(a, len(text)) if text[i] == "}")
    cyc = json.loads("\n".join(text[a:b + 1]))
    rec = next(json.loads(l) for l in open(results, encoding="utf-8") if l.strip() and json.loads(l)["label"] == label)
    top = {p["phase"]: p for p in cyc["phases"]}
    phases, t = [dict(kind="boot", begin_s=0.0, end_s=top["boot"]["seconds"])], top["boot"]["seconds"]
    for p in rec["phases"]:
        if p["seconds"] > 0:
            phases.append(dict(kind=KIND[p["phase"]], begin_s=t, end_s=t + p["seconds"])); t += p["seconds"]
    phases.append(dict(kind="shutdown", begin_s=t, end_s=t + top["shutdown"]["seconds"]))
    on = cyc["relay_on_ns"]; end = phases[-1]["end_s"]
    rows = [((ns - on) / 1e9, w) for ns, w in read_window(guard, on, on + int(end * 1e9))]
    rows = [r for r in rows if r[0] >= 0.05]  # the inrush sample at power on is the input capacitors, as in F-ASTRA
    for p in phases:
        p["seconds"] = round(p["end_s"] - p["begin_s"], 3)
    e = sum((tb - ta) * (wa + wb) / 2 for (ta, wa), (tb, wb) in zip(rows, rows[1:]))
    rec_out = dict(state=f"orbit cycle {label}", phases=phases, mission_s=round(end, 2), energy_j=round(e, 1),
                   mean_w=round(e / (rows[-1][0] - rows[0][0]), 4), max_w=max(w for _, w in rows))
    json.dump(rec_out, open(out + ".json", "w", encoding="utf-8"), indent=1)
    open(out + "-trace.csv", "w", encoding="ascii", newline="\n").write("t_s,power_w\n" + "\n".join(f"{t:.3f},{w:.4f}" for t, w in rows) + "\n")
    print(f"{label}: {end:.2f} s, {len(rows)} samples, {e:.1f} J, mean {rec_out['mean_w']} W, max {rec_out['max_w']} W; "
          + ", ".join(f"{p['kind']} {p['seconds']}" for p in phases))


if __name__ == "__main__":
    main()
