# SPDX-License-Identifier: GPL-2.0
"""Boot a disposable QEMU VM and parse the structured serial markers."""

import os
import re
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

BASE_APPEND = "console=ttyS0 panic=-1 oops=panic loglevel=4 rdinit=/init"

_MARKER_RC = re.compile(r"^KBUF_VERIF: (\w+) rc=(\d+)\s*$", re.M)
_DMESG = re.compile(r"^KBUF_VERIF: dmesg-begin\s*$(.*?)^KBUF_VERIF: dmesg-end",
                    re.M | re.S)


@dataclass
class BootResult:
    serial: str
    timed_out: bool
    rcs: dict = field(init=False)

    def __post_init__(self):
        self.rcs = {m.group(1): int(m.group(2))
                    for m in _MARKER_RC.finditer(self.serial)}

    @property
    def completed(self) -> bool:
        """The guest reached the final marker (no hang, oops or panic)."""
        return "KBUF_VERIF: done" in self.serial and not self.timed_out

    def rc(self, stage: str) -> int:
        """Exit code of a stage ('insmod', 'cmd'); -1 if marker missing."""
        return self.rcs.get(stage, -1)

    @property
    def dmesg(self) -> str:
        m = _DMESG.search(self.serial)
        return m.group(1).strip() if m else ""

    @property
    def oops(self) -> bool:
        return bool(re.search(r"BUG:|Oops:|general protection|"
                              r"Call Trace:|KASAN|KCSAN", self.serial))


class QemuRunner:
    def __init__(self, kernel: Path, initrd: Path, mem: int = 512,
                 smp: int = 2):
        self.kernel = Path(kernel)
        self.initrd = Path(initrd)
        self.mem = mem
        self.smp = smp
        self.kvm = os.access("/dev/kvm", os.W_OK)

    def boot(self, params: str = "", timeout: int = 120) -> BootResult:
        accel = ["-enable-kvm", "-cpu", "host"] if self.kvm \
            else ["-cpu", "qemu64"]
        cmd = ["qemu-system-x86_64", *accel,
               "-kernel", str(self.kernel),
               "-initrd", str(self.initrd),
               "-append", f"{BASE_APPEND} {params}".strip(),
               "-m", str(self.mem), "-smp", str(self.smp),
               "-nographic", "-no-reboot"]
        # The serial stream is not guaranteed to be valid UTF-8 (guest tests
        # may print raw buffer bytes), so capture bytes and decode leniently.
        def _txt(b):
            return b.decode(errors="replace") if b else ""

        try:
            proc = subprocess.run(cmd, capture_output=True, timeout=timeout)
            return BootResult(serial=_txt(proc.stdout) + _txt(proc.stderr),
                              timed_out=False)
        except subprocess.TimeoutExpired as e:
            return BootResult(serial=_txt(e.stdout) + _txt(e.stderr),
                              timed_out=True)


def resolve_kernel(repo: Path) -> Path:
    """Prefer the provisioned .qemu/bzImage, fall back to the host kernel."""
    provisioned = Path(repo) / ".qemu" / "bzImage"
    if provisioned.is_file() and os.access(provisioned, os.R_OK):
        return provisioned
    host = Path(f"/boot/vmlinuz-{os.uname().release}")
    if host.is_file() and os.access(host, os.R_OK):
        return host
    raise RuntimeError(
        f"no readable kernel image; provision one:\n"
        f"    sudo install -D -m644 {host} {provisioned}")
