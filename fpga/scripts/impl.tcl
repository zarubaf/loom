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

source [file join [file dirname [info script]] reports.tcl]
loom::reports_impl $work_dir impl

# ----------------------------------------------------------------
# Write outputs
# ----------------------------------------------------------------
write_checkpoint -force $work_dir/results/impl.dcp
write_bitstream -force $work_dir/results/loom_shell.bit
