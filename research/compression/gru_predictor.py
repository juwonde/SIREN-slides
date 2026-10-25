"""SIREN Model 3 (GRU lossless-compression predictor) as an integer predictor (``docs/gru-integration-design.md``).

The float original (``demisodapple/SIREN_MODEL3_COMP``) predicts the next count of 28 channels with one GRU layer
(input 28 normalized log counts + 6 auxiliary values, hidden 64), a Linear 64 -> 28 and a sigmoid read as a codebook
position. Lossless decoding needs the ground to reproduce the payload's predictions bit for bit, so payload PL and
ground share this integer predictor: every constant is computed once (float64) when the contract is built and stored
as integers; a step is integer comparisons and integer arithmetic only.

One step, inputs q[t] (int counts x scale) and aux[t] (float32 x 6), state h (B_h-bit codes):
  x codes      count channel: thresholds on q (monotonic inverse of (log1p(q / scale) - mean) / std);
               auxiliary: float32 thresholds (an IEEE comparison is the same everywhere)
  accumulators acc = M . [x codes; h codes]   (int8 M: 384 rows = input part of r, z, n and hidden part of r, z, n,
               each row with its own scale; input scales and zero points folded into M and the biases)
  fixed point  A = requant(acc) per row with its bias, at 2^-F
  r, z         one shared sigmoid threshold table on A_x + A_h -> B_g-bit codes, value code / 2^B_g
  n            pre = A_nx + rne(g_r * A_nh / 2^B_g); one shared tanh threshold table -> B_h-bit codes,
               value code / 2^(B_h - 1)
  h'           n + rne(g_z * (h - n) / 2^B_g), clipped to B_h bits
  index        thresholds on Wo . h' accumulators (sigmoid^-1((k - 1/2) / (L - 1))), prediction = codebook[index]

Delta inputs (``extend_delta``, an inference optimization trained with ``train_delta``): a contract with
``source.delta_inputs`` = D takes D more input codes, x(t) - x(t-1) of its first D x codes (0 at the day's first step,
not reset with the hidden state), in the input rows after the x codes; the requant constants, the hidden rows, the
thresholds and the output layer stay, so the FINN tail keeps its parameters and the G4 engine fills its x lanes.
"""
from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path

import numpy as np

from .._validation import need

SCHEMA = "f-astra/gru-predictor-contract/v1"
AUXILIARY = 6


# ---------------------------------------------------------------- checkpoint (float original)

def load_checkpoint(path):
    """The float model of a SIREN Model 3 checkpoint, read with torch ``weights_only`` and an explicit allowlist of
    the numpy objects it holds (no arbitrary code). Returns numpy float64 arrays and metadata."""
    import _codecs
    import collections
    import torch
    try:
        from numpy._core.multiarray import _reconstruct
    except ImportError:                                   # numpy < 2
        from numpy.core.multiarray import _reconstruct
    allowed = [_codecs.encode, collections.OrderedDict, np.dtype, np.ndarray, _reconstruct]
    allowed += [type(np.dtype(t)) for t in ("float32", "float64", "int32", "int64", "uint8")]
    torch.serialization.add_safe_globals(allowed)
    ck = torch.load(str(path), map_location="cpu", weights_only=True)
    sd = ck["state_dict"]
    f = lambda k: sd[k].detach().cpu().numpy().astype(np.float64)
    params = dict(w_ih=f("gru.weight_ih_l0"), w_hh=f("gru.weight_hh_l0"), b_ih=f("gru.bias_ih_l0"),
                  b_hh=f("gru.bias_hh_l0"), w_o=f("output.weight"), b_o=f("output.bias"),
                  mean=np.asarray(ck["mean"], np.float32), std=np.asarray(ck["std"], np.float32),
                  scale=int(ck["config"]["quantization_scale"]), hidden=int(ck["config"]["hidden_dim"]),
                  channels=[str(c) for c in ck["channels"]],
                  codebooks=[np.asarray(b, np.int64) for b in ck["codebooks"]],
                  checkpoint_sha256=hashlib.sha256(Path(path).read_bytes()).hexdigest())
    c, hd = len(params["channels"]), params["hidden"]
    need(params["w_ih"].shape == (3 * hd, c + AUXILIARY) and params["w_hh"].shape == (3 * hd, hd)
         and params["w_o"].shape == (c, hd) and len(params["codebooks"]) == c, "unexpected checkpoint shapes")
    need(all(len(b) >= 2 and np.all(np.diff(b) > 0) for b in params["codebooks"]), "codebooks must be increasing")
    return params


def _sigmoid(v):
    return 1.0 / (1.0 + np.exp(-v))


def float_predictions(params, q, aux, torch_path=True):
    """Predictions of the float original for q[1:] (int64 [T-1, C]) and their codebook indices. ``torch_path``
    runs the same float32 PyTorch GRU as the original (our own module built from the weights); otherwise numpy
    float64."""
    q = np.asarray(q, np.int64)
    aux = np.asarray(aux, np.float32)
    c, hd = len(params["channels"]), params["hidden"]
    counts = (q.astype(np.float64) / params["scale"]).astype(np.float32)
    normalized = (np.log1p(counts) - params["mean"]) / params["std"]
    x = np.concatenate([normalized, aux], axis=1).astype(np.float32)[:-1]
    if torch_path:
        import torch
        gru = torch.nn.GRU(c + AUXILIARY, hd, num_layers=1, batch_first=True)
        out = torch.nn.Linear(hd, c)
        with torch.no_grad():
            for name, key in (("weight_ih_l0", "w_ih"), ("weight_hh_l0", "w_hh"), ("bias_ih_l0", "b_ih"), ("bias_hh_l0", "b_hh")):
                getattr(gru, name).copy_(torch.from_numpy(params[key].astype(np.float32)))
            out.weight.copy_(torch.from_numpy(params["w_o"].astype(np.float32)))
            out.bias.copy_(torch.from_numpy(params["b_o"].astype(np.float32)))
            h, fractions = None, []
            for t in range(len(x)):                       # one step at a time, as the original runtime does
                seq, h = gru(torch.from_numpy(x[t][None, None]), h)
                fractions.append(torch.sigmoid(out(seq))[0, 0].numpy())
        fraction = np.stack(fractions).astype(np.float32)
    else:
        h = np.zeros(hd)
        fraction = np.empty((len(x), c))
        for t in range(len(x)):
            gi = params["w_ih"] @ x[t] + params["b_ih"]
            gh = params["w_hh"] @ h + params["b_hh"]
            r = _sigmoid(gi[:hd] + gh[:hd])
            z = _sigmoid(gi[hd:2 * hd] + gh[hd:2 * hd])
            n = np.tanh(gi[2 * hd:] + r * gh[2 * hd:])
            h = (1 - z) * n + z * h
            fraction[t] = _sigmoid(params["w_o"] @ h + params["b_o"])
    index = np.stack([np.clip(np.rint(fraction[:, i] * (len(b) - 1)), 0, len(b) - 1).astype(np.int64)
                      for i, b in enumerate(params["codebooks"])], axis=1)
    pred = np.stack([params["codebooks"][i][index[:, i]] for i in range(c)], axis=1)
    return pred, index


# ---------------------------------------------------------------- contract

def _count_range(params):
    """Per channel normalized range covered by its codebook (the values training saw)."""
    lo = np.array([(math.log1p(float(b[0]) / params["scale"]) - float(m)) / float(s)
                   for b, m, s in zip(params["codebooks"], params["mean"], params["std"])])
    hi = np.array([(math.log1p(float(b[-1]) / params["scale"]) - float(m)) / float(s)
                   for b, m, s in zip(params["codebooks"], params["mean"], params["std"])])
    return lo, hi


def _count_thresholds(params, mid, step, bits):
    """Per channel int64 thresholds T_k (k = 1 .. 2^bits - 1) on q: code = #(q >= T_k) - 2^(bits - 1) equals
    clip(round_half_up((v(q) - mid) / step)), v(q) = (log1p(q / scale) - mean) / std."""
    half = 1 << (bits - 1)
    out = []
    for c in range(len(params["channels"])):
        mean, std = float(params["mean"][c]), float(params["std"][c])
        row = []
        for k in range(1, 2 * half):
            v = mid[c] + (k - half - 0.5) * step[c]          # code >= k - half  <=>  v(q) >= v
            q = params["scale"] * math.expm1(v * std + mean)
            t = max(0, math.ceil(q - 1e-9)) if q > 0 else 0
            row.append(min(int(t), (1 << 62)))
        out.append(row)
    return out


def _aux_thresholds(bits):
    """float32 thresholds of the auxiliary inputs: sin / cos in [-1, 1], direction {0, 1}, phase [0, 1)."""
    half = 1 << (bits - 1)
    rows, steps, mids = [], [], []
    for lo, hi in ((-1.0, 1.0),) * 4 + ((0.0, 1.0), (0.0, 1.0)):
        mid, step = (lo + hi) / 2, (hi - lo) / (2 * half - 2)
        rows.append([float(np.float32(mid + (k - half - 0.5) * step)) for k in range(1, 2 * half)])
        steps.append(step)
        mids.append(mid)
    return rows, np.array(mids), np.array(steps)


def _quantize_rows(m):
    scale = np.maximum(np.abs(m).max(axis=1), 1e-12) / 127.0
    return np.clip(np.rint(m / scale[:, None]), -127, 127).astype(np.int64), scale


def _requant(scale, bias, fixed_bits, precision=15):
    """Per row: A = rne((acc * multiplier + offset) / 2^shift) ~ (scale * acc + bias) * 2^fixed_bits, with the
    multiplier normalized to [2^(precision - 1), 2^precision)."""
    mul, add, shift = [], [], []
    for s_, b_ in zip(scale, bias):
        target = float(s_) * (1 << fixed_bits)
        sh = max(0, precision - 1 - math.floor(math.log2(target))) if target > 0 else precision
        mul.append(int(round(target * (1 << sh))))
        add.append(int(round(float(b_) * (1 << fixed_bits) * (1 << sh))))
        shift.append(int(sh))
    return mul, add, shift


def build(params, x_bits=8, h_bits=12, gate_bits=8, fixed_bits=12, multiplier_bits=15):
    """The integer predictor contract of a float checkpoint (JSON-serializable).

    The gate matrix has 384 rows: the input part of r, z, n (192 rows, input codes only) and the hidden part of r, z, n
    (192 rows, hidden codes only), each row with its own int8 scale (one MVAU with zero blocks on the hardware). Every
    row is requantized to one fixed point 2^-fixed_bits with its bias; r and z add their two parts, n keeps them apart
    for the r product. The sigmoid and tanh thresholds are then one table each, shared by every channel. The hidden
    state takes most of the precision (6.42.0 ablation: 8-bit hidden codes move most codebook indices)."""
    need(x_bits == 8 and 8 <= h_bits <= 16 and 6 <= gate_bits <= 10,
         "this contract version uses 8-bit input codes, 8..16-bit hidden codes and 6..10-bit gates")
    need(fixed_bits >= h_bits - 2, "the tanh fixed point must resolve the hidden code steps")
    c, hd = len(params["channels"]), params["hidden"]
    xh = 1 << (x_bits - 1)
    lo, hi = _count_range(params)
    cmid, cstep = (lo + hi) / 2, np.maximum(hi - lo, 1e-6) / (2 * xh - 2)
    amid_rows, amid, astep = _aux_thresholds(x_bits)
    mid = np.concatenate([cmid, amid])
    step = np.concatenate([cstep, astep])
    s_h = 1.0 / (1 << (h_bits - 1))
    w_ih, w_hh, b_ih, b_hh = params["w_ih"], params["w_hh"], params["b_ih"], params["b_hh"]
    nx, nh = c + AUXILIARY, hd
    real = np.zeros((6 * hd, nx + nh))
    real[:3 * hd, :nx] = w_ih * step                       # input part of r, z, n
    real[3 * hd:, nx:] = w_hh * s_h                        # hidden part of r, z, n
    bias = np.concatenate([w_ih @ mid + b_ih, b_hh])
    mq, ms = _quantize_rows(real)
    mul, add, shift = _requant(ms, bias, fixed_bits, multiplier_bits)
    fx = 1 << fixed_bits
    g = 1 << gate_bits
    sig_thr = []
    for k in range(1, g):
        pr = (k - 0.5) / g                                 # gate code >= k  <=>  sigmoid(pre) >= (k - 1/2) / 2^gate_bits
        sig_thr.append(int(math.ceil(math.log(pr / (1 - pr)) * fx - 1e-9)))
    hh = 1 << (h_bits - 1)
    tanh_thr = []
    for k in range(1, 2 * hh):
        y = (k - hh - 0.5) / hh                            # n code >= k - hh  <=>  tanh(pre) >= y
        tanh_thr.append(int(math.ceil(math.atanh(y) * fx - 1e-9)))
    oq, os_ = _quantize_rows(params["w_o"] * s_h)
    out_thr = []
    for ch, book in enumerate(params["codebooks"]):
        top = len(book) - 1
        row = []
        for k in range(1, len(book)):
            pr = (k - 0.5) / top
            row.append(int(math.ceil((math.log(pr / (1 - pr)) - params["b_o"][ch]) / os_[ch] - 1e-9)))
        out_thr.append(row)
    contract = dict(
        schema=SCHEMA, source=dict(checkpoint_sha256=params["checkpoint_sha256"], channels=params["channels"],
                                   scale=params["scale"], hidden=hd),
        bits=dict(x=x_bits, h=h_bits, gate=gate_bits, fixed=fixed_bits, multiplier=multiplier_bits),
        count_thresholds=_count_thresholds(params, cmid, cstep, x_bits), aux_thresholds=amid_rows,
        gates=dict(weights=mq.tolist(), multiplier=mul, offset=add, shift=shift, sigmoid_thresholds=sig_thr,
                   tanh_thresholds=tanh_thr),
        output=dict(weights=oq.tolist(), index_thresholds=out_thr),
        codebooks=[b.astype(np.int64).tolist() for b in params["codebooks"]])
    contract["sha256"] = contract_sha(contract)
    return contract


def contract_sha(contract):
    body = {k: v for k, v in contract.items() if k != "sha256"}
    return hashlib.sha256(json.dumps(body, sort_keys=True, separators=(",", ":")).encode("ascii")).hexdigest()


# ---------------------------------------------------------------- integer predictor

def delta_inputs(contract):
    """Number of delta inputs of a contract (``extend_delta``; 0 for the original form)."""
    return int(contract["source"].get("delta_inputs", 0))


class Integer:
    """Integer predictor of a contract: arrays prepared once, ``step`` per time point."""

    def __init__(self, contract):
        need(contract.get("schema") == SCHEMA and contract.get("sha256") == contract_sha(contract), "GRU predictor contract required")
        b = contract["bits"]
        self.c = len(contract["source"]["channels"])
        self.hd = contract["source"]["hidden"]
        self.gate_bits = b["gate"]
        self.xh, self.hh = 1 << (b["x"] - 1), 1 << (b["h"] - 1)
        self.count_thr = np.asarray(contract["count_thresholds"], np.int64)
        self.aux_thr = np.asarray(contract["aux_thresholds"], np.float32)
        g = contract["gates"]
        self.m = np.asarray(g["weights"], np.int64)
        self.mul = np.asarray(g["multiplier"], np.int64)
        self.add = np.asarray(g["offset"], np.int64)
        self.shift = np.asarray(g["shift"], np.int64)
        self.sig_thr = np.asarray(g["sigmoid_thresholds"], np.int64)
        self.tanh_thr = np.asarray(g["tanh_thresholds"], np.int64)
        self.o = np.asarray(contract["output"]["weights"], np.int64)
        self.out_thr = [np.asarray(r, np.int64) for r in contract["output"]["index_thresholds"]]
        self.books = [np.asarray(bk, np.int64) for bk in contract["codebooks"]]
        self.nd = delta_inputs(contract)
        self.prev = None                                  # the last step's x codes (delta inputs)
        self.reset()

    def reset(self):
        """The hidden state back to zero (a segment start); the delta inputs keep the previous step's codes."""
        self.h = np.zeros(self.hd, np.int64)

    @staticmethod
    def _rne_shift(v, s):
        """Round half to even of v / 2^s for int64 arrays (``s`` a scalar or an array of shifts >= 1)."""
        s = np.asarray(s, np.int64)
        q = v >> s
        r = v - (q << s)
        half = np.int64(1) << (s - 1)
        return q + ((r > half) | ((r == half) & ((q & 1) == 1))).astype(np.int64)

    def x_codes(self, q, aux):
        cq = (np.asarray(q, np.int64)[:, None] >= self.count_thr).sum(axis=1) - self.xh
        ca = (np.asarray(aux, np.float32)[:, None] >= self.aux_thr).sum(axis=1) - self.xh
        return np.concatenate([cq, ca])

    def inputs(self, q, aux):
        """The input codes of q[t], aux[t]: the x codes, then the delta inputs (without advancing)."""
        x = self.x_codes(q, aux)
        if not self.nd:
            return x
        return np.concatenate([x, x[:self.nd] - (x if self.prev is None else self.prev)[:self.nd]])

    def step(self, q, aux):
        """One step: codes of q[t], aux[t]; returns (index, prediction) for t + 1 and advances the hidden state."""
        hd = self.hd
        codes = self.inputs(q, aux)
        self.prev = codes[:len(self.count_thr) + AUXILIARY]
        acc = self.m @ np.concatenate([codes, self.h])
        a = self._rne_shift(acc * self.mul + self.add, self.shift)        # fixed point 2^-fixed, biases included
        rz = a[:2 * hd] + a[3 * hd:5 * hd]
        g_r = np.searchsorted(self.sig_thr, rz[:hd], side="right")
        g_z = np.searchsorted(self.sig_thr, rz[hd:], side="right")
        pre = a[2 * hd:3 * hd] + self._rne_shift(g_r * a[5 * hd:], self.gate_bits)
        n = np.searchsorted(self.tanh_thr, pre, side="right") - self.hh
        h = n + self._rne_shift(g_z * (self.h - n), self.gate_bits)
        self.h = np.clip(h, -self.hh, self.hh - 1)
        acc_o = self.o @ self.h
        index = np.array([int(np.searchsorted(t, acc_o[i], side="right")) for i, t in enumerate(self.out_thr)], np.int64)
        return index, np.array([b[i] for b, i in zip(self.books, index)], np.int64)


def integer_predictions(contract, q, aux, segment_steps=None):
    """Predictions of the integer predictor for q[1:] (int64 [T-1, C]) and their indices; with ``segment_steps`` the
    hidden state goes back to zero every that many steps (``segmented``)."""
    if segment_steps:
        _, idx = segmented(contract, q, aux, segment_steps)
        books = [np.asarray(b, np.int64) for b in contract["codebooks"]]
        return np.stack([books[i][idx[:, i]] for i in range(idx.shape[1])], axis=1), idx
    p = Integer(contract)
    q = np.asarray(q, np.int64)
    idx, pred = [], []
    for t in range(len(q) - 1):
        i, v = p.step(q[t], aux[t])
        idx.append(i)
        pred.append(v)
    return np.stack(pred), np.stack(idx)


def segmented(contract, q, aux, segment_steps):
    """The integer predictor in segments (inference optimization for batched hardware): the hidden state is zero at
    every multiple of ``segment_steps``, so all segments step together, one row each (exactly ``Integer.step`` within a
    segment). Returns (h after each step int64 [T-1, H], indices int64 [T-1, C])."""
    p = Integer(contract)
    q, aux = np.asarray(q, np.int64), np.asarray(aux, np.float32)
    n, L = len(q) - 1, int(segment_steps)
    need(n >= 1 and L >= 1, "steps and segment length")
    rows = -(-n // L)
    h = np.zeros((rows, p.hd), np.int64)
    h_out = np.zeros((n, p.hd), np.int64)
    idx = np.zeros((n, p.c), np.int64)
    hd = p.hd
    for k in range(L):
        t = np.arange(rows) * L + k
        ok = t < n
        tt = t[ok]
        codes = lambda u: np.concatenate([(q[u][:, :, None] >= p.count_thr[None]).sum(axis=2) - p.xh,
                                          (aux[u][:, :, None] >= p.aux_thr[None]).sum(axis=2) - p.xh], axis=1)
        x = codes(tt)
        if p.nd:                                          # x(t) - x(t-1), 0 at the day's first step
            x = np.concatenate([x, x[:, :p.nd] - codes(np.maximum(tt - 1, 0))[:, :p.nd]], axis=1)
        hh = h[ok]
        a = p._rne_shift(np.concatenate([x, hh], axis=1) @ p.m.T * p.mul + p.add, p.shift)
        rz = a[:, :2 * hd] + a[:, 3 * hd:5 * hd]
        g_r = np.searchsorted(p.sig_thr, rz[:, :hd], side="right")
        g_z = np.searchsorted(p.sig_thr, rz[:, hd:], side="right")
        pre = a[:, 2 * hd:3 * hd] + p._rne_shift(g_r * a[:, 5 * hd:], p.gate_bits)
        nn = np.searchsorted(p.tanh_thr, pre, side="right") - p.hh
        hn = np.clip(nn + p._rne_shift(g_z * (hh - nn), p.gate_bits), -p.hh, p.hh - 1)
        h[ok] = hn
        h_out[tt] = hn
        acc_o = hn @ p.o.T
        for i, th in enumerate(p.out_thr):
            idx[tt, i] = np.searchsorted(th, acc_o[:, i], side="right")
    return h_out, idx


def compare(params, contract, q, aux, torch_path=True):
    """Index agreement and residual magnitudes of the integer predictor against the float original."""
    fp, fi = float_predictions(params, q, aux, torch_path)
    ip, ii = integer_predictions(contract, q, aux)
    target = np.asarray(q, np.int64)[1:]
    d = np.abs(ii - fi)
    return dict(steps=int(len(target)), channels=int(target.shape[1]), index_equal=float((d == 0).mean()),
                index_within_1=float((d <= 1).mean()), index_max_abs=int(d.max()),
                residual_mean_abs_float=float(np.abs(target - fp).mean()),
                residual_mean_abs_integer=float(np.abs(target - ip).mean()))


# ---------------------------------------------------------------- coarse gate codes (inference optimization)

def _gptq_rows(w, inputs, grid_step, limit, damp):
    """Rows ``w`` (float [R, K]) on the grid ``grid_step`` * k, |value| <= ``limit``: columns rounded one at a time,
    largest input variance first, each rounding error spread over the remaining columns through the Cholesky factor
    of the inverse input covariance (GPTQ, Frantar et al. 2022)."""
    h = inputs.T @ inputs / max(len(inputs), 1)
    order = np.argsort(-np.diag(h))
    back = np.argsort(order)
    h = h[np.ix_(order, order)]
    dead = np.diag(h) == 0
    h[dead, dead] = 1.0
    h += damp * float(np.mean(np.diag(h))) * np.eye(len(h))
    u = np.linalg.cholesky(np.linalg.inv(h)).T
    work = np.array(w[:, order], np.float64)
    out = np.zeros_like(work)
    for j in range(work.shape[1]):
        out[:, j] = np.clip(np.round(work[:, j] / grid_step), -limit // grid_step, limit // grid_step) * grid_step
        err = (work[:, j] - out[:, j]) / u[j, j]
        work[:, j + 1:] -= np.outer(err, u[j, j + 1:])
    return out[:, back]


def coarse_gates(contract, q, aux, step=16, damp=0.01, iterations=2):
    """The gate codes of ``contract`` on a coarser grid at the same row scales: multiples of ``step`` with
    |code| < 2^(B_g - 1) (step 16 of 8-bit codes: 4-bit effective codes on the unchanged 8-bit datapath), rounded by
    GPTQ against the [x; h] codes the predictor sees on the calibration steps ``q``, ``aux`` (first the original
    model's trajectory, then ``iterations`` - 1 times the coarse model's own). The input rows keep their x columns and
    the hidden rows their h columns. Requant constants, thresholds, the output layer and every width stay, so the
    engine and the FINN tail keep their parameters; the result (hashed, ``precision.coarse``) is the new integer
    reference. An inference optimization: judge it by the compression of held-out days (``gru_codec``)."""
    need(contract.get("schema") == SCHEMA and contract.get("sha256") == contract_sha(contract), "GRU predictor contract required")
    bits = contract["bits"]["gate"]
    limit = ((1 << (bits - 1)) - 1) // step * step
    need(step >= 2 and step & (step - 1) == 0 and limit >= step, "step: a power of two below the gate code range")
    need(int(iterations) >= 1 and damp > 0 and len(q) >= 2, "iterations >= 1, damp > 0 and calibration steps")
    w = np.asarray(contract["gates"]["weights"], np.int64)
    hd = contract["source"]["hidden"]
    nx = w.shape[1] - hd
    rows_x, rows_h = np.arange(3 * hd), np.arange(3 * hd, 6 * hd)
    need(w.shape[0] == 6 * hd and not w[rows_x][:, nx:].any() and not w[rows_h][:, :nx].any(),
         "gate rows: input rows on the x codes, hidden rows on the h codes")

    def codes(c):
        p = Integer(c)
        out = []
        for t in range(len(q)):
            out.append(np.concatenate([p.x_codes(q[t], aux[t]), p.h]))
            p.step(q[t], aux[t])
        return np.asarray(out, np.float64)

    result = json.loads(json.dumps(contract))
    current = contract
    for _ in range(int(iterations)):
        v = codes(current)
        coarse = np.zeros_like(w)
        coarse[np.ix_(rows_x, np.arange(nx))] = _gptq_rows(w[np.ix_(rows_x, np.arange(nx))], v[:, :nx], step, limit, damp)
        coarse[np.ix_(rows_h, np.arange(nx, nx + hd))] = _gptq_rows(w[np.ix_(rows_h, np.arange(nx, nx + hd))], v[:, nx:],
                                                                    step, limit, damp)
        result["gates"]["weights"] = coarse.astype(np.int64).tolist()
        result.pop("sha256", None)
        result["sha256"] = contract_sha(result)
        current = result
    v = codes(contract)
    err = v @ (np.asarray(result["gates"]["weights"], np.float64) - w).T
    result.pop("sha256")
    result["precision"] = dict(coarse=dict(step=step, limit=limit, method="gptq", damp=damp, iterations=int(iterations),
                                           calibration_steps=len(q), source_sha256=contract["sha256"],
                                           changed=float(np.mean(np.asarray(result["gates"]["weights"]) != w)),
                                           accumulator_error_rms=float(np.sqrt(np.mean(err ** 2))),
                                           accumulator_rms=float(np.sqrt(np.mean((v @ w.T.astype(np.float64)) ** 2)))))
    result["sha256"] = contract_sha(result)
    return result


# ---------------------------------------------------------------- delta inputs (inference optimization)

DELTA_CONTAINER_BITS = 9                       # x(t) - x(t-1) of 8-bit codes


def _input_scales(params, x_bits=8):
    """(mid, step) of the x codes and the x-part row scales of ``build`` (the contract's x-part grid)."""
    c, hd = len(params["channels"]), params["hidden"]
    xh = 1 << (x_bits - 1)
    lo, hi = _count_range(params)
    _, amid, astep = _aux_thresholds(x_bits)
    mid = np.concatenate([(lo + hi) / 2, amid])
    step = np.concatenate([np.maximum(hi - lo, 1e-6) / (2 * xh - 2), astep])
    _, scale = _quantize_rows(params["w_ih"] * step)
    return mid, step, scale


def train_delta(params, contract, days, delta=30, segment_steps=7, epochs=4, lr=3e-3, batch=8192, seed=1,
                device=None, log=None, prune_density=None, prune_epochs=2):
    """Float weights (3H x ``delta``) of ``delta`` inputs x(t) - x(t-1) (the first ``delta`` x codes, in input units)
    next to the frozen float model of ``params``, trained on the observation ``days`` (q, aux pairs) in segments of
    ``segment_steps`` from the zero state (the batched hardware path): binary cross-entropy of the sigmoid output
    against the codebook position of the next count (the nearest codebook entry), Adam, the delta weights only (from
    zero, so training starts at the original model). The inputs are the contract's quantized x codes (x = mid + code *
    step), as the integer predictor sees them. ``prune_density`` (0..1): after the epochs the delta weights with the largest
    integer codes (``extend_delta``'s row scales and grid) are kept up to that share of the matrix, the rest are zero,
    and ``prune_epochs`` more epochs train the kept ones (the engine's power follows the share of nonzero products:
    a sparser delta part costs less for the same operation count)."""
    import torch
    need(contract.get("schema") == SCHEMA and delta_inputs(contract) == 0, "an original-form contract required")
    need(prune_density is None or (0.0 < float(prune_density) < 1.0 and int(prune_epochs) >= 0), "prune density 0..1")
    c, hd = len(params["channels"]), params["hidden"]
    nx = c + AUXILIARY
    need(1 <= delta <= nx and segment_steps >= 1 and epochs >= 1, "delta 1..x codes, segment and epochs")
    mid, step, _ = _input_scales(params, contract["bits"]["x"])
    p = Integer(contract)
    L = int(segment_steps)
    xs, ds, ts = [], [], []
    for q, aux in days:
        q, aux = np.asarray(q, np.int64), np.asarray(aux, np.float32)
        x = np.concatenate([np.stack([np.searchsorted(p.count_thr[ch], q[:, ch], side="right") for ch in range(c)], 1),
                            np.stack([np.searchsorted(p.aux_thr[k], aux[:, k], side="right") for k in range(AUXILIARY)], 1)],
                           axis=1) - p.xh
        d = np.zeros((len(x), delta), np.int64)
        d[1:] = x[1:, :delta] - x[:-1, :delta]
        tgt = np.zeros((len(q), c), np.float32)
        for ch, book in enumerate(params["codebooks"]):
            b = np.asarray(book, np.int64)
            i = np.clip(np.searchsorted(b, q[:, ch]), 1, len(b) - 1)
            i = np.where(np.abs(b[i - 1] - q[:, ch]) <= np.abs(b[i] - q[:, ch]), i - 1, i)
            tgt[:, ch] = i / (len(b) - 1)
        n = (len(x) - 1) // L * L
        xs.append((mid + x[:n] * step).astype(np.float32).reshape(-1, L, nx))
        ds.append((d[:n] * step[:delta]).astype(np.float32).reshape(-1, L, delta))
        ts.append(tgt[1:n + 1].reshape(-1, L, c))
    xs, ds, ts = np.concatenate(xs), np.concatenate(ds), np.concatenate(ts)
    dev = device or ("cuda" if torch.cuda.is_available() else "cpu")
    T = lambda a: torch.as_tensor(np.asarray(a, np.float32), device=dev)
    w_ih, w_hh, b_ih, b_hh = T(params["w_ih"]), T(params["w_hh"]), T(params["b_ih"]), T(params["b_hh"])
    w_o, b_o = T(params["w_o"]), T(params["b_o"])
    w_d = torch.zeros((3 * hd, delta), device=dev, requires_grad=True)

    def forward(xb, db):
        h = torch.zeros((xb.shape[0], hd), device=dev)
        out = []
        for k in range(L):
            gi = xb[:, k] @ w_ih.T + b_ih + db[:, k] @ w_d.T
            gh = h @ w_hh.T + b_hh
            r = torch.sigmoid(gi[:, :hd] + gh[:, :hd])
            z = torch.sigmoid(gi[:, hd:2 * hd] + gh[:, hd:2 * hd])
            h = (1 - z) * torch.tanh(gi[:, 2 * hd:] + r * gh[:, 2 * hd:]) + z * h
            out.append(torch.sigmoid(h @ w_o.T + b_o))
        return torch.stack(out, 1)

    torch.manual_seed(seed)
    opt = torch.optim.Adam([w_d], lr=lr)
    rng = np.random.default_rng(seed)
    history = []
    mask = None

    def run_epoch(epoch):
        order = rng.permutation(len(xs))
        total, count = 0.0, 0
        for s in range(0, len(order), batch):
            i = order[s:s + batch]
            loss = torch.nn.functional.binary_cross_entropy(forward(T(xs[i]), T(ds[i])), T(ts[i]))
            opt.zero_grad()
            loss.backward()
            opt.step()
            if mask is not None:
                with torch.no_grad():
                    w_d.mul_(mask)
            total += float(loss.detach()) * len(i)
            count += len(i)
        history.append(total / count)
        if log:
            log("delta epoch %d: training BCE %.6f%s" % (epoch, history[-1], " (pruned)" if mask is not None else ""))
    for epoch in range(int(epochs)):
        run_epoch(epoch)
    pruned = None
    if prune_density is not None:
        # the codes extend_delta would write: |w| x step / row scale; the largest kept up to the density
        _, step_x, scale = _input_scales(params, contract["bits"]["x"])
        size = np.abs(w_d.detach().cpu().numpy() * step_x[:delta] / scale[:, None])
        keep = int(round(float(prune_density) * size.size))
        order_ = np.argsort(size, axis=None)[::-1]
        m = np.zeros(size.size, np.float32)
        m[order_[:keep]] = 1.0
        mask = torch.as_tensor(m.reshape(size.shape), device=dev)
        with torch.no_grad():
            w_d.mul_(mask)
        for epoch in range(int(prune_epochs)):
            run_epoch(int(epochs) + epoch)
        pruned = dict(density=float(prune_density), kept=keep, epochs=int(prune_epochs))
    return w_d.detach().cpu().numpy().astype(np.float64), dict(segments=int(len(xs)), epochs=int(epochs), lr=lr,
                                                               batch=batch, seed=seed, training_bce=history,
                                                               prune=pruned)


def extend_delta(contract, params, weights, grid_step=16, training=None):
    """The contract with delta inputs: the float delta weights (3H x D, ``train_delta``) on the x-part row scales of
    ``build`` and the contract's grid (multiples of ``grid_step`` within the gate code range, the coarse 4-bit codes),
    as D more columns of the input rows after the x codes. Everything else stays (the FINN tail's parameters); the x
    container of the declared accumulator widths becomes ``DELTA_CONTAINER_BITS`` (x(t) - x(t-1) of 8-bit codes)."""
    need(contract.get("schema") == SCHEMA and contract.get("sha256") == contract_sha(contract) and not delta_inputs(contract),
         "an original-form GRU predictor contract required")
    hd = contract["source"]["hidden"]
    m = np.asarray(contract["gates"]["weights"], np.int64)
    nx = m.shape[1] - hd
    w = np.asarray(weights, np.float64)
    need(w.ndim == 2 and w.shape[0] == 3 * hd and 1 <= w.shape[1] <= nx and nx + w.shape[1] <= 64,
         "delta weights 3H x D, D at most the x codes and x codes + D at most 64 (the G4 x group)")
    nd = w.shape[1]
    _, step, scale = _input_scales(params, contract["bits"]["x"])
    limit = ((1 << (contract["bits"]["gate"] - 1)) - 1) // grid_step * grid_step
    codes = np.clip(np.rint(w * step[:nd] / scale[:, None] / grid_step) * grid_step, -limit, limit).astype(np.int64)
    out = json.loads(json.dumps(contract))
    m2 = np.zeros((6 * hd, nx + nd + hd), np.int64)
    m2[:, :nx] = m[:, :nx]
    m2[:3 * hd, nx:nx + nd] = codes
    m2[:, nx + nd:] = m[:, nx:]
    out["gates"]["weights"] = m2.tolist()
    out["source"]["delta_inputs"] = nd
    out["bits"]["x_container"] = DELTA_CONTAINER_BITS
    out.pop("sha256", None)
    out["precision"] = dict(out.get("precision") or {}, delta=dict(
        inputs=nd, of="x(t) - x(t-1) of the first %d x codes, 0 at the day's first step" % nd, grid_step=grid_step,
        limit=limit, nonzero=float(np.mean(codes != 0)), source_sha256=contract["sha256"], **(training or {})))
    out["sha256"] = contract_sha(out)
    return out
