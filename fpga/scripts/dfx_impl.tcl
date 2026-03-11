# SPDX-License-Identifier: Apache-2.0
# Loom — Full DFX implementation (first-time build)
#
# Loads static_synth.dcp + rm_synth.dcp, defines pblock, runs full P&R.
# Outputs:
#   static_routed.dcp  — locked static checkpoint (reused for all future RM builds)
#   full.bit           — full bitstream (program to SPI flash for persistence)
#   rm_partial.bit     — partial bitstream for the initial RM (JTAG load)
#
# RM_NAME defaults to "rm".

set work_dir  $::env(WORK_DIR)
set board_dir $::env(BOARD_DIR)
set rm_name   [expr {[info exists ::env(RM_NAME)] ? $::env(RM_NAME) : "rm"}]

# Open static synthesis checkpoint
open_checkpoint $work_dir/results/static_synth.dcp

# Load the RM into the reconfigurable cell
read_checkpoint -cell u_emu_top $work_dir/results/${rm_name}_synth.dcp

# DFX pblock — defines RP area and marks cell as reconfigurable
read_xdc $board_dir/u250_dfx.xdc

# Implementation-only constraints
read_xdc $board_dir/u250_implementation.xdc

# ----------------------------------------------------------------
# Implementation
# ----------------------------------------------------------------
opt_design
place_design
phys_opt_design
route_design

# ----------------------------------------------------------------
# Reports
# ----------------------------------------------------------------
source [file join [file dirname [info script]] reports.tcl]
loom::reports_impl $work_dir dfx_impl

# ----------------------------------------------------------------
# Outputs
# ----------------------------------------------------------------
# Locked static checkpoint — reused for every future RM build
write_checkpoint -force $work_dir/results/static_routed.dcp

# Full bitstream — program to SPI flash for persistent shell
write_bitstream -force $work_dir/results/full.bit

# Partial bitstream for the initial RM
write_bitstream -force -cell u_emu_top $work_dir/results/${rm_name}_partial.bit

# static_routed.dcp (written above) is the full routed design — it serves
# as the pr_verify baseline for all future dfx_rm builds.
