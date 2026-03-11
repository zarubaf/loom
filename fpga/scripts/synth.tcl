# SPDX-License-Identifier: Apache-2.0
# Loom — Vivado synthesis script (flat build, DUT included)

source [file join [file dirname [info script]] reports.tcl]
source $::env(BOARD_DIR)/settings.tcl

set work_dir  $::env(WORK_DIR)
set board_dir $::env(BOARD_DIR)

create_project -in_memory -part $XILINX_PART
set_property board_part $XILINX_BOARD_PART [current_project]

loom::read_ips
loom::read_shell_rtl
read_verilog $::env(TRANSFORMED_V)
read_xdc $board_dir/u250_pins.xdc
read_xdc $board_dir/u250_timing.xdc

synth_design -top loom_shell -flatten_hierarchy rebuilt -verilog_define "XILINX=1"

file mkdir $work_dir/results
write_checkpoint -force $work_dir/results/synth.dcp
loom::reports_synth $work_dir synth
