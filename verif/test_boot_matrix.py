# SPDX-License-Identifier: GPL-2.0
"""Parameterized boot matrix over the ndevices= module parameter."""

import pytest


@pytest.mark.matrix
@pytest.mark.parametrize("ndevices", [1, 2, 8, 64])
def test_ndevices_matrix(vm, ndevices):
    """Each boot loads the module with a different ndevices= and checks
    that exactly that many static device nodes appear."""
    res = vm.run(cmd=f"/scenarios/smoke.sh,{ndevices}", ndevices=ndevices)
    assert res.rc("insmod") == 0, f"insmod ndevices={ndevices} failed"
    assert res.rc("cmd") == 0, f"device sanity failed at ndevices={ndevices}"
    assert not res.oops
