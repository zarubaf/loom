# SPDX-License-Identifier: Apache-2.0
# Loom Yosys passes
# Each pass is built as a loadable plugin (.so)

# scan_insert pass - scan chain insertion
add_subdirectory(scan_insert)

# dpi_bridge pass - DPI-to-hardware bridge
add_subdirectory(dpi_bridge)

# emu_top pass - top-level wrapper generation with clock gating
add_subdirectory(emu_top)
