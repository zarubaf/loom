#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Loom — PCIe rescan after FPGA programming
#
# After loading a new bitstream via JTAG, the PCIe link drops.
# This script re-enumerates the device and reloads the XDMA driver.
#
# Usage: sudo ./pcie_rescan.sh

set -e

# Remove XDMA driver (if loaded)
if lsmod | grep -q xdma; then
    echo "Removing xdma driver..."
    rmmod xdma
fi

# Rescan PCIe bus
echo "Rescanning PCIe bus..."
echo 1 > /sys/bus/pci/rescan
sleep 1

# Check for Xilinx device
xlnx_dev=$(lspci | grep -i xilinx | head -1)
if [ -z "$xlnx_dev" ]; then
    echo "ERROR: No Xilinx device found after rescan"
    exit 1
fi
echo "Found: $xlnx_dev"

# Reload XDMA driver
if modinfo xdma &>/dev/null; then
    echo "Loading xdma driver..."
    modprobe xdma
    sleep 1

    if [ -e /dev/xdma0_user ]; then
        echo "OK: /dev/xdma0_user is available"
    else
        echo "WARNING: /dev/xdma0_user not found — driver may not have bound"
        echo "Try: loomx -t xdma -d $(echo "$xlnx_dev" | cut -d' ' -f1)"
    fi
else
    bdf=$(echo "$xlnx_dev" | cut -d' ' -f1)
    echo "xdma driver not installed — enabling sysfs BAR access"
    echo 1 > "/sys/bus/pci/devices/0000:$bdf/enable"
    echo "OK: use loomx -t xdma -d 0000:$bdf"
fi
