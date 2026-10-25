"""SIREN Model 3 evaluation on real observation days (G1, ``docs/gru-integration-design.md``): for each day the float
original and the integer predictor encode with the same lossless codec; the integer packet is decoded and must
restore every count exactly. Meant to run where the data are (the remote server); only the metrics leave.

``read_day`` follows the original NetCDF adapter: the checkpoint's 28 count variables, exact at the checkpoint scale,
and six auxiliary inputs (sin / cos of latitude and longitude, direction, orbital phase).
"""
from __future__ import annotations

import hashlib
import json
import time

import numpy as np

from .._validation import need


def read_day(path, channels, scale):
    """(q int64 [T, C], auxiliary float32 [T, 6]) of one day file."""
    from netCDF4 import Dataset
    columns = []
    with Dataset(str(path), "r") as ds:
        for channel in channels:
            raw = ds[channel][:]
            need(not (np.ma.isMaskedArray(raw) and np.any(np.ma.getmaskarray(raw))), "masked value in " + channel)
            values = np.asarray(raw, np.float32).reshape(-1)
            need(np.all(np.isfinite(values)) and not np.any(values < 0), "invalid counts in " + channel)
            q = np.rint(values.astype(np.float64) * scale).astype(np.int64)
            need(np.array_equal((q.astype(np.float64) / scale).astype(np.float32), values), "counts not exact at the scale: " + channel)
            columns.append(q)
        lat = np.asarray(ds["lat"][:], np.float32).reshape(-1)
        lon = np.asarray(ds["lon"][:], np.float32).reshape(-1)
        raw_direction = np.asarray(ds["sat_direction"][:], np.float32).reshape(-1)
    q = np.column_stack(columns)
    need(len(q) == len(lat) == len(lon) == len(raw_direction), "coordinate length mismatch")
    return q, auxiliary(lat, lon, raw_direction)


def auxiliary(lat, lon, raw_direction):
    """Auxiliary inputs (float32 [T, 6]: sin and cos of latitude and longitude, direction, orbit phase) of float32
    positions; a constant direction column is replaced by the sign of the latitude gradient (the board PS forms the
    same values: f_astra_weekly_prep.c)."""
    lat, lon = np.asarray(lat, np.float32), np.asarray(lon, np.float32)
    raw_direction = np.asarray(raw_direction, np.float32)
    derived = (np.gradient(lat.astype(np.float64)) < 0).astype(np.float32)
    direction = raw_direction if len(np.unique(raw_direction)) > 1 else derived
    phase = np.mod(np.where(direction < 0.5, (lat.astype(np.float64) + 90.0) / 360.0,
                            0.5 + (90.0 - lat.astype(np.float64)) / 360.0), 1.0).astype(np.float32)
    la, lo = np.deg2rad(lat.astype(np.float64)), np.deg2rad(lon.astype(np.float64))
    return np.column_stack([np.sin(la), np.cos(la), np.sin(lo), np.cos(lo), direction, phase]).astype(np.float32)


def evaluate_day(params, contract, q, aux, block_samples=1800, level=5, torch_path=True):
    """Float and integer predictor compression of one day with the same codec, and the integer round trip."""
    from . import gru_codec, gru_predictor
    started = time.perf_counter()
    fp, fi = gru_predictor.float_predictions(params, q, aux, torch_path)
    t_float = time.perf_counter() - started
    started = time.perf_counter()
    ip, ii = gru_predictor.integer_predictions(contract, q, aux)
    t_int = time.perf_counter() - started
    _, mf = gru_codec.encode(q, aux, fp, params["checkpoint_sha256"], params["channels"], params["scale"], block_samples, level,
                             predictor="float-original")
    packet, mi = gru_codec.encode(q, aux, ip, contract["sha256"], contract["source"]["channels"], contract["source"]["scale"],
                                  block_samples, level)
    restored, _ = gru_codec.decode(packet, contract)
    target = np.asarray(q, np.int64)[1:]
    d = np.abs(ii - fi)
    return dict(samples=int(len(q)), channels=int(q.shape[1]),
                ratio_float=mf["count_to_packet_ratio"], ratio_integer=mi["count_to_packet_ratio"],
                ratio_change_percent=100.0 * (mi["count_to_packet_ratio"] / mf["count_to_packet_ratio"] - 1.0),
                packet_bytes_float=mf["packet_bytes"], packet_bytes_integer=mi["packet_bytes"],
                modes_float=mf["mode_blocks"], modes_integer=mi["mode_blocks"],
                integer_exact=bool(np.array_equal(restored, q)), index_equal=float((d == 0).mean()),
                index_within_1=float((d <= 1).mean()), residual_mean_abs_float=float(np.abs(target - fp).mean()),
                residual_mean_abs_integer=float(np.abs(target - ip).mean()), seconds_float=t_float, seconds_integer=t_int,
                q_sha256=hashlib.sha256(np.asarray(q, "<i4").tobytes()).hexdigest())


def evaluate(checkpoint, contract_path, day_files, output, block_samples=1800, level=5):
    """Evaluate every day file; write the metrics JSON to ``output`` (metrics only, no data)."""
    from . import gru_predictor
    params = gru_predictor.load_checkpoint(checkpoint)
    with open(contract_path, encoding="utf-8") as handle:
        contract = json.load(handle)
    need(contract["source"]["checkpoint_sha256"] == params["checkpoint_sha256"], "the contract is not of this checkpoint")
    days = []
    for path in day_files:
        q, aux = read_day(path, params["channels"], params["scale"])
        row = evaluate_day(params, contract, q, aux, block_samples, level)
        row["day"] = str(path).replace("\\", "/").rsplit("/", 1)[-1]
        days.append(row)
    total_raw = sum(d["samples"] * d["channels"] * 4 for d in days)
    summary = dict(days=len(days), integer_exact_all=all(d["integer_exact"] for d in days),
                   ratio_float=total_raw / max(1, sum(d["packet_bytes_float"] for d in days)),
                   ratio_integer=total_raw / max(1, sum(d["packet_bytes_integer"] for d in days)))
    summary["ratio_change_percent"] = 100.0 * (summary["ratio_integer"] / summary["ratio_float"] - 1.0)
    result = dict(schema="f-astra/gru-evaluation/v1", checkpoint_sha256=params["checkpoint_sha256"],
                  contract_sha256=contract["sha256"], bits=contract["bits"], summary=summary, days=days)
    with open(output, "w", encoding="utf-8") as handle:
        json.dump(result, handle, indent=2)
    return result
