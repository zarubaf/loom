#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Loom — PCIe hot-reset + rescan after FPGA programming
#
# After loading a new bitstream via JTAG, the FPGA's PCIe hard block resets
# and the link goes down.  The kernel still holds a stale device entry whose
# BAR mappings point nowhere (reads return 0xFFFFFFFF).
#
# Recovery sequence:
#   1. Unload XDMA driver
#   2. Record the upstream bridge of the Xilinx endpoint
#   3. Remove the stale endpoint from the kernel
#   4. Issue a Secondary Bus Reset on the upstream bridge — this forces PCIe
#      link retraining with the newly-programmed FPGA
#   5. Rescan the PCIe bus
#   6. Reload the XDMA driver
#
# Usage: sudo ./pcie_rescan.sh
#    or: make -C fpga rescan

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOOM_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
XDMA_KO="$LOOM_ROOT/third_party/dma_ip_drivers/XDMA/linux-kernel/xdma/xdma.ko"

# --- 1. Unload XDMA driver ------------------------------------------------
if lsmod | grep -q xdma; then
    echo "Removing xdma driver..."
    rmmod xdma
fi

# --- 2/3. Find and remove stale Xilinx endpoint(s) ------------------------
bridge_bdf=""
for dev in /sys/bus/pci/devices/*/vendor; do
    dir="$(dirname "$dev")"
    if [ "$(cat "$dir/vendor" 2>/dev/null)" = "0x10ee" ]; then
        bdf_old="$(basename "$dir")"
        # Record the upstream bridge before removing the device
        if [ -L "$dir/driver" ]; then
            echo "$dir/driver" | xargs readlink -f || true
        fi
        # The parent of the device in sysfs is the upstream bridge
        bridge_path="$(dirname "$dir")"
        if [ -f "$bridge_path/config" ]; then
            bridge_bdf="$(basename "$bridge_path")"
        fi
        echo "Removing stale PCIe device $bdf_old..."
        echo 1 > "$dir/remove"
    fi
done
sleep 1

# --- 4. Secondary Bus Reset on upstream bridge ----------------------------
if [ -n "$bridge_bdf" ] && [ -f "/sys/bus/pci/devices/$bridge_bdf/config" ]; then
    echo "Issuing Secondary Bus Reset on bridge $bridge_bdf..."
    # Bridge Control register is at offset 0x3E.  Bit 6 = Secondary Bus Reset.
    # Toggle it: set, wait, clear.
    bridge_cfg="/sys/bus/pci/devices/$bridge_bdf/config"
    bc=$(od -An -tx2 -j 0x3e -N 2 "$bridge_cfg" | tr -d ' ')
    bc_val=$((16#$bc))
    bc_set=$(printf '%04x' $(( bc_val | 0x0040 )))
    bc_clr=$(printf '%04x' $(( bc_val & ~0x0040 )))
    # Write set (big-endian byte order for the 2-byte field)
    printf "\\x${bc_set:2:2}\\x${bc_set:0:2}" | dd of="$bridge_cfg" bs=1 seek=$((0x3e)) conv=notrunc 2>/dev/null
    sleep 0.5
    printf "\\x${bc_clr:2:2}\\x${bc_clr:0:2}" | dd of="$bridge_cfg" bs=1 seek=$((0x3e)) conv=notrunc 2>/dev/null
    sleep 1
    echo "Secondary Bus Reset complete."
else
    echo "WARNING: Could not find upstream bridge — skipping hot reset."
    echo "         A cold reboot may be needed."
fi

# --- 5. Rescan PCIe bus ---------------------------------------------------
echo "Rescanning PCIe bus..."
echo 1 > /sys/bus/pci/rescan
sleep 2

# --- 6. Check for re-enumerated Xilinx device -----------------------------
xlnx_dev=$(lspci | grep -i xilinx | head -1)
if [ -z "$xlnx_dev" ]; then
    echo "ERROR: No Xilinx device found after rescan"
    exit 1
fi
echo "Found: $xlnx_dev"
bdf=$(echo "$xlnx_dev" | cut -d' ' -f1)

# Try loading XDMA driver
if [ -f "$XDMA_KO" ]; then
    echo "Loading xdma driver (third_party)..."
    insmod "$XDMA_KO"
else
    echo "No xdma driver found — enabling sysfs BAR access"
    echo 1 > "/sys/bus/pci/devices/0000:$bdf/enable"
    echo "OK: use loomx -t xdma -d 0000:$bdf"
    exit 0
fi

sleep 1
if [ -e /dev/xdma0_user ]; then
    echo "OK: /dev/xdma0_user available"
else
    echo "WARNING: /dev/xdma0_user not found — try sysfs: loomx -t xdma -d 0000:$bdf"
fi
