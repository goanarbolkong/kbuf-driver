# SPDX-License-Identifier: GPL-2.0
"""Memory and race verification gates.

These run only with ``--kbuf-variant=kasan|kcsan`` (see conftest), i.e. on a
debug kernel built by ``scripts/build-debug-kernel.sh``. The driver's own
workloads are re-run under the instrumentation and the boot fails if the
kernel emits *any* KASAN / KCSAN / lockdep / BUG splat: ``res.oops`` already
greps the serial log for those signatures.

- KASAN guest: generic KASAN + kmemleak + lockdep + slab fault injection.
  ``failslab`` drives every ring-allocation error path; the workloads exercise
  use-after-free / out-of-bounds coverage on the data ring and the dynamic
  device lifetime (the ctl path that bit us once, see DEBUGGING.md).
- KCSAN guest: the data-race detector, aimed at the lock-free SPSC ring's
  release/acquire handoff under a pinned producer/consumer pair.
"""

import pytest

# Workloads worth re-running under instrumentation. Names map to /tests/<name>
# static binaries already shipped in the guest image.
KASAN_WORKLOADS = [
    "test_spsc",    # ring index churn; OOB/UAF on the slot buffers
    "test_mmap",    # vmalloc_user magic ring; OOB across the double map
    "test_ctl",     # dynamic create/destroy; kref lifetime, freed cdev
    "test_edge",    # partial reads + signal interruption error paths
    "test_dmabuf",  # dma-buf export/import; sg_table + vmap, attach lifetime
]


@pytest.mark.gate
def test_failslab_unwind(vm):
    """Ring-alloc -ENOMEM unwind stays clean under slab fault injection."""
    res = vm.run(cmd="/scenarios/failslab.sh", guest_timeout=120,
                 boot_timeout=vm.default_boot_timeout + 240)
    assert res.rc("cmd") == 0, "failslab scenario reported a failure"
    assert not res.oops, "instrumentation splat during fault injection"


@pytest.mark.gate
@pytest.mark.parametrize("name", KASAN_WORKLOADS)
def test_workload_clean(vm, name):
    """Each workload completes with no KASAN/KCSAN/lockdep/BUG report."""
    res = vm.run(cmd=f"/tests/{name}", guest_timeout=120,
                 boot_timeout=vm.default_boot_timeout + 240)
    assert res.rc("cmd") == 0, f"{name} failed inside the guest"
    assert not res.oops, f"instrumentation splat during {name}"
