# SPDX-License-Identifier: Apache-2.0
# Loom Yosys passes
# Each pass is built as a loadable plugin (.so)

# scan_insert pass - scan chain insertion for FFs
add_subdirectory(scan_insert)

# loom_instrument pass - DPI bridge + flop enable
add_subdirectory(loom_instrument)

# emu_top pass - top-level wrapper generation
add_subdirectory(emu_top)

# mem_shadow pass - shadow port insertion for memories
add_subdirectory(mem_shadow)

# reset_extract pass - extract reset values, strip async resets
add_subdirectory(reset_extract)
