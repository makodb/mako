"""Linux CPU evidence for the shared-host execution-plus-steal policy.

I/O wait is reported separately, not vetoed as execution contention. Guest
counters are retained for provenance but already belong to user/nice totals.
Invalid evidence raises RuntimeError; it is never clamped into passing data.
"""

from fractions import Fraction
import os
from pathlib import Path
import time
import uuid

POLICY = "shared-host-worker-smt-execution-v2"
SCHEMA = "linux-cpu-accounting-v2"
COUNTER_FIELDS = ("user", "nice", "system", "idle", "iowait", "irq", "softirq", "steal")
_OPTIONAL_FIELDS = ("guest", "guest_nice")
_EXECUTION_FIELDS = ("user", "nice", "system", "irq", "softirq")


def _integer(value, label, minimum=0):
    if type(value) is not int or value < minimum:
        raise RuntimeError(f"invalid CPU accounting {label}: {value!r}")
    return value


def _cpu_key(cpu):
    if type(cpu) is int and cpu >= 0:
        return str(cpu)
    if isinstance(cpu, str) and cpu.isascii() and cpu.isdecimal() and str(int(cpu)) == cpu:
        return cpu
    raise RuntimeError(f"invalid CPU identity: {cpu!r}")


def _endpoint(value, key):
    if not isinstance(value, dict) or value.get("schema") != SCHEMA:
        raise RuntimeError("invalid CPU accounting schema")
    boot = value.get("boot_id")
    try:
        if not isinstance(boot, str) or str(uuid.UUID(boot)) != boot:
            raise ValueError("not a canonical boot UUID")
    except ValueError as error:
        raise RuntimeError("invalid CPU accounting boot identity") from error
    _integer(value.get("clock_ticks_per_second"), "clock tick frequency", 1)
    start = _integer(value.get("before_ns"), "read start")
    end = _integer(value.get("after_ns"), "read end")
    if start > end:
        raise RuntimeError("reversed CPU accounting read bracket")
    cpus = value.get("cpus")
    if not isinstance(cpus, dict) or key not in cpus or not isinstance(cpus[key], dict):
        raise RuntimeError(f"missing CPU accounting record for CPU {key}")
    counters = cpus[key]
    for field in COUNTER_FIELDS:
        _integer(counters.get(field), f"CPU {key} {field}")
    for field in _OPTIONAL_FIELDS:
        if field in counters:
            _integer(counters[field], f"CPU {key} {field}")
    return counters


def snapshot(cpus):
    """Read selected per-CPU counters once inside monotonic read brackets."""
    try:
        selected = {_cpu_key(cpu) for cpu in cpus}
        boot = Path("/proc/sys/kernel/random/boot_id").read_text().strip()
        frequency = os.sysconf("SC_CLK_TCK")
        start = time.monotonic_ns()
        text = Path("/proc/stat").read_text()
        end = time.monotonic_ns()
        records = {}
        for line in text.splitlines():
            parts = line.split()
            if not parts or not parts[0].startswith("cpu"):
                continue
            key = parts[0][3:]
            if key not in selected:
                continue
            if key in records:
                raise RuntimeError(f"duplicate CPU accounting record for CPU {key}")
            if len(parts) < 9:
                raise RuntimeError(f"incomplete CPU accounting record for CPU {key}")
            records[key] = dict(zip(COUNTER_FIELDS + _OPTIONAL_FIELDS,
                                    (int(field) for field in parts[1:11])))
        result = {"schema": SCHEMA, "boot_id": boot,
                  "clock_ticks_per_second": frequency, "before_ns": start,
                  "after_ns": end, "cpus": records}
        for key in selected:
            _endpoint(result, key)
        return result
    except (OSError, ValueError, TypeError, OverflowError) as error:
        raise RuntimeError(f"unavailable CPU accounting snapshot: {error}") from error


def interval(before, after, cpu):
    """Validate raw endpoints and return execution, contention and idle metrics."""
    key = _cpu_key(cpu)
    first = _endpoint(before, key)
    last = _endpoint(after, key)
    for field in ("schema", "boot_id", "clock_ticks_per_second"):
        if before[field] != after[field]:
            raise RuntimeError(f"CPU accounting {field} changed between endpoints")
    if before["after_ns"] > after["before_ns"]:
        raise RuntimeError("overlapping or reversed CPU accounting endpoints")
    ticks = {field: last[field] - first[field] for field in COUNTER_FIELDS}
    if any(value < 0 for value in ticks.values()):
        raise RuntimeError(f"decreasing CPU accounting counter for CPU {key}")
    total = sum(ticks.values())
    if total <= 0:
        raise RuntimeError(f"nonpositive CPU accounting total for CPU {key}")
    execution = sum(ticks[field] for field in _EXECUTION_FIELDS)
    contention = execution + ticks["steal"]
    return {"ticks": ticks, "total_ticks": total, "execution_ticks": execution,
            "contention_ticks": contention, "idle_percent": 100 * ticks["idle"] / total,
            "iowait_fraction": ticks["iowait"] / total,
            "steal_fraction": ticks["steal"] / total,
            "execution_fraction": execution / total,
            "contention_fraction": contention / total}


def within_limit(metrics, ceiling):
    """Compare inclusive rational contention ceilings without float cancellation."""
    try:
        if isinstance(ceiling, bool):
            raise ValueError("boolean ceiling")
        limit = Fraction(str(ceiling))
        if not 0 <= limit <= 1:
            raise ValueError("ceiling outside [0, 1]")
        if not isinstance(metrics, dict):
            raise RuntimeError("invalid CPU accounting metrics")
        total = _integer(metrics.get("total_ticks"), "total ticks", 1)
        contention = _integer(metrics.get("contention_ticks"), "contention ticks")
        if contention > total:
            raise RuntimeError("CPU contention exceeds total ticks")
        return contention * limit.denominator <= total * limit.numerator
    except (ValueError, TypeError, ZeroDivisionError, OverflowError) as error:
        raise RuntimeError(f"invalid CPU contention ceiling: {ceiling!r}") from error
