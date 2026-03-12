# SPDX-License-Identifier: Apache-2.0
# Loom — DFX pblock for the reconfigurable partition (loom_emu_top)
#
# U250 DFX pblock — verified clock region layout (8 cols x 4 rows per SLR):
#
# Static hard blocks (7 clock regions total, confirmed via device query):
#   SLR0 X0Y0   — ICAP_ULTRASCALE (loom_icap_ctrl, PCIe DFX programming)
#   SLR0 X4Y3   — emu_refclk IBUFDS (IOB_X0Y182/183)
#   SLR1 X4Y4   — pcie_perst_n IOB_X0Y255
#   SLR1 X7Y4   — PCIE40E4_X0Y1
#   SLR1 X7Y5   — (PCIe clock routing)
#   SLR1 X7Y6   — GTYE4_COMMON_X1Y6
#   SLR1 X7Y7   — GTYE4_CHANNEL_X1Y30/31
#
# Static logic (~19K LUTs) fits comfortably in these 7 CRs (~91K LUT capacity).
# RP gets 121/128 CRs ≈ 94.5% of the device.
#
# Note: CLOCKREGION_X0Y0 is excluded from the RP to allow placement of
# ICAP_ULTRASCALE (hard block at ICAP_X0Y0) in the static region.

create_pblock rp_emu_top
add_cells_to_pblock [get_pblocks rp_emu_top] [get_cells u_emu_top]

# SLR0: all except X0Y0 (ICAP) and X4Y3 (emu_refclk IBUFDS)
resize_pblock [get_pblocks rp_emu_top] -add {CLOCKREGION_X1Y0:CLOCKREGION_X3Y3}
resize_pblock [get_pblocks rp_emu_top] -add {CLOCKREGION_X0Y1:CLOCKREGION_X0Y3}
resize_pblock [get_pblocks rp_emu_top] -add {CLOCKREGION_X4Y0:CLOCKREGION_X4Y2}
resize_pblock [get_pblocks rp_emu_top] -add {CLOCKREGION_X5Y0:CLOCKREGION_X7Y3}

# SLR1: all except X4Y4 (pcie_perst_n IO) and column X7 (PCIe/GT hard blocks)
resize_pblock [get_pblocks rp_emu_top] -add {CLOCKREGION_X0Y4:CLOCKREGION_X3Y7}
resize_pblock [get_pblocks rp_emu_top] -add {CLOCKREGION_X4Y5:CLOCKREGION_X4Y7}
resize_pblock [get_pblocks rp_emu_top] -add {CLOCKREGION_X5Y4:CLOCKREGION_X6Y7}

# SLR2 and SLR3: no static hard blocks — give fully to RP
resize_pblock [get_pblocks rp_emu_top] -add {SLR2 SLR3}

set_property HD.RECONFIGURABLE true [get_cells u_emu_top]

# Constrain ICAPE3 to static region (SLR0, outside RP pblock)
set_property LOC ICAP_X0Y0 [get_cells u_icap_ctrl/u_icap]
