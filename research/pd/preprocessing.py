"""PD normalized-count, validity and causal temporal feature construction."""
from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime
import re
import numpy as np

_SAMPLE_DATE_PATTERN = re.compile(r"_(\d{8})_count_rate_teacher$")


@dataclass(frozen=True)
class V9CountStats:
    mean: tuple[float, ...]
    std: tuple[float, ...]

    def arrays(self) -> tuple[np.ndarray, np.ndarray]:
        mean = np.asarray(self.mean, dtype=np.float32).reshape(1, 1, -1)
        std = np.asarray(self.std, dtype=np.float32).reshape(1, 1, -1)
        return mean, np.clip(std, 1.0e-6, None)


def make_v9_features(count_norm: np.ndarray, mask: np.ndarray) -> np.ndarray:
    """Build count-only features without imputing missing observations.

    Invalid count entries are zeroed after normalization and accompanied by an
    explicit validity mask. The delta branch is also masked so missing gaps do
    not become artificial jumps.
    """

    count_norm = np.asarray(count_norm, dtype=np.float32)
    mask = (np.asarray(mask, dtype=np.float32) > 0.0).astype(np.float32)
    clean = np.where(mask > 0.0, count_norm, 0.0).astype(np.float32)
    delta = np.zeros_like(clean, dtype=np.float32)
    valid_pairs = mask[:, 1:, :] * mask[:, :-1, :]
    delta[:, 1:, :] = (clean[:, 1:, :] - clean[:, :-1, :]) * valid_pairs
    return np.concatenate([clean, delta, mask], axis=-1).astype(np.float32)


def make_v9_multistream_onset_features(
    count_norm: np.ndarray,
    mask: np.ndarray,
    *,
    orbit_lag_steps: int = 20,
    baseline_steps: int = 15,
    baseline_gap_steps: int = 2,
    minimum_baseline_points: int = 5,
    cusum_shift: float = 0.5,
    cusum_decay: float = 0.98,
    cusum_clip: float = 20.0,
) -> np.ndarray:
    """Build causal per-stream onset features for multi-channel, multi-FOV TCNs.

    The output is flattened in stream-major order: every count stream owns the
    six adjacent features ``count, delta, valid, orbit residual, rolling
    residual, CUSUM state``. Invalid observations remain zero and are never
    treated as measured counts.
    """

    count_norm = np.asarray(count_norm, dtype=np.float32)
    mask = (np.asarray(mask, dtype=np.float32) > 0.0).astype(np.float32)
    if count_norm.ndim != 3 or mask.shape != count_norm.shape:
        raise ValueError("count_norm and mask must have matching shape (N,T,C)")
    if int(orbit_lag_steps) < 1:
        raise ValueError("orbit_lag_steps must be positive")
    if int(baseline_steps) < 1 or int(baseline_gap_steps) < 0:
        raise ValueError("baseline_steps must be positive and baseline_gap_steps non-negative")

    clean = np.where(mask > 0.0, count_norm, 0.0).astype(np.float32)
    delta = np.zeros_like(clean)
    valid_pairs = mask[:, 1:, :] * mask[:, :-1, :]
    delta[:, 1:, :] = (clean[:, 1:, :] - clean[:, :-1, :]) * valid_pairs

    orbit_residual = np.zeros_like(clean)
    lag = int(orbit_lag_steps)
    orbit_valid = mask[:, lag:, :] * mask[:, :-lag, :]
    orbit_residual[:, lag:, :] = (clean[:, lag:, :] - clean[:, :-lag, :]) * orbit_valid

    rolling_residual = np.zeros_like(clean)
    baseline = int(baseline_steps)
    gap = int(baseline_gap_steps)
    minimum = max(1, int(minimum_baseline_points))
    weighted = clean * mask
    weighted_prefix = np.concatenate(
        [np.zeros_like(weighted[:, :1, :]), np.cumsum(weighted, axis=1, dtype=np.float32)],
        axis=1,
    )
    mask_prefix = np.concatenate(
        [np.zeros_like(mask[:, :1, :]), np.cumsum(mask, axis=1, dtype=np.float32)],
        axis=1,
    )
    for time_index in range(clean.shape[1]):
        stop = time_index - gap
        start = stop - baseline
        if start < 0 or stop <= start:
            continue
        baseline_sum = weighted_prefix[:, stop, :] - weighted_prefix[:, start, :]
        baseline_count = mask_prefix[:, stop, :] - mask_prefix[:, start, :]
        valid = (baseline_count >= float(minimum)) & (mask[:, time_index, :] > 0.0)
        baseline_mean = baseline_sum / np.clip(baseline_count, 1.0, None)
        rolling_residual[:, time_index, :] = np.where(
            valid,
            clean[:, time_index, :] - baseline_mean,
            0.0,
        )

    rolling_residual = np.clip(rolling_residual, -8.0, 8.0).astype(np.float32)
    cusum_state = np.zeros_like(clean)
    state = np.zeros((clean.shape[0], clean.shape[2]), dtype=np.float32)
    decay = float(cusum_decay)
    for time_index in range(clean.shape[1]):
        state = np.maximum(
            0.0,
            decay * state + rolling_residual[:, time_index, :] - float(cusum_shift),
        )
        state *= mask[:, time_index, :]
        cusum_state[:, time_index, :] = np.minimum(state, float(cusum_clip)) / max(
            float(cusum_clip),
            1.0e-6,
        )

    stream_features = np.stack(
        [clean, delta, mask, orbit_residual, rolling_residual, cusum_state],
        axis=-1,
    )
    return stream_features.reshape(
        stream_features.shape[0],
        stream_features.shape[1],
        stream_features.shape[2] * stream_features.shape[3],
    ).astype(np.float32)


def make_v9_multistream_weak_onset_features(
    count_norm: np.ndarray,
    mask: np.ndarray,
    *,
    weak_baseline_steps: int = 36,
    weak_baseline_gap_steps: int = 2,
    weak_minimum_baseline_points: int = 12,
    weak_std_floor: float = 0.25,
    weak_cusum_shift: float = 0.15,
    weak_cusum_decay: float = 0.995,
    weak_cusum_clip: float = 40.0,
) -> np.ndarray:
    """Add low-SNR causal evidence to the standard multi-stream features.

    The additional rolling z-score uses only observations before the current
    point. A slow-decay CUSUM then accumulates small, coherent rises that do not
    cross the standard 0.5-sigma CUSUM shift used by the baseline layout.
    """

    count_norm = np.asarray(count_norm, dtype=np.float32)
    mask = (np.asarray(mask, dtype=np.float32) > 0.0).astype(np.float32)
    if count_norm.ndim != 3 or mask.shape != count_norm.shape:
        raise ValueError("count_norm and mask must have matching shape (N,T,C)")
    baseline = int(weak_baseline_steps)
    gap = int(weak_baseline_gap_steps)
    minimum = max(1, int(weak_minimum_baseline_points))
    if baseline < 1 or gap < 0:
        raise ValueError("weak_baseline_steps must be positive and weak_baseline_gap_steps non-negative")

    base = make_v9_multistream_onset_features(count_norm, mask).reshape(
        count_norm.shape[0],
        count_norm.shape[1],
        count_norm.shape[2],
        6,
    )
    clean = np.where(mask > 0.0, count_norm, 0.0).astype(np.float32)
    weighted = clean * mask
    squared = np.square(clean) * mask
    weighted_prefix = np.concatenate(
        [np.zeros_like(weighted[:, :1, :]), np.cumsum(weighted, axis=1, dtype=np.float32)],
        axis=1,
    )
    squared_prefix = np.concatenate(
        [np.zeros_like(squared[:, :1, :]), np.cumsum(squared, axis=1, dtype=np.float32)],
        axis=1,
    )
    mask_prefix = np.concatenate(
        [np.zeros_like(mask[:, :1, :]), np.cumsum(mask, axis=1, dtype=np.float32)],
        axis=1,
    )
    rolling_z = np.zeros_like(clean)
    for time_index in range(clean.shape[1]):
        stop = time_index - gap
        start = stop - baseline
        if start < 0 or stop <= start:
            continue
        count = mask_prefix[:, stop, :] - mask_prefix[:, start, :]
        valid = (count >= float(minimum)) & (mask[:, time_index, :] > 0.0)
        mean = (weighted_prefix[:, stop, :] - weighted_prefix[:, start, :]) / np.clip(
            count,
            1.0,
            None,
        )
        second_moment = (
            squared_prefix[:, stop, :] - squared_prefix[:, start, :]
        ) / np.clip(count, 1.0, None)
        variance = np.maximum(second_moment - np.square(mean), 0.0)
        scale = np.maximum(np.sqrt(variance), float(weak_std_floor))
        rolling_z[:, time_index, :] = np.where(
            valid,
            (clean[:, time_index, :] - mean) / scale,
            0.0,
        )
    rolling_z = np.clip(rolling_z, -8.0, 8.0).astype(np.float32)

    weak_cusum = np.zeros_like(clean)
    state = np.zeros((clean.shape[0], clean.shape[2]), dtype=np.float32)
    for time_index in range(clean.shape[1]):
        state = np.maximum(
            0.0,
            float(weak_cusum_decay) * state
            + rolling_z[:, time_index, :]
            - float(weak_cusum_shift),
        )
        state *= mask[:, time_index, :]
        weak_cusum[:, time_index, :] = np.minimum(state, float(weak_cusum_clip)) / max(
            float(weak_cusum_clip),
            1.0e-6,
        )

    return np.concatenate(
        [base, rolling_z[..., None], weak_cusum[..., None]],
        axis=-1,
    ).reshape(
        count_norm.shape[0],
        count_norm.shape[1],
        count_norm.shape[2] * 8,
    ).astype(np.float32)


def make_v9_contiguous_multistream_weak_onset_features(
    count_norm: np.ndarray,
    mask: np.ndarray,
    sample_ids: np.ndarray,
    source_ids: np.ndarray,
) -> np.ndarray:
    """Carry causal rolling statistics across adjacent daily windows.

    The model still receives one 24-hour window. Only preprocessing state is
    computed over consecutive days from the same spacecraft, matching online
    operation where rolling background and CUSUM state do not reset at UTC
    midnight. Date gaps and spacecraft boundaries reset the state.
    """

    count_norm = np.asarray(count_norm, dtype=np.float32)
    mask = np.asarray(mask, dtype=np.float32)
    sample_ids = np.asarray(sample_ids, dtype=str).reshape(-1)
    source_ids = np.asarray(source_ids, dtype=str).reshape(-1)
    if count_norm.ndim != 3 or mask.shape != count_norm.shape:
        raise ValueError("count_norm and mask must have matching shape (N,T,C)")
    if sample_ids.size != count_norm.shape[0] or source_ids.size != sample_ids.size:
        raise ValueError("sample_ids and source_ids must match count rows")

    row_by_source_date: dict[tuple[str, np.datetime64], int] = {}
    row_dates: list[np.datetime64 | None] = []
    for index, (sample_id, source_id) in enumerate(
        zip(sample_ids.tolist(), source_ids.tolist())
    ):
        match = _SAMPLE_DATE_PATTERN.search(sample_id)
        if match is None:
            row_dates.append(None)
            continue
        date = np.datetime64(
            datetime.strptime(match.group(1), "%Y%m%d").date(),
            "D",
        )
        row_dates.append(date)
        row_by_source_date[(source_id, date)] = index

    previous_count = np.zeros_like(count_norm)
    previous_mask = np.zeros_like(mask)
    for index, (source_id, date) in enumerate(
        zip(source_ids.tolist(), row_dates)
    ):
        if date is None:
            continue
        previous_index = row_by_source_date.get(
            (source_id, date - np.timedelta64(1, "D"))
        )
        if previous_index is None:
            continue
        previous_count[index] = count_norm[previous_index]
        previous_mask[index] = mask[previous_index]

    warm_count = np.concatenate([previous_count, count_norm], axis=1)
    warm_mask = np.concatenate([previous_mask, mask], axis=1)
    warm_features = make_v9_multistream_weak_onset_features(
        warm_count,
        warm_mask,
    )
    return warm_features[:, count_norm.shape[1] :, :]


def make_v9_paper_aligned_onset_features(
    count_norm: np.ndarray,
    count_log10: np.ndarray,
    mask: np.ndarray,
    sample_ids: np.ndarray,
    source_ids: np.ndarray,
    *,
    channels_per_fov: int = 3,
    ratio_baseline_steps: int = 15,
    ratio_gap_steps: int = 2,
    aggregate_baseline_steps: int = 72,
    aggregate_minimum_points: int = 36,
) -> np.ndarray:
    """Add causal telescope-sum statistics used by published onset methods.

    P4, P5, and P6 are converted back to linear count-rate and summed within
    each telescope. The added features are local log-ratio, rolling z-score,
    positive CUSUM state, and aggregate validity. They are repeated beside each
    channel in the same FOV so the existing stream/FOV hierarchy remains
    unchanged.
    """

    count_norm = np.asarray(count_norm, dtype=np.float32)
    count_log10 = np.asarray(count_log10, dtype=np.float32)
    mask = (np.asarray(mask, dtype=np.float32) > 0.0).astype(np.float32)
    if count_norm.shape != count_log10.shape or mask.shape != count_norm.shape:
        raise ValueError("count_norm, count_log10, and mask must have matching shapes")
    if count_norm.shape[2] % int(channels_per_fov) != 0:
        raise ValueError("channel count must be divisible by channels_per_fov")

    sample_ids = np.asarray(sample_ids, dtype=str).reshape(-1)
    source_ids = np.asarray(source_ids, dtype=str).reshape(-1)
    row_by_source_date: dict[tuple[str, np.datetime64], int] = {}
    row_dates: list[np.datetime64 | None] = []
    for index, (sample_id, source_id) in enumerate(
        zip(sample_ids.tolist(), source_ids.tolist())
    ):
        match = _SAMPLE_DATE_PATTERN.search(sample_id)
        if match is None:
            row_dates.append(None)
            continue
        date = np.datetime64(
            datetime.strptime(match.group(1), "%Y%m%d").date(),
            "D",
        )
        row_dates.append(date)
        row_by_source_date[(source_id, date)] = index

    previous_count = np.zeros_like(count_norm)
    previous_log10 = np.zeros_like(count_log10)
    previous_mask = np.zeros_like(mask)
    for index, (source_id, date) in enumerate(
        zip(source_ids.tolist(), row_dates)
    ):
        if date is None:
            continue
        previous_index = row_by_source_date.get(
            (source_id, date - np.timedelta64(1, "D"))
        )
        if previous_index is None:
            continue
        previous_count[index] = count_norm[previous_index]
        previous_log10[index] = np.nan_to_num(
            count_log10[previous_index],
            nan=0.0,
        )
        previous_mask[index] = mask[previous_index]

    warm_count = np.concatenate([previous_count, count_norm], axis=1)
    warm_log10 = np.concatenate(
        [previous_log10, np.nan_to_num(count_log10, nan=0.0)],
        axis=1,
    )
    warm_mask = np.concatenate([previous_mask, mask], axis=1)
    base = make_v9_multistream_weak_onset_features(
        warm_count,
        warm_mask,
    ).reshape(
        warm_count.shape[0],
        warm_count.shape[1],
        warm_count.shape[2],
        8,
    )

    linear_count = np.power(
        10.0,
        np.clip(warm_log10, -8.0, 8.0),
        dtype=np.float32,
    )
    fov_count = warm_count.shape[2] // int(channels_per_fov)
    fov_linear = linear_count.reshape(
        warm_count.shape[0],
        warm_count.shape[1],
        fov_count,
        int(channels_per_fov),
    )
    fov_mask = warm_mask.reshape(
        warm_mask.shape[0],
        warm_mask.shape[1],
        fov_count,
        int(channels_per_fov),
    )
    aggregate_valid = np.all(fov_mask > 0.0, axis=-1)
    aggregate = np.where(
        aggregate_valid,
        fov_linear.sum(axis=-1),
        0.0,
    ).astype(np.float32)
    aggregate_log = np.where(
        aggregate_valid,
        np.log10(np.clip(aggregate, 1.0e-8, None)),
        0.0,
    ).astype(np.float32)

    weighted = aggregate_log * aggregate_valid
    squared = np.square(aggregate_log) * aggregate_valid
    value_prefix = np.concatenate(
        [
            np.zeros_like(weighted[:, :1]),
            np.cumsum(weighted, axis=1, dtype=np.float32),
        ],
        axis=1,
    )
    square_prefix = np.concatenate(
        [
            np.zeros_like(squared[:, :1]),
            np.cumsum(squared, axis=1, dtype=np.float32),
        ],
        axis=1,
    )
    valid_prefix = np.concatenate(
        [
            np.zeros_like(aggregate_valid[:, :1], dtype=np.float32),
            np.cumsum(aggregate_valid, axis=1, dtype=np.float32),
        ],
        axis=1,
    )
    log_ratio = np.zeros_like(aggregate_log)
    aggregate_z = np.zeros_like(aggregate_log)
    for time_index in range(warm_count.shape[1]):
        ratio_stop = time_index - int(ratio_gap_steps)
        ratio_start = ratio_stop - int(ratio_baseline_steps)
        if ratio_start >= 0 and ratio_stop > ratio_start:
            baseline_count = (
                valid_prefix[:, ratio_stop] - valid_prefix[:, ratio_start]
            )
            baseline_mean = (
                value_prefix[:, ratio_stop] - value_prefix[:, ratio_start]
            ) / np.clip(baseline_count, 1.0, None)
            ratio_valid = (
                aggregate_valid[:, time_index]
                & (baseline_count >= max(3, int(ratio_baseline_steps) // 2))
            )
            log_ratio[:, time_index] = np.where(
                ratio_valid,
                aggregate_log[:, time_index] - baseline_mean,
                0.0,
            )

        z_stop = time_index - int(ratio_gap_steps)
        z_start = z_stop - int(aggregate_baseline_steps)
        if z_start < 0 or z_stop <= z_start:
            continue
        count = valid_prefix[:, z_stop] - valid_prefix[:, z_start]
        mean = (
            value_prefix[:, z_stop] - value_prefix[:, z_start]
        ) / np.clip(count, 1.0, None)
        second = (
            square_prefix[:, z_stop] - square_prefix[:, z_start]
        ) / np.clip(count, 1.0, None)
        scale = np.maximum(np.sqrt(np.maximum(second - np.square(mean), 0.0)), 0.05)
        z_valid = (
            aggregate_valid[:, time_index]
            & (count >= int(aggregate_minimum_points))
        )
        aggregate_z[:, time_index] = np.where(
            z_valid,
            (aggregate_log[:, time_index] - mean) / scale,
            0.0,
        )
    log_ratio = np.clip(log_ratio, -4.0, 4.0).astype(np.float32)
    aggregate_z = np.clip(aggregate_z, -8.0, 8.0).astype(np.float32)
    aggregate_cusum = np.zeros_like(aggregate_z)
    state = np.zeros(
        (aggregate_z.shape[0], aggregate_z.shape[2]),
        dtype=np.float32,
    )
    for time_index in range(aggregate_z.shape[1]):
        state = np.maximum(
            0.0,
            0.995 * state + aggregate_z[:, time_index] - 0.15,
        )
        state *= aggregate_valid[:, time_index]
        aggregate_cusum[:, time_index] = np.minimum(state, 40.0) / 40.0

    fov_features = np.stack(
        [
            log_ratio,
            aggregate_z,
            aggregate_cusum,
            aggregate_valid.astype(np.float32),
        ],
        axis=-1,
    )
    repeated = np.repeat(
        fov_features[:, :, :, None, :],
        int(channels_per_fov),
        axis=3,
    ).reshape(
        warm_count.shape[0],
        warm_count.shape[1],
        warm_count.shape[2],
        4,
    )
    combined = np.concatenate([base, repeated], axis=-1).reshape(
        warm_count.shape[0],
        warm_count.shape[1],
        warm_count.shape[2] * 12,
    )
    return combined[:, count_norm.shape[1] :, :].astype(np.float32)
