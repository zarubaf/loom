# SPDX-License-Identifier: Apache-2.0
# Loom — Reconfigurable Module (RM) synthesis for DFX
#
# Synthesizes loom_emu_top + transformed DUT out-of-context.
# Input:  TRANSFORMED_V — path to transformed.v
#         RM_NAME       — name tag for output (default: "rm")
# Output: work-u250/results/${RM_NAME}_synth.dcp

source [file join [file dirname [info script]] reports.tcl]
source $::env(BOARD_DIR)/settings.tcl

set work_dir    $::env(WORK_DIR)
set rm_name     [expr {[info exists ::env(RM_NAME)] ? $::env(RM_NAME) : "rm"}]

create_project -in_memory -part $XILINX_PART

loom::read_rm_rtl
read_verilog $::env(TRANSFORMED_V)

synth_design \
    -top loom_emu_top \
    -flatten_hierarchy rebuilt \
    -mode out_of_context \
    -verilog_define "XILINX=1"

file mkdir $work_dir/results
write_checkpoint -force $work_dir/results/${rm_name}_synth.dcp
loom::reports_synth $work_dir ${rm_name}_synth
