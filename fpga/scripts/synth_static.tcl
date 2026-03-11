# SPDX-License-Identifier: Apache-2.0
# Loom — Static region synthesis for DFX
#
# Synthesizes loom_shell with loom_emu_top as a black box.
# Output: work-u250/results/static_synth.dcp

source [file join [file dirname [info script]] reports.tcl]
source $::env(BOARD_DIR)/settings.tcl

set work_dir  $::env(WORK_DIR)
set board_dir $::env(BOARD_DIR)

create_project -in_memory -part $XILINX_PART
set_property board_part $XILINX_BOARD_PART [current_project]

loom::read_ips
loom::read_shell_rtl
# loom_emu_top black-box stub — (* black_box *) attribute makes Vivado treat
# u_emu_top as the RP boundary automatically.
read_verilog -sv $::env(LOOM_SRC)/rtl/loom_emu_top_bb.sv
read_xdc $board_dir/u250_pins.xdc
read_xdc $board_dir/u250_timing.xdc

synth_design -top loom_shell -flatten_hierarchy rebuilt -verilog_define "XILINX=1"

file mkdir $work_dir/results
write_checkpoint -force $work_dir/results/static_synth.dcp
loom::reports_synth $work_dir static_synth
