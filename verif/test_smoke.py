# SPDX-License-Identifier: GPL-2.0
"""Boot + insmod + device-topology sanity."""

import pytest


@pytest.mark.smoke
def test_boot_insmod_devices(vm):
    res = vm.run(cmd="/scenarios/smoke.sh")
    assert res.rc("insmod") == 0, "insmod failed"
    assert res.rc("cmd") == 0, "device sanity checks failed"
    assert not res.oops, "kernel reported a BUG/oops during the boot"
