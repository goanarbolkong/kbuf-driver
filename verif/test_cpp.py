# SPDX-License-Identifier: GPL-2.0
"""Run the kbuf++ GoogleTest suite inside the guest (Phase 12).

The suite drives the C++ RAII wrapper (include/kbufpp.hpp) against a real
device. It links a statically built GoogleTest, so it is skipped unless that
library is present (scripts/fetch-googletest.sh); the C test suite never
depends on it.
"""

from pathlib import Path

import pytest

from kbufverif.initramfs import gtest_available

REPO = Path(__file__).resolve().parent.parent

pytestmark = pytest.mark.skipif(
    not gtest_available(REPO),
    reason="googletest not built; run scripts/fetch-googletest.sh")


@pytest.mark.functional
def test_kbufpp(vm):
    """The whole kbuf++ GoogleTest binary passes with no kernel splat."""
    res = vm.run(cmd="/tests/test_kbufpp")
    assert res.rc("cmd") == 0, "kbuf++ GoogleTest suite failed in the guest"
    assert not res.oops, "kernel splat during the kbuf++ suite"
