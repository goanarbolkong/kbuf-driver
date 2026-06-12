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
from kbufverif.qemu import QemuRunner, resolve_kernel

REPO = Path(__file__).resolve().parent.parent


def pytest_addoption(parser):
    parser.addoption(
        "--kbuf-artifacts", default=str(REPO / "verif" / "_artifacts"),
        help="directory for per-test console/dmesg artifacts")


@pytest.fixture(scope="session")
def runner():
    kernel = resolve_kernel(REPO)
    initrd = build_image(REPO, REPO / ".qemu")
    return QemuRunner(kernel, initrd)


class VM:
    """Boots one disposable VM per ``run()`` call."""

    def __init__(self, runner, artifacts: Path):
        self._runner = runner
        self._artifacts = artifacts
        # TCG (no KVM) boots take minutes, not seconds.
        self.default_boot_timeout = 120 if runner.kvm else 1200

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
def vm(runner, request):
    base = Path(request.config.getoption("--kbuf-artifacts"))
    safe = re.sub(r"[^A-Za-z0-9_.-]+", "_", request.node.nodeid)
    return VM(runner, base / safe)
