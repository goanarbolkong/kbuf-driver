# SPDX-License-Identifier: GPL-2.0
"""kbufverif — host-side verification framework for the kbuf driver.

Boots disposable QEMU VMs that load the freshly built module and run one
guest workload per boot, selected via the kernel command line.  Results come
back as structured ``KBUF_VERIF:`` markers on the serial console; pytest
turns them into individual test verdicts with per-test artifacts.
"""

from .qemu import BootResult, QemuRunner  # noqa: F401
from .initramfs import build_image  # noqa: F401
