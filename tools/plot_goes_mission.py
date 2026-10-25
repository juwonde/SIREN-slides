"""Redraw the original October 1989 GOES-7 example from NOAA measurements.

Requires numpy and matplotlib. Source data are cached under ignored build/.
No interpolation or smoothing is applied. Missing/nonpositive values leave gaps.
"""
from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
import hashlib
import io
import json
from pathlib import Path
import urllib.request

import matplotlib
matplotlib.use("Agg")
import matplotlib.dates as mdates
import matplotlib.pyplot as plt
from matplotlib.ticker import FixedLocator, FuncFormatter, NullLocator
import numpy as np


SOURCE_URL = (
    "https://www.ncei.noaa.gov/data/goes-space-environment-monitor/access/"
    "avg/1989/10/goes07/csv/g07_eps_5m_19891001_19891031.csv"
)
START = datetime(1989, 10, 18)
END = datetime(1989, 11, 1)
CHANNELS = [
    ("p3_flux_ic", "> 10 MeV", "#1f4f83", 2.1, 1.0),
    ("p4_flux_ic", "> 30 MeV", "#738b9b", 1.2, 0.9),
    ("p7_flux_ic", "> 100 MeV", "#b3bcc4", 1.2, 1.0),
]


def read_source(path: Path):
    text = path.read_text(encoding="utf-8")
    header = text.index("time_tag,")
    rows = list(csv.DictReader(io.StringIO(text[header:])))
    selected = []
    for row in rows:
        stamp = datetime.fromisoformat(row["time_tag"])
        if START <= stamp < END:
            selected.append((stamp, row))
    times = np.asarray([t for t, _ in selected])
    values = {}
    for key, *_ in CHANNELS:
        raw = np.asarray([float(row[key]) for _, row in selected])
        values[key] = np.where(raw > 0, raw, np.nan)
    if len(selected) != 4032:
        raise ValueError(f"Expected 4032 five-minute records, found {len(selected)}")
    if any((b - a).total_seconds() != 300 for a, b in zip(times, times[1:])):
        raise ValueError("Source timestamps are not spaced five minutes apart")
    return times, values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, help="Use an existing NOAA CSV")
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output_dir or root / "assets" / "mission"
    output.mkdir(parents=True, exist_ok=True)
    source = args.source or root / "build" / "data" / SOURCE_URL.rsplit("/", 1)[1]
    if not source.exists():
        source.parent.mkdir(parents=True, exist_ok=True)
        with urllib.request.urlopen(SOURCE_URL, timeout=60) as response:
            source.write_bytes(response.read())
    times, values = read_source(source)
    plt.rcParams.update({
        "font.family": "serif",
        "font.serif": ["Cambria", "Georgia", "DejaVu Serif"],
        "font.size": 17,
        "axes.labelcolor": "#363b47",
        "text.color": "#243559",
        "xtick.color": "#555d6a",
        "ytick.color": "#555d6a",
        "svg.fonttype": "path",
        "svg.hashsalt": "goes-7-october-1989",
        "path.simplify": False,
    })
    fig, ax = plt.subplots(figsize=(10.5, 4.15), dpi=100)
    fig.patch.set_alpha(0)
    ax.set_facecolor("none")
    fig.subplots_adjust(left=.095, right=.985, bottom=.18, top=.83)
    ax.set_yscale("log")
    ax.set_ylim(.025, 1e5)
    ax.set_xlim(START, END)
    ax.yaxis.set_major_locator(FixedLocator([.1, 10, 1000, 100000]))
    labels = {.1: "0.1", 10: "10", 1000: "1k", 100000: "100k"}
    ax.yaxis.set_major_formatter(FuncFormatter(lambda v, _: labels.get(v, "")))
    ax.yaxis.set_minor_locator(NullLocator())
    ax.xaxis.set_major_locator(mdates.DayLocator(bymonthday=[18, 21, 24, 27, 30]))
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%d Oct"))
    ax.tick_params(axis="both", length=0, pad=9, labelsize=17)
    for edge in ("top", "left", "right"):
        ax.spines[edge].set_visible(False)
    ax.spines["bottom"].set_color("#c6cdd4")
    ax.spines["bottom"].set_linewidth(.8)
    ax.grid(axis="y", color="#e5e9ed", linewidth=.7, zorder=0)
    # Render every five-minute sample. Matplotlib breaks lines at missing values.
    for key, label, color, width, opacity in reversed(CHANNELS):
        ax.plot(times, values[key], color=color, lw=width, alpha=opacity,
                label=label, solid_capstyle="round", zorder=3)
    handles, legend_labels = ax.get_legend_handles_labels()
    ax.legend(handles[::-1], legend_labels[::-1], frameon=False, ncol=3,
              loc="lower left", bbox_to_anchor=(-.005, 1.025), fontsize=16,
              handlelength=1.7, columnspacing=1.45, borderaxespad=0)
    ax.text(0, -.245, "1989 (UTC)", transform=ax.transAxes, fontsize=15,
            color="#555d6a", ha="left")
    ax.text(.995, -.245, "Proton flux · pfu · log scale", transform=ax.transAxes,
            fontsize=15, color="#555d6a", ha="right")
    svg = output / "goes7-oct1989-protons.svg"
    png = output / "goes7-oct1989-protons.png"
    fig.savefig(svg, transparent=True, metadata={"Date": None})
    svg.write_text("\n".join(line.rstrip() for line in svg.read_text(encoding="utf-8").splitlines()) + "\n", encoding="utf-8")
    fig.savefig(png, transparent=True, dpi=200, metadata={"Source": SOURCE_URL})
    plt.close(fig)
    metadata = {
        "source_url": SOURCE_URL,
        "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        "spacecraft": "GOES-7",
        "instrument": "EPS",
        "sample_interval_seconds": 300,
        "period_start_utc": START.isoformat() + "Z",
        "period_end_exclusive_utc": END.isoformat() + "Z",
        "records": len(times),
        "processing": "Positive corrected integral proton fluxes only; no smoothing, interpolation or resampling. Missing/nonpositive samples produce line gaps.",
        "units": "pfu = protons / (cm^2 s sr)",
        "y_axis": "logarithmic",
        "purpose": "Historical observation example for the PADO/CREST/SIREN naming story; not a SIREN model output or PADO-SAT measurement.",
        "channels": {},
    }
    for key, label, *_ in CHANNELS:
        vals = values[key]
        peak = int(np.nanargmax(vals))
        metadata["channels"][key] = {
            "label": label,
            "valid_records": int(np.isfinite(vals).sum()),
            "missing_records": int(np.isnan(vals).sum()),
            "peak_flux_pfu": float(vals[peak]),
            "peak_time_utc": times[peak].isoformat() + "Z",
        }
    (output / "goes7-oct1989-protons.source.json").write_text(
        json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"svg": str(svg), "png": str(png), "records": len(times),
                      "channels": metadata["channels"]}, indent=2))


if __name__ == "__main__":
    main()
