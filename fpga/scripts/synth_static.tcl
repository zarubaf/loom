# SPDX-License-Identifier: Apache-2.0
# Loom — Static region synthesis for DFX
#
# Synthesizes loom_shell with loom_emu_top as a black box.
# Output: work-u250/results/static_synth.dcp

source $::env(BOARD_DIR)/settings.tcl

set work_dir  $::env(WORK_DIR)
set run_dir   $::env(LOOM_RUN_DIR)
set board_dir $::env(BOARD_DIR)
set ip_dir    $::env(IP_DIR)
set loom_src  $::env(LOOM_SRC)

create_project -in_memory -part $XILINX_PART
set_property board_part $XILINX_BOARD_PART [current_project]

read_ip $ip_dir/xlnx_xdma/xlnx_xdma.srcs/sources_1/ip/xlnx_xdma/xlnx_xdma.xci
read_ip $ip_dir/xlnx_clk_gen/xlnx_clk_gen.srcs/sources_1/ip/xlnx_clk_gen/xlnx_clk_gen.xci
read_ip $ip_dir/xlnx_cdc/xlnx_cdc.srcs/sources_1/ip/xlnx_cdc/xlnx_cdc.xci
read_ip $ip_dir/xlnx_decoupler/xlnx_decoupler.srcs/sources_1/ip/xlnx_decoupler/xlnx_decoupler.xci

read_verilog -sv \
  $loom_src/rtl/loom_axi4_err_slv.sv \
  $loom_src/rtl/loom_axil_demux.sv \
  $loom_src/rtl/loom_emu_ctrl.sv \
  $loom_src/rtl/loom_dpi_regfile.sv \
  $loom_src/rtl/loom_scan_ctrl.sv \
  $loom_src/rtl/loom_axil_firewall.sv \
  $loom_src/rtl/loom_icap_ctrl.sv \
  $loom_src/rtl/loom_shell.sv

# loom_emu_top black-box stub
read_verilog -sv $loom_src/rtl/loom_emu_top_bb.sv

read_xdc $board_dir/u250_pins.xdc
read_xdc $board_dir/u250_timing.xdc

synth_design -top loom_shell -flatten_hierarchy rebuilt -verilog_define "XILINX=1"

file mkdir $run_dir/results $run_dir/reports
write_checkpoint      -force $run_dir/results/static_synth.dcp
report_utilization    -file  $run_dir/reports/static_synth_utilization.rpt
report_timing_summary -warn_on_violation \
                      -file  $run_dir/reports/static_synth_timing.rpt
