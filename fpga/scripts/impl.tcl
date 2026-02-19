# SPDX-License-Identifier: Apache-2.0
# Loom — Vivado implementation script (place & route)

set work_dir  $::env(WORK_DIR)
set board_dir $::env(BOARD_DIR)

# Open synthesis checkpoint
open_checkpoint $work_dir/results/synth.dcp

# Read implementation-only constraints
read_xdc $board_dir/u250_implementation.xdc

# ----------------------------------------------------------------
# Implementation flow
# ----------------------------------------------------------------
opt_design
place_design
phys_opt_design
route_design

# ----------------------------------------------------------------
# Reports
# ----------------------------------------------------------------
report_utilization -file $work_dir/results/impl_utilization.rpt
report_timing_summary -file $work_dir/results/impl_timing.rpt
report_drc -file $work_dir/results/impl_drc.rpt

# ----------------------------------------------------------------
# Write outputs
# ----------------------------------------------------------------
write_checkpoint -force $work_dir/results/impl.dcp
write_bitstream -force $work_dir/results/loom_fpga_top.bit
