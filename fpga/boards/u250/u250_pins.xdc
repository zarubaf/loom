# SPDX-License-Identifier: Apache-2.0
# Loom — Alveo U250 pin constraints
#
# Most pins are handled by the IP cores. Only manually-assigned pins go here.

# Emulation reference clock (default_300mhz_clk0)
set_property BOARD_PART_PIN {default_300mhz_clk0_p} [get_ports refclk_p]
set_property BOARD_PART_PIN {default_300mhz_clk0_n} [get_ports refclk_n]

# PCIe reference clock
set_property BOARD_PART_PIN {pcie_mgt_clkn} [get_ports pcie_refclk_n]
set_property BOARD_PART_PIN {pcie_mgt_clkp} [get_ports pcie_refclk_p]
