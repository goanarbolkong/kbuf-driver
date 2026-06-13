# SPDX-License-Identifier: GPL-2.0
"""pytest wiring: one guest image per session, one QEMU boot per test.

Each test gets a ``vm`` fixture whose ``run()`` boots a fresh VM, captures
the full serial console and guest dmesg as per-test artifacts, and fails
the test if the guest never reached the final marker (hang/oops/panic).
"""

import re
from pathlib import Path

import pytest

from kbufverif import build_image
from kbufverif.qemu import QemuRunner, resolve_kernel, resolve_variant

REPO = Path(__file__).resolve().parent.parent


def pytest_addoption(parser):
    parser.addoption(
        "--kbuf-artifacts", default=str(REPO / "verif" / "_artifacts"),
        help="directory for per-test console/dmesg artifacts")
    parser.addoption(
        "--kbuf-variant", default=None, choices=["kasan", "kcsan"],
        help="boot a debug-kernel variant built by "
             "scripts/build-debug-kernel.sh; enables the gate tests")


@pytest.fixture(scope="session")
def variant(request):
    return request.config.getoption("--kbuf-variant")


def pytest_collection_modifyitems(config, items):
    """Gate tests only make sense on an instrumented kernel; skip otherwise."""
    if config.getoption("--kbuf-variant"):
        return
    skip = pytest.mark.skip(reason="needs --kbuf-variant (KASAN/KCSAN kernel)")
    for item in items:
        if "gate" in item.keywords:
            item.add_marker(skip)


@pytest.fixture(scope="session")
def runner(variant):
    if variant:
        kernel, kdir = resolve_variant(REPO, variant)
        initrd = build_image(REPO, REPO / ".qemu", kdir=kdir)
        # Instrumented kernels need more headroom than the 512 MiB default.
        return QemuRunner(kernel, initrd, mem=2048)
    kernel = resolve_kernel(REPO)
    initrd = build_image(REPO, REPO / ".qemu")
    return QemuRunner(kernel, initrd)


class VM:
    """Boots one disposable VM per ``run()`` call."""

    def __init__(self, runner, artifacts: Path, slow: bool = False):
        self._runner = runner
        self._artifacts = artifacts
        # TCG (no KVM) boots take minutes, not seconds; KASAN/KCSAN
        # instrumentation slows even a KVM guest by a large factor.
        base = 120 if runner.kvm else 1200
        self.default_boot_timeout = base * 4 if slow else base

    def run(self, cmd=None, ndevices=None, guest_timeout=None,
            noinsmod=False, boot_timeout=None, expect_done=True):
        params = []
        if ndevices is not None:
            params.append(f"kbuf.ndevices={ndevices}")
        if noinsmod:
            params.append("kbuf.noinsmod")
        if cmd:
            params.append(f"kbuf.cmd={cmd}")
        if guest_timeout is not None:
            params.append(f"kbuf.timeout={guest_timeout}")

        res = self._runner.boot(" ".join(params),
                                timeout=boot_timeout
                                or self.default_boot_timeout)

        self._artifacts.mkdir(parents=True, exist_ok=True)
        (self._artifacts / "console.log").write_text(res.serial)
        (self._artifacts / "dmesg.txt").write_text(res.dmesg)

        if expect_done:
            assert res.completed, (
                f"guest never reached the done marker "
                f"(timed_out={res.timed_out}); see "
                f"{self._artifacts / 'console.log'}")
        return res


@pytest.fixture
def vm(runner, request, variant):
    base = Path(request.config.getoption("--kbuf-artifacts"))
    safe = re.sub(r"[^A-Za-z0-9_.-]+", "_", request.node.nodeid)
    return VM(runner, base / safe, slow=bool(variant))
