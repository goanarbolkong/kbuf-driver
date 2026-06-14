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


@pytest.mark.functional
def test_trace_backpressure(vm):
    """Fast producer vs slow consumer: the kbuf:* tracepoints must show the
    8-slot ring saturate (backpressure) and fire wakeups. Also the data source
    for docs/img/trace_timeline.png (scripts/plot_trace.py)."""
    res = vm.run(cmd="/scenarios/trace_capture.sh")
    assert res.rc("cmd") == 0
    assert not res.oops
    assert "occ=8" in res.serial, "ring never saturated (no backpressure seen)"
    assert "kbuf_wakeup" in res.serial, "no wakeup events recorded"
