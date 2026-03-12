# SPDX-License-Identifier: Apache-2.0
# Loom — Reconfigurable Module implementation (fast path)
#
# Uses the locked static_routed.dcp to implement a new RM.
# Only the RP region is placed and routed — much faster than a full build.
#
# Input:  RM_NAME — name tag (matches rm_synth.dcp produced by synth_rm.tcl)
# Output: work-u250/results/${RM_NAME}_partial.bit

source [file join [file dirname [info script]] reports.tcl]

set work_dir  $::env(WORK_DIR)
set run_dir   $::env(LOOM_RUN_DIR)
set board_dir $::env(BOARD_DIR)
set rm_name   [expr {[info exists ::env(RM_NAME)] ? $::env(RM_NAME) : "rm"}]

# Open locked static checkpoint (contains static + initial RM fully routed)
open_checkpoint $work_dir/results/static_routed.dcp

# Remove initial RM, exposing u_emu_top as a black box
update_design -cell u_emu_top -black_box

# Lock static region — RM is gone so only static routing gets locked
lock_design -level routing

# Load new RM into the black box
read_checkpoint -cell u_emu_top $work_dir/results/${rm_name}_synth.dcp

# Implementation-only constraints
read_xdc $board_dir/u250_implementation.xdc

# ----------------------------------------------------------------
# Partial implementation (RP only)
# ----------------------------------------------------------------
opt_design
loom::reports_stage $run_dir $rm_name opt

place_design
loom::reports_stage $run_dir $rm_name place

phys_opt_design
loom::reports_stage $run_dir $rm_name phys_opt

route_design

# ----------------------------------------------------------------
# Reports
# ----------------------------------------------------------------
loom::reports_impl $run_dir $rm_name

# ----------------------------------------------------------------
# Partial bitstream
# ----------------------------------------------------------------
file mkdir $run_dir/results

write_bitstream -force -cell u_emu_top $run_dir/results/${rm_name}_partial.bit

# Write design hash sidecar so loomx can verify the bitstream was accepted.
set manifest [file join [file dirname $::env(TRANSFORMED_V)] loom_manifest.toml]
if {[file exists $manifest]} {
    set fh [open $manifest r]
    set content [read $fh]
    close $fh
    if {[regexp {hash\s*=\s*"([0-9a-f]+)"} $content _ design_hash]} {
        set fh [open $run_dir/results/${rm_name}_partial.hash w]
        puts -nonewline $fh $design_hash
        close $fh
    }
}

# Verify static region matches golden checkpoint.
write_checkpoint -force $run_dir/results/${rm_name}_routed.dcp
pr_verify \
  -initial    $work_dir/results/static_routed.dcp \
  -additional $run_dir/results/${rm_name}_routed.dcp \
  -file       $run_dir/reports/${rm_name}_pr_verify.rpt
puts "pr_verify passed."

puts "DFX RM done: $work_dir/results/${rm_name}_partial.bit"
