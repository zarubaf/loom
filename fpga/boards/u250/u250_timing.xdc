# SPDX-License-Identifier: Apache-2.0
# Loom — Alveo U250 timing constraints

# ----------------------------------------------------------------
# External clocks
# ----------------------------------------------------------------

# PCIe reference clock (100 MHz)
set refclk_pcie [create_clock -name refclk_pcie -period 10.000 [get_ports pcie_refclk_p]]

# Emulation reference clock (300 MHz)
set refclk_emu [create_clock -name refclk_emu -period 3.333 [get_ports refclk_p]]

# External clocks are asynchronous
set_clock_groups -name async_refclks -asynchronous \
  -group $refclk_pcie \
  -group $refclk_emu

# ----------------------------------------------------------------
# False paths
# ----------------------------------------------------------------

set_false_path -through [get_ports pcie_perst_n]

# Reset synchronizer is a proper 2-stage sync — no timing path needed
set_false_path -to [get_pins {rst_sync_q1_reg/D}]
