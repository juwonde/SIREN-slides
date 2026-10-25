from __future__ import annotations

import numpy as np


DEFAULT_HOLD_WINDOWS = 1


def apply_alarm_policy(
    probabilities: np.ndarray,
    valid: np.ndarray,
    *,
    threshold: float | np.ndarray,
    minimum_consecutive: int,
    hold_windows: int = DEFAULT_HOLD_WINDOWS,
    max_active_windows: int | None = None,
) -> np.ndarray:
    """Convert anomaly scores into a causal, stateful alarm timeline.

    An alarm opens only after ``minimum_consecutive`` valid scores reach the
    threshold. Once open, every new score above the threshold extends the
    alarm, and the alarm remains active for ``hold_windows`` subsequent score
    intervals. Invalid intervals force the alarm state to reset.
    """
    scores = np.asarray(probabilities, dtype=np.float64)
    valid_mask = np.asarray(valid, dtype=bool)
    if scores.ndim != 1 or valid_mask.ndim != 1 or scores.shape != valid_mask.shape:
        raise ValueError("probabilities and valid must be same-length 1D arrays")
    thresholds = np.asarray(threshold, dtype=np.float64)
    if thresholds.ndim == 0:
        thresholds = np.full(scores.shape, float(thresholds), dtype=np.float64)
    if thresholds.shape != scores.shape:
        raise ValueError("threshold must be scalar or match the score timeline")
    if not np.all(np.isfinite(thresholds)):
        raise ValueError("threshold must be finite")
    if minimum_consecutive <= 0:
        raise ValueError("minimum_consecutive must be positive")
    if hold_windows < 0:
        raise ValueError("hold_windows must be non-negative")
    if max_active_windows is not None and max_active_windows <= 0:
        raise ValueError("max_active_windows must be positive when provided")

    above = (scores >= thresholds) & valid_mask
    confirmed = np.zeros_like(above)
    for left, right in true_runs(above):
        confirmation = left + minimum_consecutive - 1
        if confirmation < right:
            confirmed[confirmation:right] = True

    if hold_windows == 0 or not np.any(confirmed):
        alarms = confirmed
    else:
        indices = np.arange(len(confirmed), dtype=np.int64)
        last_confirmation = np.maximum.accumulate(
            np.where(confirmed, indices, -len(confirmed) - 1)
        )
        last_invalid = np.maximum.accumulate(np.where(~valid_mask, indices, -1))
        alarms = (indices - last_confirmation <= hold_windows) & (
            last_confirmation > last_invalid
        )
        alarms &= valid_mask
    if max_active_windows is None:
        return alarms
    capped = np.zeros_like(alarms)
    for left, right in true_runs(alarms):
        capped[left : min(right, left + max_active_windows)] = True
    return capped


def apply_sustained_latch_policy(
    tail_probabilities: np.ndarray,
    sustained_probabilities: np.ndarray,
    recovery_probabilities: np.ndarray,
    valid: np.ndarray,
    *,
    open_threshold: float | np.ndarray,
    keep_threshold: float,
    recovery_threshold: float,
    minimum_consecutive: int = 1,
    max_unsupported_windows: int = 0,
    max_unanchored_windows: int | None = None,
    reanchor_threshold: float | np.ndarray | None = None,
) -> np.ndarray:
    """Causally retain an opened anomaly through transition-free windows.

    Only the sequence-tail detector may open an alarm. Sustained evidence can
    keep an already-open alarm active but can never create an alarm by itself;
    this prevents the persistent branch from introducing standalone nominal
    false positives. A recovery-head confirmation or too many unsupported
    windows closes the latch. ``max_unanchored_windows`` additionally requires
    the opening detector to reconfirm a long alarm periodically. This prevents
    one false boundary detection from being extended indefinitely by a drifted
    sustained score without imposing a fixed maximum duration on real events.
    ``reanchor_threshold`` may be lower than ``open_threshold`` because it can
    extend an existing alarm but can never create one.
    """

    tail = np.asarray(tail_probabilities, dtype=np.float64)
    sustained = np.asarray(sustained_probabilities, dtype=np.float64)
    recovery = np.asarray(recovery_probabilities, dtype=np.float64)
    valid_mask = np.asarray(valid, dtype=bool)
    if any(value.ndim != 1 for value in (tail, sustained, recovery, valid_mask)):
        raise ValueError("all sustained-latch inputs must be one-dimensional")
    if not (tail.shape == sustained.shape == recovery.shape == valid_mask.shape):
        raise ValueError("all sustained-latch inputs must have the same shape")
    open_values = np.asarray(open_threshold, dtype=np.float64)
    if open_values.ndim == 0:
        open_values = np.full(tail.shape, float(open_values), dtype=np.float64)
    if open_values.shape != tail.shape:
        raise ValueError("open_threshold must be scalar or match the score timeline")
    if bool((~np.isfinite(open_values)).any()) or bool(
        ((open_values < 0.0) | (open_values > 1.0)).any()
    ):
        raise ValueError("open_threshold must be finite and in [0, 1]")
    reanchor_values = (
        open_values
        if reanchor_threshold is None
        else np.asarray(reanchor_threshold, dtype=np.float64)
    )
    if reanchor_values.ndim == 0:
        reanchor_values = np.full(
            tail.shape, float(reanchor_values), dtype=np.float64
        )
    if reanchor_values.shape != tail.shape:
        raise ValueError(
            "reanchor_threshold must be scalar or match the score timeline"
        )
    if bool((~np.isfinite(reanchor_values)).any()) or bool(
        ((reanchor_values < 0.0) | (reanchor_values > 1.0)).any()
    ):
        raise ValueError("reanchor_threshold must be finite and in [0, 1]")
    for name, threshold in (
        ("keep_threshold", keep_threshold),
        ("recovery_threshold", recovery_threshold),
    ):
        if not np.isfinite(threshold) or not 0.0 <= threshold <= 1.0:
            raise ValueError(f"{name} must be finite and in [0, 1]")
    if minimum_consecutive <= 0:
        raise ValueError("minimum_consecutive must be positive")
    if max_unsupported_windows < 0:
        raise ValueError("max_unsupported_windows must be non-negative")
    if max_unanchored_windows is not None and max_unanchored_windows < 0:
        raise ValueError("max_unanchored_windows must be non-negative")

    alarms = np.zeros(tail.shape, dtype=bool)
    active = False
    consecutive = 0
    unsupported = 0
    unanchored = 0
    for index in range(len(tail)):
        if not valid_mask[index]:
            active = False
            consecutive = 0
            unsupported = 0
            unanchored = 0
            continue
        tail_open = tail[index] >= open_values[index]
        if not active:
            consecutive = consecutive + 1 if tail_open else 0
            if consecutive >= minimum_consecutive:
                active = True
                unsupported = 0
                unanchored = 0
            alarms[index] = active
            continue
        if tail[index] >= reanchor_values[index]:
            unanchored = 0
        else:
            unanchored += 1
            if (
                max_unanchored_windows is not None
                and unanchored > max_unanchored_windows
            ):
                active = False
                consecutive = 0
                unsupported = 0
                unanchored = 0
                continue
        if recovery[index] >= recovery_threshold and not tail_open:
            active = False
            consecutive = 0
            unsupported = 0
            unanchored = 0
            continue
        if tail_open or sustained[index] >= keep_threshold:
            unsupported = 0
        else:
            unsupported += 1
            if unsupported > max_unsupported_windows:
                active = False
                consecutive = 0
                unsupported = 0
                unanchored = 0
        alarms[index] = active
    return alarms


def true_runs(values: np.ndarray) -> list[tuple[int, int]]:
    """Return half-open intervals for all True runs in a one-dimensional mask."""
    array = np.asarray(values, dtype=bool)
    if array.ndim != 1:
        raise ValueError("values must be a 1D array")
    padded = np.concatenate(([False], array, [False])).astype(np.int8)
    changes = np.diff(padded)
    starts = np.flatnonzero(changes == 1)
    ends = np.flatnonzero(changes == -1)
    return list(zip(starts.tolist(), ends.tolist(), strict=True))


def project_alarm_runs_to_decision_time(
    alarms: np.ndarray,
    starts: np.ndarray,
    *,
    window_size: int,
    stride: int,
    timeline_end: int | None = None,
) -> list[tuple[int, int]]:
    """Project window alarms onto causal decision-time intervals.

    A model cannot issue a decision at the beginning of its input window. Its
    first observable alarm time is the final sample of that window, and the
    decision remains current until the next stride interval. This projection
    therefore decouples input context length from displayed alarm duration.
    """
    alarm_mask = np.asarray(alarms, dtype=bool)
    window_starts = np.asarray(starts, dtype=np.int64)
    if alarm_mask.ndim != 1 or window_starts.ndim != 1:
        raise ValueError("alarms and starts must be 1D arrays")
    if alarm_mask.shape != window_starts.shape:
        raise ValueError("alarms and starts must have the same length")
    if window_size <= 1 or stride <= 0:
        raise ValueError("window_size must exceed one and stride must be positive")
    if timeline_end is not None and timeline_end < 0:
        raise ValueError("timeline_end must be non-negative")

    segments: list[tuple[int, int]] = []
    for left, right in true_runs(alarm_mask):
        decision_left = int(window_starts[left] + window_size - 1)
        decision_right = int(window_starts[right - 1] + window_size - 1 + stride)
        if timeline_end is not None:
            decision_right = min(decision_right, int(timeline_end))
        if decision_right > decision_left:
            segments.append((decision_left, decision_right))
    return segments


def merge_close_segments(
    segments: list[tuple[int, int]], *, maximum_gap: int
) -> list[tuple[int, int]]:
    """Group nearby alarm activations into notification incidents.

    The active-window mask is deliberately left unchanged. This function only
    controls when a repeated activation should create a new notification.
    """
    return [
        (components[0][0], max(right for _left, right in components))
        for components in group_close_segments(segments, maximum_gap=maximum_gap)
    ]


def group_close_segments(
    segments: list[tuple[int, int]], *, maximum_gap: int
) -> list[list[tuple[int, int]]]:
    """Group notifications without treating inactive gaps as active alarms."""
    if maximum_gap < 0:
        raise ValueError("maximum_gap must be non-negative")
    if not segments:
        return []
    ordered = sorted((int(left), int(right)) for left, right in segments)
    if any(left >= right for left, right in ordered):
        raise ValueError("segments must be non-empty half-open intervals")
    groups: list[list[tuple[int, int]]] = [[ordered[0]]]
    for left, right in ordered[1:]:
        previous_right = max(segment_right for _segment_left, segment_right in groups[-1])
        if left - previous_right <= maximum_gap:
            groups[-1].append((left, right))
        else:
            groups.append([(left, right)])
    return groups
