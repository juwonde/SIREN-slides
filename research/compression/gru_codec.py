"""Lossless BZ2 hybrid codec of SIREN Model 3 with the F-ASTRA integer predictor (``gru_predictor``).

The packet layout follows the original reference format v1 (``SCBZ2\\x01\\r\\n``, little-endian header length, JSON
header, BZ2 auxiliary inputs, per block and channel the smallest of raw / temporal delta / GRU residual, BZ2 level 5,
ties raw -> delta -> gru). Two header fields differ: ``predictor`` names the integer predictor and ``model_sha256``
is the integer contract's hash, so a packet decodes only with the same contract. Both the payload encoder and the
ground decoder run the same integer predictor, so the reconstruction is exact on any machine.

``encode`` also accepts any prediction function (for example the float original) so compression ratios can be
compared with one codec.
"""
from __future__ import annotations

import bz2
import hashlib
import json
import struct
import zlib

import numpy as np

from .._validation import need

MAGIC = b"SCBZ2\x01\r\n"
PREDICTOR = "f-astra-integer-gru/v1"
I32 = np.iinfo(np.int32)


def _i32(values):
    values = np.asarray(values)
    need(values.dtype.kind in "iu", "count and residual values must be integers")
    need(not (np.any(values < I32.min) or np.any(values > I32.max)), "integer value exceeds int32")
    return values.astype("<i4")


def zigzag(values):
    r = _i32(values).astype(np.int64)
    return ((r << 1) ^ (r >> 63)).astype("<u4")


def unzigzag(values):
    u = np.asarray(values).astype(np.int64)
    return _i32((u >> 1) ^ -(u & 1))


def _json(value):
    return json.dumps(value, separators=(",", ":"), ensure_ascii=True).encode("utf-8")


def encode(q, aux, predictions, model_id, channels, scale, block_samples=1800, level=5, predictor=PREDICTOR,
           segment_steps=None):
    """Packet bytes and metrics for counts ``q`` (int [T, C]) whose next-step predictions are ``predictions``
    (int [T - 1, C], from the integer predictor or any other)."""
    q = _i32(q)
    aux = np.asarray(aux, "<f4")
    t_len, c = q.shape
    need(t_len >= 2 and aux.shape == (t_len, 6) and np.all(np.isfinite(aux)) and np.all(q >= 0), "invalid inputs")
    need(np.asarray(predictions).shape == (t_len - 1, c) and block_samples > 0 and 1 <= level <= 9, "invalid settings")
    n = t_len - 1
    delta = _i32(q[1:].astype(np.int64) - q[:-1].astype(np.int64))
    residual = _i32(q[1:].astype(np.int64) - np.asarray(predictions, np.int64))
    records, payloads, modes = [], [], {"raw": 0, "delta": 0, "gru": 0}
    for block, start in enumerate(range(0, n, block_samples)):
        end = min(start + block_samples, n)
        for ch in range(c):
            candidates = (("raw", bz2.compress(q[1 + start:1 + end, ch].astype("<i4").tobytes(), level)),
                          ("delta", bz2.compress(zigzag(delta[start:end, ch]).tobytes(), level)),
                          ("gru", bz2.compress(zigzag(residual[start:end, ch]).tobytes(), level)))
            options = []
            for mode, payload in candidates:
                record = dict(block=block, start=start, count=end - start, channel=ch, mode=mode, bytes=len(payload),
                              crc32=zlib.crc32(payload))
                options.append((len(payload) + len(_json(record)), record, payload))
            _, record, payload = min(options, key=lambda o: o[0])
            records.append(record)
            payloads.append(payload)
            modes[record["mode"]] += 1
    aux_payload = bz2.compress(aux[:-1].tobytes(), level)
    header = dict(version=1, codec="bz2", level=level, dtype="int32-le", samples=t_len, channels=list(channels),
                  scale=int(scale), model_sha256=model_id, predictor=predictor, block_samples=block_samples,
                  **({"segment_steps": int(segment_steps)} if segment_steps else {}),
                  initial_q=q[0].tolist(), aux_bytes=len(aux_payload), aux_crc32=zlib.crc32(aux_payload),
                  records=records, q_sha256=hashlib.sha256(q.tobytes()).hexdigest())
    h = _json(header)
    packet = MAGIC + struct.pack("<I", len(h)) + h + aux_payload + b"".join(payloads)
    metrics = dict(count_raw_bytes=int(q.nbytes), packet_bytes=len(packet), count_to_packet_ratio=q.nbytes / len(packet),
                   auxiliary_payload_bytes=len(aux_payload), selected_count_payload_bytes=sum(map(len, payloads)),
                   header_bytes=len(MAGIC) + 4 + len(h), mode_blocks=modes, samples=t_len, channels=c,
                   block_samples=block_samples, predictor=predictor, model_sha256=model_id)
    return packet, metrics


CANDIDATES_MAGIC = b"SCGC"


def candidates(q, aux, block_samples=1800, level=5):
    """The C encoder's pre stage (``--stage pre``, a daily window: no predictions yet): per block and channel the
    smaller of the raw and delta payloads with its record (ties raw), the compressed auxiliary inputs and the counts'
    SHA-256. ``encode`` then picks the gru payload only where it is smaller still, so the final stage's packet is the
    one-stage packet byte for byte."""
    q = _i32(q)
    aux = np.asarray(aux, "<f4")
    t_len, c = q.shape
    need(t_len >= 2 and aux.shape == (t_len, 6) and np.all(np.isfinite(aux)) and np.all(q >= 0), "invalid inputs")
    need(block_samples > 0 and 1 <= level <= 9, "invalid settings")
    n = t_len - 1
    delta = _i32(q[1:].astype(np.int64) - q[:-1].astype(np.int64))
    aux_payload = bz2.compress(aux[:-1].tobytes(), level)
    out = [CANDIDATES_MAGIC, struct.pack("<5I", 1, t_len, c, block_samples, level),
           hashlib.sha256(q.tobytes()).hexdigest().encode("ascii"),
           struct.pack("<2I", len(aux_payload), zlib.crc32(aux_payload)), aux_payload]
    for block, start in enumerate(range(0, n, block_samples)):
        end = min(start + block_samples, n)
        for ch in range(c):
            options = []
            for code, (mode, data) in enumerate((("raw", q[1 + start:1 + end, ch].astype("<i4").tobytes()),
                                                 ("delta", zigzag(delta[start:end, ch]).tobytes()))):
                payload = bz2.compress(data, level)
                record = dict(block=block, start=start, count=end - start, channel=ch, mode=mode, bytes=len(payload),
                              crc32=zlib.crc32(payload))
                options.append((len(payload) + len(_json(record)), code, payload))
            _, code, payload = min(options, key=lambda o: o[0])
            out += [struct.pack("<BII", code, zlib.crc32(payload), len(payload)), payload]
    return b"".join(out)


def encode_integer(contract, q, aux, block_samples=1800, level=5):
    """Encode with the integer predictor of ``contract``."""
    from . import gru_predictor
    predictions, _ = gru_predictor.integer_predictions(contract, q, aux)
    return encode(q, aux, predictions, contract["sha256"], contract["source"]["channels"], contract["source"]["scale"],
                  block_samples, level)


NATIVE_SOURCE = "f_astra_gru_encode.c"          # the PS encoder in C (same packet, byte for byte)


def codebook_bytes(contract):
    """The codebooks of ``contract`` for the C encoder (``--codebooks``): per channel a uint32 LE count and that many
    int32 LE values."""
    out = bytearray()
    for book in contract["codebooks"]:
        values = _i32(book)
        out += struct.pack("<I", len(values)) + values.tobytes()
    return bytes(out)


def native_inputs(folder, q, aux, model_id, channels, scale, predictions=None, index=None, codebooks=None,
                  block_samples=1800, level=5):
    """Write the C encoder's input files into ``folder`` and return (its arguments with file names relative to
    ``folder``, the predictions): the predictions are given (``predictions``, int [T - 1, C]) or are the GRU tail's
    codebook indices (``index`` with the ``codebooks`` of the contract, as on the board)."""
    from pathlib import Path
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    q = _i32(q)
    aux = np.asarray(aux, "<f4")
    t_len, c = q.shape
    (folder / "q.i32").write_bytes(q.tobytes())
    (folder / "aux.f32").write_bytes(aux.tobytes())
    args = ["--q", "q.i32", "--aux", "aux.f32", "--samples", str(t_len), "--channels", str(c),
            "--channels-json", _json(list(channels)).decode("ascii"), "--scale", str(int(scale)), "--model-sha256",
            model_id, "--block", str(block_samples), "--level", str(level), "--out", "packet.bin", "--metrics",
            "metrics.json"]
    if index is not None:
        need(codebooks is not None, "indices need the codebooks")
        index = _i32(index)
        books = [np.asarray(b, np.int64) for b in codebooks]
        predictions = np.stack([books[ch][index[:, ch]] for ch in range(c)], axis=1)
        (folder / "index.i32").write_bytes(index.tobytes())
        (folder / "codebooks.bin").write_bytes(codebook_bytes(dict(codebooks=codebooks)))
        args += ["--index", "index.i32", "--codebooks", "codebooks.bin"]
    else:
        (folder / "pred.i32").write_bytes(_i32(predictions).tobytes())
        args += ["--pred", "pred.i32"]
    return args, predictions


def native_check(run, folder, q, aux, model_id, channels, scale, predictions=None, index=None, codebooks=None,
                 block_samples=1800, level=5):
    """The C encoder's packet against ``encode`` on the same inputs. ``run(args)`` runs the encoder with its working
    directory at ``folder`` (input file names are relative) and returns its exit status. Returns the comparison, the
    packet and the Python metrics."""
    from pathlib import Path
    folder = Path(folder)
    args, predictions = native_inputs(folder, q, aux, model_id, channels, scale, predictions, index, codebooks,
                                      block_samples, level)
    status = run(args)
    need(status == 0, "the C encoder failed (exit %s)" % status)
    native = (folder / "packet.bin").read_bytes()
    result, packet, metrics = compare_packet(native, q, aux, predictions, model_id, channels, scale, block_samples,
                                             level)
    result["native_metrics"] = json.loads((folder / "metrics.json").read_text())
    return result, native, metrics


def compare_packet(native, q, aux, predictions, model_id, channels, scale, block_samples=1800, level=5,
                   segment_steps=None):
    """A packet of the C encoder against ``encode`` on the same inputs: (comparison, Python packet, metrics)."""
    packet, metrics = encode(q, aux, predictions, model_id, channels, scale, block_samples, level,
                             segment_steps=segment_steps)
    first = next((k for k, (a, b) in enumerate(zip(native, packet)) if a != b), None)
    if first is None and len(native) != len(packet):
        first = min(len(native), len(packet))
    return dict(equal=native == packet, native_bytes=len(native), python_bytes=len(packet),
                native_sha256=hashlib.sha256(native).hexdigest(), python_sha256=hashlib.sha256(packet).hexdigest(),
                first_difference=first, mode_blocks=metrics["mode_blocks"],
                count_to_packet_ratio=metrics["count_to_packet_ratio"]), packet, metrics


def _decompress_exact(payload, expected):
    d = bz2.BZ2Decompressor()
    data = d.decompress(payload, max_length=expected + 1)
    need(len(data) == expected and d.eof and not d.unused_data, "BZ2 payload size or stream mismatch")
    return data


def decode(packet, contract):
    """Exact counts q of a packet encoded with the integer predictor of ``contract`` (the ground decoder)."""
    from . import gru_predictor
    need(packet.startswith(MAGIC) and len(packet) >= len(MAGIC) + 4, "invalid packet magic")
    offset = len(MAGIC) + 4
    size = struct.unpack("<I", packet[len(MAGIC):offset])[0]
    need(size <= len(packet) - offset, "truncated header")
    h = json.loads(packet[offset:offset + size])
    offset += size
    need(h["version"] == 1 and h["codec"] == "bz2" and h["dtype"] == "int32-le" and h.get("predictor") == PREDICTOR
         and h["model_sha256"] == contract["sha256"] and h["channels"] == contract["source"]["channels"]
         and h["scale"] == contract["source"]["scale"], "packet and integer predictor contract differ")
    n, c, block_samples = int(h["samples"]) - 1, len(h["channels"]), int(h["block_samples"])
    segment = int(h.get("segment_steps", 0))         # the predictor's state is zero again every that many steps
    need(n >= 1 and block_samples >= 1 and segment >= 0, "invalid sample, block or segment counts")

    def take(length, crc):
        nonlocal offset
        need(isinstance(length, int) and 0 <= length <= len(packet) - offset, "truncated payload")
        data = packet[offset:offset + length]
        offset += length
        need(zlib.crc32(data) == crc, "payload CRC32 mismatch")
        return data

    aux = np.frombuffer(_decompress_exact(take(h["aux_bytes"], h["aux_crc32"]), n * 24), "<f4").reshape(n, 6)
    q = np.empty((n + 1, c), np.int64)
    q[0] = _i32(h["initial_q"])
    records = iter(h["records"])
    p = gru_predictor.Integer(contract)
    for block, start in enumerate(range(0, n, block_samples)):
        count = min(block_samples, n - start)
        arrays, modes = [], []
        for ch in range(c):
            r = next(records, None)
            need(r is not None and (r["block"], r["start"], r["count"], r["channel"]) == (block, start, count, ch)
                 and r["mode"] in ("raw", "delta", "gru"), "invalid record order or mode")
            data = _decompress_exact(take(r["bytes"], r["crc32"]), count * 4)
            values = np.frombuffer(data, "<i4" if r["mode"] == "raw" else "<u4")
            arrays.append(values.astype(np.int64) if r["mode"] == "raw" else unzigzag(values).astype(np.int64))
            modes.append(r["mode"])
        for i in range(count):
            t = start + i
            if segment and t % segment == 0:
                p.reset()
            _, prediction = p.step(q[t], aux[t])            # the state advances in every mode
            for ch in range(c):
                base = q[t, ch] if modes[ch] == "delta" else prediction[ch] if modes[ch] == "gru" else 0
                q[t + 1, ch] = arrays[ch][i] + base
    need(next(records, None) is None and offset == len(packet), "extra records or bytes")
    q = _i32(q)
    need(hashlib.sha256(q.tobytes()).hexdigest() == h["q_sha256"], "exact reconstruction hash mismatch")
    return q, h
