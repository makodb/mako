#!/usr/bin/env python3
"""Source model of the private cache, not observed benchmark hit counters."""
import importlib.util
import json
from pathlib import Path

spec = importlib.util.spec_from_file_location('driver', Path(__file__).with_name('run-resolved-variants.py'))
driver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(driver)


def bucket(table, key):
    value = table ^ 0x9E3779B97F4A7C15
    for offset in range(0, len(key), 8):
        value ^= int.from_bytes(key[offset:offset+8], 'little')
        value = (value * 0xBF58476D1CE4E5B9) & driver.MASK
        value ^= value >> 27
    return driver.mix(value ^ len(key)) & 511


def modeled_hits(scenario, worker=0, warmup=65536, iterations=262144):
    distribution, requested, key_bytes, tables = driver.SCENARIOS[scenario]
    keys = requested or warmup + iterations
    if key_bytes > 32 or distribution == 'stream':
        return 0
    positions = {(table, key): bucket(table+1, (worker*keys+key).to_bytes(8, 'big') + b'Z'*(key_bytes-8))
                 for table in range(tables) for key in range(keys)}
    entries = [None] * 512
    hits = 0
    for iteration in range(warmup+iterations):
        identity = driver.identity(distribution, keys, tables, driver.SEED, iteration)
        position = positions[identity]
        if iteration >= warmup and entries[position] == identity:
            hits += 1
        entries[position] = identity
    return hits


if __name__ == '__main__':
    print(json.dumps({
        'kind': 'source-model-not-measured-counters',
        'assumptions': 'W1 fresh private cache, table object IDs1..tables, all puts succeed, exact current hash and schedule, no interleaved operations',
        'entries': 512, 'storage_bytes_per_worker': 32768,
        'warmup_per_worker': 65536, 'iterations_per_worker': 262144,
        'results': {name: {'modeled_measured_hits': modeled_hits(name),
                           'modeled_measured_hit_fraction': modeled_hits(name)/262144,
                           'long_key_bypass': values[2] > 32}
                    for name, values in driver.SCENARIOS.items()},
    }, indent=2, sort_keys=True))
