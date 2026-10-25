from __future__ import annotations

import numpy as np


FEATURE_DIMS = {
    "raw": 1,
    "raw_diff": 2,
    "raw_diff_ewma": 3,
    "nominal_raw": 1,
    "nominal_raw_diff": 2,
    "nominal_raw_diff_ewma": 3,
    "causal_raw": 1,
    "causal_raw_diff": 2,
    "causal_raw_diff_ewma": 3,
    "hybrid_raw_diff_ewma": 6,
    "hybrid_raw_diff_ewma_multiscale": 9,
    "nominal_raw_diff_ewma_multiscale": 6,
    "dual_1h_4h_raw_diff_ewma": 6,
    "hybrid_dual_1h_4h_raw_diff_ewma": 9,
    "hybrid_dual_1h_pre4h_raw_diff_ewma": 9,
}


def feature_dim(mode: str) -> int:
    try:
        return FEATURE_DIMS[mode]
    except KeyError as error:
        raise ValueError(f"unsupported feature mode: {mode}") from error


def anonymous_window_features(
    window: np.ndarray,
    mode: str = "raw_diff_ewma",
    ewma_alpha: float = 0.1,
) -> np.ndarray:
    """Return `(channels, features, time)` without channel-specific statistics.

    A complete input window contains only observations at or before the current
    decision time. Median/MAD normalization therefore remains deployable without
    mission-specific calibration data and does not encode channel identity.
    """

    values = np.asarray(window, dtype=np.float32)
    if values.ndim != 2:
        raise ValueError("window must be shaped (time, channels)")
    if not np.all(np.isfinite(values)):
        raise ValueError("window contains non-finite values")
    if not 0.0 < ewma_alpha <= 1.0:
        raise ValueError("ewma_alpha must be in (0, 1]")
    feature_dim(mode)

    center = np.median(values, axis=0)
    mad = 1.4826 * np.median(np.abs(values - center), axis=0)
    std = values.std(axis=0, dtype=np.float64).astype(np.float32)
    scale = np.where(mad > 1e-6, mad, np.where(std > 1e-6, std, 1.0))
    normalized = np.clip((values - center) / scale, -20.0, 20.0).astype(np.float32)
    streams = [normalized]
    if mode in {"raw_diff", "raw_diff_ewma"}:
        delta = np.empty_like(normalized)
        delta[0] = 0.0
        delta[1:] = normalized[1:] - normalized[:-1]
        streams.append(delta)
    if mode == "raw_diff_ewma":
        smooth = np.empty_like(normalized)
        smooth[0] = normalized[0]
        for index in range(1, normalized.shape[0]):
            smooth[index] = ewma_alpha * normalized[index] + (1.0 - ewma_alpha) * smooth[index - 1]
        streams.append(normalized - smooth)
    return np.stack(streams, axis=1).transpose(2, 1, 0).astype(np.float32, copy=False)


def nominal_window_features(
    normalized_windows: np.ndarray,
    mode: str = "nominal_raw_diff_ewma",
    ewma_alpha: float = 0.1,
) -> np.ndarray:
    """Expand nominal-calibrated windows without re-centering each window.

    Input is ``(windows, time)``. The channel-specific nominal calibration is
    deliberately external to the model checkpoint; the shared model receives
    only dimensionless values and therefore has no channel-ID parameter.
    """

    values = np.asarray(normalized_windows, dtype=np.float32)
    if values.ndim != 2:
        raise ValueError("normalized_windows must be shaped (windows, time)")
    if not np.all(np.isfinite(values)):
        raise ValueError("normalized_windows contains non-finite values")
    if mode.startswith("causal_"):
        mode = "nominal_" + mode.removeprefix("causal_")
    if mode not in {"nominal_raw", "nominal_raw_diff", "nominal_raw_diff_ewma"}:
        raise ValueError(f"unsupported nominal feature mode: {mode}")
    if not 0.0 < ewma_alpha <= 1.0:
        raise ValueError("ewma_alpha must be in (0, 1]")

    streams = [values]
    if mode in {"nominal_raw_diff", "nominal_raw_diff_ewma"}:
        delta = np.empty_like(values)
        delta[:, 0] = 0.0
        delta[:, 1:] = values[:, 1:] - values[:, :-1]
        streams.append(delta)
    if mode == "nominal_raw_diff_ewma":
        smooth = np.empty_like(values)
        smooth[:, 0] = values[:, 0]
        for index in range(1, values.shape[1]):
            smooth[:, index] = (
                ewma_alpha * values[:, index]
                + (1.0 - ewma_alpha) * smooth[:, index - 1]
            )
        streams.append(values - smooth)
    return np.stack(streams, axis=1).astype(np.float32, copy=False)
