# SPDX-License-Identifier: Apache-2.0
# Loom — Vivado implementation script (place & route)

source [file join [file dirname [info script]] reports.tcl]

set work_dir  $::env(WORK_DIR)
set run_dir   $::env(LOOM_RUN_DIR)
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
# Reports and outputs
# ----------------------------------------------------------------
loom::reports_impl $run_dir impl

file mkdir $run_dir/results
write_checkpoint -force $run_dir/results/impl.dcp
write_bitstream  -force $run_dir/results/loom_shell.bit
