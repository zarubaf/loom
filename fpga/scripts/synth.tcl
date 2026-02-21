# SPDX-License-Identifier: Apache-2.0
# Loom — Vivado synthesis script

# Board settings
source $::env(BOARD_DIR)/settings.tcl

set work_dir  $::env(WORK_DIR)
set board_dir $::env(BOARD_DIR)
set ip_dir    $::env(IP_DIR)

# Create in-memory project
create_project -in_memory -part $XILINX_PART
set_property board_part $XILINX_BOARD_PART [current_project]

# ----------------------------------------------------------------
# Read IPs
# ----------------------------------------------------------------
read_ip $ip_dir/xlnx_xdma/xlnx_xdma.srcs/sources_1/ip/xlnx_xdma/xlnx_xdma.xci
read_ip $ip_dir/xlnx_clk_gen/xlnx_clk_gen.srcs/sources_1/ip/xlnx_clk_gen/xlnx_clk_gen.xci
read_ip $ip_dir/xlnx_cdc/xlnx_cdc.srcs/sources_1/ip/xlnx_cdc/xlnx_cdc.xci
read_ip $ip_dir/xlnx_decoupler/xlnx_decoupler.srcs/sources_1/ip/xlnx_decoupler/xlnx_decoupler.xci

# ----------------------------------------------------------------
# Read RTL
# ----------------------------------------------------------------
# Infrastructure RTL
set loom_src $::env(LOOM_SRC)
read_verilog -sv \
  $loom_src/rtl/loom_axi4_err_slv.sv \
  $loom_src/rtl/loom_axil_demux.sv \
  $loom_src/rtl/loom_emu_ctrl.sv \
  $loom_src/rtl/loom_dpi_regfile.sv \
  $loom_src/rtl/loom_scan_ctrl.sv \
  $loom_src/rtl/loom_shell.sv

# Transformed DUT (generated Verilog)
read_verilog $::env(TRANSFORMED_V)

# ----------------------------------------------------------------
# Read constraints
# ----------------------------------------------------------------
read_xdc $board_dir/u250_pins.xdc
read_xdc $board_dir/u250_timing.xdc

# ----------------------------------------------------------------
# Synthesize
# ----------------------------------------------------------------
synth_design -top loom_shell -flatten_hierarchy rebuilt

# ----------------------------------------------------------------
# Write checkpoint
# ----------------------------------------------------------------
file mkdir $work_dir/results
write_checkpoint -force $work_dir/results/synth.dcp
report_utilization -file $work_dir/results/synth_utilization.rpt
report_timing_summary -file $work_dir/results/synth_timing.rpt
