"""Plot a selected standalone weekly run and a standalone download cycle.

Inputs are explicit external measurement folders; no board is accessed.
Raw traces stay in the caller-selected runtime folder. Figures and a reduced
receipt go to OUT. Power is the measured 12 V development-board input.
"""
import argparse
import csv
import json
import pathlib
import re

from ops_windows import COL, INK6, energy, panel
from pd_run_energy import svg as pd_svg


def read(path):
    return json.loads(path.read_text(encoding="utf-8"))


def trace(path):
    with path.open() as f:
        return [(float(r[0]), float(r[1])) for r in list(csv.reader(f))[1:]]


def stitch(raw, intervals):
    rows, offset = [], 0.0
    for a, b in intervals:
        rows += [(offset + t - a, w) for t, w in raw if a <= t < b]
        offset += b - a
    return rows, offset


def save_trace(path, rows):
    path.write_text("t_s,power_w\n" + "\n".join(f"{t:.6f},{w:.7f}" for t, w in rows) + "\n", encoding="ascii")


def main():
    ap = argparse.ArgumentParser(__doc__)
    ap.add_argument("--weekly-dir", type=pathlib.Path, required=True)
    ap.add_argument("--orbit-dir", type=pathlib.Path, required=True)
    ap.add_argument("--run", required=True)
    ap.add_argument("--download", required=True)
    ap.add_argument("--bitstream", required=True)
    ap.add_argument("--out", type=pathlib.Path, required=True)
    ap.add_argument("--runtime-out", type=pathlib.Path, required=True)
    a = ap.parse_args()
    if re.match(r"(um|fx)", a.run):
        raise ValueError("mode-integration experiments are excluded")
    a.out.mkdir(parents=True, exist_ok=True)
    a.runtime_out.mkdir(parents=True, exist_ok=True)
    j = read(a.weekly_dir / f"weekly-{a.run}.json")
    b = read(a.weekly_dir / f"boot-{a.run}.json")
    assert j["passed"] and b["session"]["passed"]
    assert all(j["models"][k]["passed"] and j["models"][k]["byte_exact"] for k in ("bus", "pd"))
    assert j["gru_batch"]["passed"]
    gops = [j["models"]["bus"]["gops"], j["models"]["pd"]["gops"], j["gru_batch"]["run_gops_min"]]
    assert min(gops) >= 50
    raw = trace(a.weekly_dir / f"boot-{a.run}-trace.csv")
    ph = b["phases"]
    one = lambda kind: next(p for p in ph if p["kind"] == kind)
    boot, pl, prep, bus, gru, pd, off = [one(k) for k in ("boot", "plload", "prep", "model", "grubatch", "hybrid", "shutdown")]
    enc = [p for p in ph if p["kind"] == "encode"]
    end_work = max(p["end_s"] for p in enc)
    intervals = [(0, boot["end_s"]), (pl["begin_s"], end_work), (off["begin_s"], off["end_s"])]
    rows, duration = stitch(raw, intervals)
    shift = boot["end_s"] - pl["begin_s"]
    phases = [dict(boot)] + [dict(p, begin_s=p["begin_s"] + shift, end_s=p["end_s"] + shift)
              for p in ph if p["kind"] not in ("boot", "test", "harness", "shutdown")]
    phases.append(dict(off, begin_s=duration - off["seconds"], end_s=duration))
    labels = [(f"boot · {boot['seconds']:.1f} s", 1, 6.3, "start", INK6),
              ("AI #2 + AI #3 GRU", 1, 5.6, "start", COL["model"]),
              (f"AI #1 · {pd['seconds']:.1f} s", 80, 6.3, "middle", COL["hybrid"]),
              ("encode · 23.4 s", 132, 5.6, "middle", COL["encode"]),
              (f"spare · {180-duration:.0f} s", 164, 3.5, "middle", INK6)]
    svg, peak = panel(rows, phases, 180, (0, 30, 60, 90, 120, 150, 180), labels, 18, .2, (0, .1, .2), True)
    (a.out / "weekly-window-selected.svg").write_text(svg, encoding="utf-8")
    save_trace(a.runtime_out / "weekly-selected.csv", rows)
    pdr = [(t-pd["begin_s"], w) for t, w in raw if pd["begin_s"] <= t < pd["end_s"]]
    erows, e = [(pdr[0][0], pdr[0][1], 0)], 0
    for (ta, wa), (tb, wb) in zip(pdr, pdr[1:]):
        e += (tb-ta)*(wa+wb)/2/3600
        erows.append((tb, wb, e))
    (a.out / "pd-run-selected.svg").write_text(pd_svg(erows, 504), encoding="utf-8")

    d = read(a.orbit_dir / f"cycle-{a.download}.json")
    rawd = trace(a.orbit_dir / f"cycle-{a.download}-trace.csv")
    records = [json.loads(l) for l in (a.orbit_dir / "orbit-results.jsonl").read_text().splitlines() if l.strip()]
    o = next(r for r in reversed(records) if r.get("label") == a.download)
    start = next(t for t, s in o["markers"] if s.startswith("flight on")) - d["relay_on_ns"]/1e9
    phases_d, intervals_d = [], [(0, d["boot_s"])]
    offset = d["boot_s"]
    phases_d.append(dict(kind="boot", begin_s=0, end_s=offset))
    kinds = {"flight":"idle", "rx":"download", "rx-tail":"download", "prep":"dprep", "store":"store", "post-idle":"idle"}
    for p in o["phases"]:
        end = start+p["seconds"]
        intervals_d.append((start, end))
        phases_d.append(dict(kind=kinds[p["phase"]], begin_s=offset, end_s=offset+p["seconds"]))
        offset += p["seconds"]; start = end
    shutdown = next(p for p in d["phases"] if p["phase"] == "shutdown")
    # The saved baseline receipt locates this exact shutdown in the same raw trace.
    baseline = read(a.orbit_dir / "baseline-modes.json")["download"]
    assert read(a.orbit_dir / "page-data.json")["dl_label"] == a.download
    sd = baseline["shutdown_match"]["start_s"]
    intervals_d.append((sd, sd+shutdown["seconds"]))
    phases_d.append(dict(kind="shutdown", begin_s=offset, end_s=offset+shutdown["seconds"]))
    rowsd, dd = stitch(rawd, intervals_d)
    labels_d = [("boot · 21.1 s", 3, 5.8, "start", INK6),
                ("CAN receive · 275 s", 170, 5.1, "middle", COL["download"]),
                ("preprocess + store · 4.1 s", 330, 6.3, "middle", COL["store"]),
                ("off · 9.3 s", 355, 5.6, "middle", INK6),
                (f"spare · {600-dd:.0f} s", 460, 3.5, "middle", INK6)]
    svg, dp = panel(rowsd, phases_d, 600, (0, 150, 300, 450, 600), labels_d, 16, .3, (0, .1, .2, .3), False)
    (a.out / "orbit-window-selected.svg").write_text(svg, encoding="utf-8")
    save_trace(a.runtime_out / "orbit-selected.csv", rowsd)
    tx = d["orbit"]["tx"]
    receipt = dict(weekly=dict(run=a.run, bundle=j["bundle"], bitstream=a.bitstream, scope="standalone weekly mode; selected best observed power, Quiet week",
                   seconds=round(duration,3), sampled_peak_w=peak[1], gops=gops,
                   model_peaks_w=[j["summary"][k]["max_w"] for k in ("BUS","PD","Comp GRU")], energy_Wh=energy(rows,0,duration)/3600,
                   pd_seconds=pd["seconds"], pd_peak_w=max(w for _,w in pdr), pd_mean_w=sum(w for _,w in pdr)/len(pdr), pd_energy_Wh=e),
                   download=dict(run=a.download, seconds=round(dd,3), mean_w=d["mission_mean_w"], energy_Wh=energy(rowsd,0,dd)/3600,
                   source_mission_seconds=d["mission_s"], input_bytes=tx["input_bytes"], can_bitrate=tx["bitrate"], target_Bps=tx["policy"]["selected_Bps"], effective_Bps=tx["effective_Bps"], object_equal=d["orbit"]["object_equal"], features_equal=d["orbit"]["features_equal"]),
                   measurement="INA228, 12 V board input, 100 Hz. Sensor supply and separate fan excluded.",
                   boundary="Measured boot, operation and shutdown intervals; test upload, baseline and result retrieval excluded. Not a contiguous flight mission or a flight-unit power measurement.")
    (a.out / "selected-mode-results.json").write_text(json.dumps(receipt,indent=2)+"\n",encoding="utf-8")
    print(json.dumps(receipt,indent=2))


if __name__ == "__main__":
    main()
