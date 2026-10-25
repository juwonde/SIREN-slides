"""Plot a fixed, powered CAN download test; boot and NN are outside this window.

External evidence paths are explicit CLI inputs. Only the reduced receipt and
editable SVG go to the presentation repository; raw traces stay external.
"""
import argparse
import csv
import json
import pathlib

from ops_windows import COL, INK6, energy, panel


def main():
    ap = argparse.ArgumentParser(__doc__)
    ap.add_argument("--evidence", type=pathlib.Path, required=True)
    ap.add_argument("--report", type=pathlib.Path, required=True)
    ap.add_argument("--out", type=pathlib.Path, required=True)
    a = ap.parse_args()
    report = json.loads(a.report.read_text(encoding="utf-8"))
    windows = [json.loads(l) for l in (a.evidence / "windows.jsonl").read_text().splitlines() if l]
    window = next(w for w in windows if w["name"] == "D_gzip")
    assert "bitrate 500000" in window["before"]
    tx = json.loads((a.evidence / "object-D_gzip-verified.json").read_text())
    assert tx["complete"] and tx["receiver_file_SHA_verified"]
    assert report["unchanged"]["output_hashes_equal"] and report["final_window"]["can_window_pass"]
    rows = []
    t0, t1 = window["t0_ns"], window["t1_ns"]
    with (a.evidence / "guard-board-1" / "power.csv").open() as f:
        for r in csv.DictReader(f):
            t = int(r["host_utc_ns"])
            if t0 <= t <= t1:
                rows.append(((t-t0)/1e9, float(r["power_W"])))
    assert len(rows) == report["final_window"]["samples"]
    assert abs(max(w for _, w in rows)-report["final_window"]["sampled_max_W"]) < 1e-8
    done = report["job_seconds"]
    receive_end = (tx["finished_utc_ns"]-t0)/1e9
    phases = [dict(kind="download", begin_s=0, end_s=receive_end),
              dict(kind="store", begin_s=receive_end, end_s=done),
              dict(kind="idle", begin_s=done, end_s=600.01)]
    labels = [("RX + processing · 118 s", 110, 6.3, "middle", COL["download"]),
              ("CAN-enabled idle · 482 s", 390, 5.6, "middle", INK6),
              ("600 s mean · 2.85 W", 400, 4.7, "middle", COL["store"])]
    svg, peak = panel(rows, phases, 600, (0, 150, 300, 450, 600), labels, 16, .5, (0, .25, .5), False)
    sampled_energy = energy(rows, 0, rows[-1][0])/3600
    svg = svg.replace(f">{sampled_energy:.2f} Wh<", f">{sampled_energy:.3f} Wh<")
    active_j = energy(rows, 0, done)
    active_wh = active_j/3600
    done_x = 88 + done/600*692
    done_y = 405 - active_wh/.5*105
    background = '<rect width="800" height="540" fill="#FFFFFF"/>'
    highlight = (f'<rect x="88" y="300" width="{done_x-88:.3f}" height="105" fill="#1F6F6B" opacity="0.14"/>'
                 f'<rect x="{done_x:.3f}" y="300" width="1.5" height="105" fill="#1F6F6B" opacity="0.65"/>')
    svg = svg.replace(background, background+highlight)
    annotation = (f'<circle cx="{done_x:.3f}" cy="{done_y:.3f}" r="5" fill="#1F6F6B" stroke="white" stroke-width="2"/>'
                  '<text x="250" y="288" style="font-family:\'Source Serif 4\';font-size:28px;font-weight:700" fill="#1F6F6B">'
                  f'0–118 s: {active_wh:.3f} Wh ({active_j:.0f} J)</text>')
    svg = svg.replace('</svg>', annotation+'</svg>')
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(svg, encoding="utf-8")
    reduced = dict(date=report["date"],
        run=a.evidence.name, boundary=report["test_scope"],
        job_seconds=done, window_seconds=report["final_window"]["seconds"],
        mean_w=report["final_window"]["hardware_energy_mean_W"],
        observed_max_w=peak[1], sample_rate_hz=report["final_window"]["actual_sample_hz"],
        hardware_energy_Wh=report["final_window"]["hardware_energy_delta_J"]/3600,
        plotted_integral_Wh=sampled_energy, input_bytes=tx["confirmed_bytes"],
        active_work_seconds=done, active_work_integral_J=active_j, active_work_integral_Wh=active_wh,
        can_bitrate=500000, target_data_Bps=tx["profile"]["rate_Bps"], effective_data_Bps=tx["useful_Bps"],
        can_requests=report["final_window"]["can_sent"], can_responses=report["final_window"]["can_received"],
        input_and_outputs_verified=True, NN_executed=False,
        comparison=report["deltas"], full_session_max_w=report["full_final_session"]["sampled_max"]["power_W"],
        limitations=report["limitations"])
    a.out.with_suffix(".json").write_text(json.dumps(reduced,indent=2)+"\n",encoding="utf-8")
    print(json.dumps({k:v for k,v in reduced.items() if k not in ("comparison","limitations")},indent=2))


if __name__ == "__main__":
    main()
