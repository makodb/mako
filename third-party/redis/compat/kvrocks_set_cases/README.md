# Kvrocks-Derived Set Compatibility Cases

These tests port in-scope set command cases from Apache Kvrocks'
`tests/gocase/unit/type/set/set_test.go` into Mako's `pytest + redis-py`
compatibility harness.

Source:

- https://github.com/apache/kvrocks/blob/unstable/tests/gocase/unit/type/set/set_test.go

The goal is not to run the Kvrocks harness unchanged. Kvrocks starts its own
server binary and includes Kvrocks-specific configuration, encoding, and
unsupported-command coverage. This folder keeps the reusable command cases and
expected Redis replies in a Mako-native format.

Run against Mako:

```bash
python3 -m pytest third-party/redis/compat/kvrocks_set_cases -q
```

Optionally compare each case against a reference Redis on port 6379:

```bash
KVROCKS_CASES_COMPARE_REDIS=1 python3 -m pytest third-party/redis/compat/kvrocks_set_cases -q
```

