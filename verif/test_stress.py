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
