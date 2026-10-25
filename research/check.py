"""CPU checks of model causality, channel symmetry and lossless reconstruction.

Uses deterministic synthetic inputs, not the scientific evaluation dataset.
Optional executables also check C/Python packet and preprocessing equivalence.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile

import numpy as np
import torch

from .bus.features import anonymous_window_features
from .bus.model import AnonymousModernTCN, AnonymousModernTCNConfig
from .compression import gru_codec, gru_predictor
from .pd.model import V9EdgeModernTCNConfig, V9EdgeModernTCNStudent
from .pd.preprocessing import make_v9_features, make_v9_multistream_weak_onset_features
from .preprocessing import poes_sem_packets as sem


def check_models():
    torch.manual_seed(7)
    torch.set_num_threads(2)
    rng = np.random.default_rng(7)
    counts = rng.normal(size=(1, 64, 4)).astype(np.float32)
    valid = np.ones_like(counts)
    valid[:, 16:20, 1] = 0
    masked = make_v9_features(counts, valid)
    assert np.all(masked[:, 16:20, 1] == 0)
    features = make_v9_multistream_weak_onset_features(counts, valid)
    changed = counts.copy()
    changed[:, 40:] += 10
    later = make_v9_multistream_weak_onset_features(changed, valid)
    np.testing.assert_array_equal(features[:, :40], later[:, :40])
    pd = V9EdgeModernTCNStudent(V9EdgeModernTCNConfig(
        input_channels=features.shape[-1], d_model=16,
        kernel_sizes=(7, 15, 31), dropout=0,
    )).eval()
    with torch.inference_mode():
        output = pd(torch.from_numpy(features))
        altered = pd(torch.from_numpy(later))
    assert tuple(output.shape) == (1, 64)
    torch.testing.assert_close(output[:, :40], altered[:, :40], rtol=0, atol=0)

    hk = rng.normal(size=(64, 5)).astype(np.float32)
    hk_features = anonymous_window_features(hk)
    bus = AnonymousModernTCN(AnonymousModernTCNConfig(
        hidden_channels=8, temporal_blocks=2, dropout=0,
    )).eval()
    values = torch.from_numpy(hk_features[None])
    with torch.inference_mode():
        window, sequence = bus(values)
        permuted_window, permuted_sequence = bus(values[:, [2, 0, 4, 1, 3]])
    assert tuple(window.shape) == (1,)
    assert tuple(sequence.shape) == (1, 64)
    torch.testing.assert_close(window, permuted_window, rtol=1e-5, atol=1e-6)
    torch.testing.assert_close(sequence, permuted_sequence, rtol=1e-5, atol=1e-6)
    return {"pd_causal": True, "bus_channel_permutation": True}


def codec_fixture():
    rng = np.random.default_rng(11)
    channels, hidden = 3, 8
    params = {
        "channels": ["count_0", "count_1", "count_2"], "hidden": hidden,
        "scale": 1, "mean": np.full(channels, 2, np.float32),
        "std": np.ones(channels, np.float32),
        "codebooks": [np.arange(0, 128, 4, dtype=np.int64) for _ in range(channels)],
        "checkpoint_sha256": hashlib.sha256(b"synthetic-check-only").hexdigest(),
    }
    for name, shape in {
        "w_ih": (3 * hidden, channels + 6), "w_hh": (3 * hidden, hidden),
        "b_ih": (3 * hidden,), "b_hh": (3 * hidden,),
        "w_o": (channels, hidden), "b_o": (channels,),
    }.items():
        params[name] = rng.normal(0, 0.04, shape)
    contract = gru_predictor.build(params)
    q = rng.integers(0, 128, (65, channels), dtype=np.int32)
    aux = rng.uniform(-1, 1, (65, 6)).astype(np.float32)
    return params, contract, q, aux


def check_codec(encoder=None):
    params, contract, q, aux = codec_fixture()
    predictions, _ = gru_predictor.integer_predictions(contract, q, aux)
    packet, _ = gru_codec.encode(q, aux, predictions, contract["sha256"],
                                params["channels"], params["scale"], block_samples=16)
    restored, _ = gru_codec.decode(packet, contract)
    np.testing.assert_array_equal(restored, q)
    segmented, _ = gru_predictor.integer_predictions(contract, q, aux, segment_steps=16)
    segmented_packet, _ = gru_codec.encode(
        q, aux, segmented, contract["sha256"], params["channels"], params["scale"],
        block_samples=16, segment_steps=16,
    )
    np.testing.assert_array_equal(gru_codec.decode(segmented_packet, contract)[0], q)
    for damaged in (packet[:-1], packet + b"extra"):
        try:
            gru_codec.decode(damaged, contract)
        except (ValueError, OSError, EOFError):
            pass
        else:
            raise AssertionError("damaged packet accepted")
    if encoder:
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            q.astype("<i4").tofile(directory / "q.i32")
            aux.astype("<f4").tofile(directory / "aux.f32")
            predictions.astype("<i4").tofile(directory / "pred.i32")
            result = subprocess.run([
                str(Path(encoder).resolve()), "--q", str(directory / "q.i32"),
                "--aux", str(directory / "aux.f32"), "--pred", str(directory / "pred.i32"),
                "--samples", str(len(q)), "--channels", str(q.shape[1]),
                "--channels-json", json.dumps(params["channels"], separators=(",", ":")),
                "--scale", "1", "--model-sha256", contract["sha256"],
                "--block", "16", "--level", "5", "--out", str(directory / "packet.bin"),
            ], check=True, capture_output=True, text=True)
            assert result.returncode == 0
            assert (directory / "packet.bin").read_bytes() == packet
    return {"lossless_roundtrip": True, "segmented_roundtrip": True,
            "damaged_packets_rejected": True, "c_encoder_equal": True if encoder else None}


def check_preprocessing(preprocessor=None):
    times = np.asarray([49000, 51000, 101000], np.uint64)
    positions = np.asarray([[10, 20, 0]] * 3, "<f4")
    values = np.asarray([[1, 2], [3, 4], [5, 6]], "<f4")
    mask = np.asarray([1, 3, 3], np.uint64)
    schema = {"level": "l1a", "channels": [{"name": "a", "units": "#/s"},
                                              {"name": "b", "units": "#/s"}]}
    packet, listed = sem.pack_records(times, positions, values, mask, np.zeros(3, "u4"), schema)
    bins = sem.reduce_list(listed)
    assert sem.HEADER.unpack_from(bins)[4] == 3
    assert struct.unpack_from("<2H", bins, 64 + 28) == (1, 0)
    assert np.isnan(struct.unpack_from("<2f", bins, 64 + 20)[1])
    if preprocessor:
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            (directory / "replay.bin").write_bytes(packet)
            prefix = directory / "out"
            subprocess.run([str(Path(preprocessor).resolve()), "replay",
                            str(directory / "replay.bin"), str(prefix)],
                           check=True, capture_output=True, text=True)
            assert Path(str(prefix) + ".list.bin").read_bytes() == listed
            assert Path(str(prefix) + ".bins.bin").read_bytes() == bins
    return {"utc_bins_and_validity": True,
            "c_preprocessor_equal": True if preprocessor else None}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--encoder", type=Path)
    parser.add_argument("--preprocessor", type=Path)
    args = parser.parse_args()
    result = {**check_models(), **check_codec(args.encoder),
              **check_preprocessing(args.preprocessor)}
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
