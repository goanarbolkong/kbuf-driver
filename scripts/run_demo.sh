#!/bin/bash
# run_demo.sh — kbuf_driver complete demo
# Run from the kbuf_driver/ directory:
#   cd ~/os_lab/kbuf_driver
#   bash run_demo.sh

set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
info()  { echo -e "${GREEN}[INFO]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
step()  { echo -e "\n${YELLOW}======== $* ========${NC}"; }

# ── 0. make sure binaries exist ───────────────────────────────────────────────
if [ ! -x "$DIR/test_producer" ] || [ ! -x "$DIR/test_consumer" ] || [ ! -x "$DIR/test_nonblock" ]; then
    info "Compiling userspace programs..."
    make userspace
fi

# ── 1. unload any stale instance ─────────────────────────────────────────────
step "Step 1: Load driver"
if lsmod | grep -q kbuf_driver; then
    warn "kbuf_driver already loaded — unloading first"
    sudo rmmod kbuf_driver
fi

sudo insmod kbuf_driver.ko
sudo chmod a+rw /dev/kbuf
info "Module loaded. Device node:"
ls -l /dev/kbuf
info "Initial /proc/kbuf_status:"
cat /proc/kbuf_status

# ── 2. non-blocking test ──────────────────────────────────────────────────────
step "Step 2: Non-blocking mode test"
"$DIR/test_nonblock"

info "/proc/kbuf_status after nonblock test:"
cat /proc/kbuf_status

# ── 3. blocking producer + consumer ──────────────────────────────────────────
step "Step 3: Blocking producer/consumer (producer 200ms/msg, consumer 600ms/msg)"
info "Starting consumer first (will block — buffer is empty)"
"$DIR/test_consumer" 6 600 &
CONSUMER_PID=$!

sleep 0.3
info "Starting producer (faster than consumer → will eventually block on full buffer)"
"$DIR/test_producer" 6 200 &
PRODUCER_PID=$!

wait $PRODUCER_PID
wait $CONSUMER_PID

info "/proc/kbuf_status after blocking test:"
cat /proc/kbuf_status

# ── 4. dmesg ─────────────────────────────────────────────────────────────────
step "Step 4: Kernel log (dmesg)"
sudo dmesg | grep kbuf | tail -30

# ── 5. unload ─────────────────────────────────────────────────────────────────
step "Step 5: Unload driver"
sudo rmmod kbuf_driver
info "Unloaded. /dev/kbuf gone: $(ls /dev/kbuf 2>&1 || echo 'confirmed')"
sudo dmesg | grep kbuf | tail -5

echo -e "\n${GREEN}Demo complete.${NC}"
