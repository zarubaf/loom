# SPDX-License-Identifier: Apache-2.0
# Loom — Reconfigurable Module implementation (fast path)
#
# Uses the locked static_routed.dcp to implement a new RM.
# Only the RP region is placed and routed — much faster than a full build.
#
# Input:  RM_NAME — name tag (matches rm_synth.dcp produced by synth_rm.tcl)
# Output: work-u250/results/${RM_NAME}_partial.bit

set work_dir  $::env(WORK_DIR)
set board_dir $::env(BOARD_DIR)
set rm_name   [expr {[info exists ::env(RM_NAME)] ? $::env(RM_NAME) : "rm"}]

# Open locked static checkpoint
open_checkpoint $work_dir/results/static_routed.dcp

# Load new RM
read_checkpoint -cell u_emu_top $work_dir/results/${rm_name}_synth.dcp

# Implementation-only constraints
read_xdc $board_dir/u250_implementation.xdc

# ----------------------------------------------------------------
# Partial implementation (RP only)
# ----------------------------------------------------------------
opt_design
place_design -partial
phys_opt_design
route_design -partial

# ----------------------------------------------------------------
# Reports
# ----------------------------------------------------------------
report_utilization  -file $work_dir/results/${rm_name}_utilization.rpt
report_timing_summary -file $work_dir/results/${rm_name}_timing.rpt

# ----------------------------------------------------------------
# Partial bitstream
# ----------------------------------------------------------------
write_checkpoint   -force $work_dir/results/${rm_name}_routed.dcp
write_bitstream    -force -cell u_emu_top $work_dir/results/${rm_name}_partial.bit

# Verify new RM's static region matches the golden static checkpoint
pr_verify \
  -initial    $work_dir/results/static_routed.dcp \
  -additional $work_dir/results/${rm_name}_routed.dcp \
  -file       $work_dir/results/${rm_name}_pr_verify.rpt

puts "DFX RM done: $work_dir/results/${rm_name}_partial.bit"
