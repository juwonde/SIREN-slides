"""Shared input-contract check used by the original compression modules."""

def need(ok, message):
    if not ok:
        raise ValueError(message)
