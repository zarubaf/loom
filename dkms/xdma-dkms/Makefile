VERSION := 2020.2.2
GITHUB_URL := https://github.com/Xilinx/dma_ip_drivers/archive/refs/heads/master.zip
SRC_DIR := $(CURDIR)/src

.PHONY: deb clean setup

setup:
	rm -rf $(SRC_DIR) /tmp/dma_ip_drivers.zip /tmp/dma_ip_drivers-master
	curl -L -o /tmp/dma_ip_drivers.zip $(GITHUB_URL)
	unzip -q /tmp/dma_ip_drivers.zip -d /tmp
	mkdir -p $(SRC_DIR)
	cp -a /tmp/dma_ip_drivers-master/XDMA/linux-kernel/xdma $(SRC_DIR)/
	cp -a /tmp/dma_ip_drivers-master/XDMA/linux-kernel/include $(SRC_DIR)/
	rm -rf /tmp/dma_ip_drivers.zip /tmp/dma_ip_drivers-master

deb: setup
	dpkg-buildpackage -us -uc -b

clean:
	rm -rf $(SRC_DIR)
	dh clean || true
