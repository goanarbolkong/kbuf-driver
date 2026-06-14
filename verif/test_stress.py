# SPDX-License-Identifier: GPL-2.0
"""Concurrency/integrity stress: pinned producer+consumer pairs."""

import pytest

STRESS_TESTS = [
    "test_spsc",   # 20k sequenced msgs through the lock-free ring
    "test_mmap",   # 4 MiB through the 64 KiB magic ring (~64 wraps)
]


@pytest.mark.stress
@pytest.mark.parametrize("name", STRESS_TESTS)
def test_stress(vm, name):
    res = vm.run(cmd=f"/tests/{name}", guest_timeout=90,
                 boot_timeout=vm.default_boot_timeout + 120)
    assert res.rc("cmd") == 0, f"{name} failed inside the guest"
    assert not res.oops, "kernel reported a BUG/oops during the test"


@pytest.mark.stress
def test_spsc_handoff(vm):
    """SPSC mode, producer and consumer pinned to different cores: the kbuf:*
    tracepoints must show both produce and consume events, tagged on two
    distinct CPUs (the cross-core release/acquire hand-off). Also the data
    source for docs/img/spsc_handoff.png (scripts/plot_spsc.py)."""
    res = vm.run(cmd="/scenarios/spsc_capture.sh",
                 boot_timeout=vm.default_boot_timeout + 60)
    assert res.rc("cmd") == 0
    assert not res.oops
    assert "kbuf_produce" in res.serial, "no produce events captured"
    assert "kbuf_consume" in res.serial, "no consume events captured"
    cpus = {line[line.index("["):line.index("]") + 1]
            for line in res.serial.splitlines()
            if ("kbuf_produce" in line or "kbuf_consume" in line) and "[" in line}
    assert len(cpus) >= 2, f"hand-off stayed on one core: {cpus}"
