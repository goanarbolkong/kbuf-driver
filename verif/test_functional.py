# SPDX-License-Identifier: GPL-2.0
"""One boot per functional guest test binary."""

import pytest

FUNCTIONAL_TESTS = [
    "test_nonblock",   # O_NONBLOCK semantics (EAGAIN both directions)
    "test_poll",       # poll/epoll readiness, level-triggered
    "test_ioctl",      # stats/reset/resize/mode UAPI
    "test_edge",       # partial-read datagram semantics, signal -> EINTR
    "test_multi",      # per-device ring independence
    "test_ctl",        # /dev/kbuf-ctl dynamic create/destroy (kref)
]


@pytest.mark.functional
@pytest.mark.parametrize("name", FUNCTIONAL_TESTS)
def test_functional(vm, name):
    res = vm.run(cmd=f"/tests/{name}")
    assert res.rc("cmd") == 0, f"{name} failed inside the guest"
    assert not res.oops, "kernel reported a BUG/oops during the test"


@pytest.mark.functional
def test_roundtrip_observability(vm):
    """Producer/consumer round-trip; debugfs counter + tracepoint fire."""
    res = vm.run(cmd="/scenarios/roundtrip.sh")
    assert res.rc("cmd") == 0
    assert not res.oops
