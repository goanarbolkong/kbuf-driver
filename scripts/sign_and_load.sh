#!/bin/bash
# sign_and_load.sh — sign kbuf_driver.ko with a MOK and load it
#
# Run ONCE to enroll the key (requires one reboot to confirm in UEFI):
#   bash sign_and_load.sh enroll
#
# After reboot, to just sign + load:
#   bash sign_and_load.sh load
#
# To unload:
#   bash sign_and_load.sh unload

set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

SIGN_TOOL="/usr/src/linux-headers-$(uname -r)/scripts/sign-file"
KEY="$DIR/MOK.key"
CERT="$DIR/MOK.crt"
MODULE="$DIR/kbuf_driver.ko"

sign_module() {
    if [ ! -f "$KEY" ] || [ ! -f "$CERT" ]; then
        echo "[*] Generating MOK key pair..."
        openssl req -new -x509 -newkey rsa:2048 \
            -keyout "$KEY" -out "$CERT" -days 36500 \
            -subj "/CN=kbuf-lab-mok/" -nodes
        echo "[*] Key generated: MOK.key / MOK.crt"
    fi

    echo "[*] Signing $MODULE ..."
    sudo "$SIGN_TOOL" sha256 "$KEY" "$CERT" "$MODULE"
    echo "[*] Module signed."
}

case "${1:-load}" in
    enroll)
        sign_module
        echo ""
        echo "[*] Converting certificate to DER format for mokutil..."
        CERT_DER="$DIR/MOK.der"
        openssl x509 -in "$CERT" -outform DER -out "$CERT_DER"
        echo "[*] Enrolling MOK into UEFI (you will be prompted for a one-time password)..."
        echo "    Choose any simple password — you'll type it once on the next boot screen."
        sudo mokutil --import "$CERT_DER"
        echo ""
        echo "===================================================================="
        echo "  REBOOT NOW, then at the blue MOK Manager screen:"
        echo "    1. Select 'Enroll MOK'  →  'Continue'  →  'Yes'"
        echo "    2. Enter the password you just set"
        echo "    3. Select 'Reboot'"
        echo ""
        echo "  After reboot, run:  bash sign_and_load.sh load"
        echo "===================================================================="
        ;;
    load)
        sign_module
        if lsmod | grep -q kbuf_driver; then
            echo "[*] Already loaded — reloading..."
            sudo rmmod kbuf_driver
        fi
        echo "[*] Loading module..."
        sudo insmod "$MODULE"
        echo "[*] Loaded. Device: $(ls /dev/kbuf)"
        echo "[*] /proc/kbuf_status:"
        cat /proc/kbuf_status
        ;;
    unload)
        sudo rmmod kbuf_driver && echo "[*] Unloaded."
        ;;
    *)
        echo "Usage: $0 [enroll|load|unload]"
        exit 1
        ;;
esac
