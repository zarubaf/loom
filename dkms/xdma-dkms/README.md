# xdma-dkms — Xilinx XDMA Driver (DKMS)

Debian package for the Xilinx XDMA PCIe DMA kernel driver.  DKMS
automatically rebuilds the module whenever the kernel is upgraded.

## Prerequisites

```bash
sudo apt-get install dkms debhelper dh-dkms curl unzip
```

## Build

```bash
cd dkms/xdma-dkms
make deb
```

This fetches the XDMA source from
<https://github.com/Xilinx/dma_ip_drivers>, extracts the Linux kernel
driver, and produces `../xdma-dkms_2020.2.2-1_all.deb`.

## Install

```bash
sudo apt install ./xdma-dkms_2020.2.2-1_all.deb
```

Verify:

```bash
dkms status | grep xdma
sudo modprobe xdma
ls /dev/xdma*
```

## Uninstall

```bash
sudo apt remove xdma-dkms
```

## What it does

- Installs XDMA kernel source to `/usr/src/xdma-2020.2.2/`
- Registers the module with DKMS so it rebuilds on kernel upgrades
- Adds a udev rule (`SUBSYSTEM=="xdma", MODE="0666"`) for non-root access
