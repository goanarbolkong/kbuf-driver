# SPDX-License-Identifier: GPL-2.0
"""Module lifecycle: refcounting and unload/reload cycles."""

import pytest


@pytest.mark.module
def test_unload_under_load(vm):
    """rmmod must be refused (EBUSY) while a device fd is open."""
    res = vm.run(cmd="/scenarios/unload_under_load.sh")
    assert res.rc("cmd") == 0
    assert not res.oops


@pytest.mark.module
def test_reload_with_param(vm):
    """Full rmmod/insmod cycle honouring ndevices=2."""
    res = vm.run(cmd="/scenarios/reload.sh")
    assert res.rc("cmd") == 0
    assert not res.oops
