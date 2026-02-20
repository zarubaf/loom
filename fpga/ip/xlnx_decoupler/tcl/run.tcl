# SPDX-License-Identifier: Apache-2.0
# Loom — DFX Decoupler IP
#
# Configured with 2 interfaces:
#   intf0: AXI-Lite (20-bit addr, 32-bit data) — register access path
#   intf1: AXI4 full (64-bit addr, 128-bit data, 4-bit ID) — DMA path

set part $::env(XILINX_PART)
set board $::env(XILINX_BOARD_PART)

set ipName xlnx_decoupler

create_project $ipName . -force -part $part
set_property board_part $board [current_project]

create_ip -name dfx_decoupler -vendor xilinx.com -library ip -version 1.0 -module_name $ipName

set_property -dict [list \
  CONFIG.ALL_PARAMS {HAS_AXI_LITE 1 HAS_SIGNAL_CONTROL 1 HAS_SIGNAL_STATUS 1 INTF {intf0 {ID 0 VLNV xilinx.com:interface:aximm_rtl:1.0 MODE slave PROTOCOL AXI4LITE SIGNALS {ARVALID {PRESENT 1} ARREADY {PRESENT 1} AWVALID {PRESENT 1} AWREADY {PRESENT 1} BVALID {PRESENT 1} BREADY {PRESENT 1} RVALID {PRESENT 1} RREADY {PRESENT 1} WVALID {PRESENT 1} WREADY {PRESENT 1} AWADDR {PRESENT 1 WIDTH 20} AWPROT {PRESENT 1 WIDTH 3} WDATA {PRESENT 1 WIDTH 32} WSTRB {PRESENT 1 WIDTH 4} BRESP {PRESENT 1 WIDTH 2} ARADDR {PRESENT 1 WIDTH 20} ARPROT {PRESENT 1 WIDTH 3} RDATA {PRESENT 1 WIDTH 32} RRESP {PRESENT 1 WIDTH 2}}} intf1 {ID 1 VLNV xilinx.com:interface:aximm_rtl:1.0 MODE slave PROTOCOL AXI4 SIGNALS {ARVALID {PRESENT 1} ARREADY {PRESENT 1} AWVALID {PRESENT 1} AWREADY {PRESENT 1} BVALID {PRESENT 1} BREADY {PRESENT 1} RVALID {PRESENT 1} RREADY {PRESENT 1} WVALID {PRESENT 1} WREADY {PRESENT 1} WLAST {PRESENT 1} RLAST {PRESENT 1} AWID {PRESENT 1 WIDTH 4} AWADDR {PRESENT 1 WIDTH 64} AWLEN {PRESENT 1 WIDTH 8} AWSIZE {PRESENT 1 WIDTH 3} AWBURST {PRESENT 1 WIDTH 2} AWLOCK {PRESENT 1 WIDTH 1} AWCACHE {PRESENT 1 WIDTH 4} AWPROT {PRESENT 1 WIDTH 3} WDATA {PRESENT 1 WIDTH 128} WSTRB {PRESENT 1 WIDTH 16} BID {PRESENT 1 WIDTH 4} BRESP {PRESENT 1 WIDTH 2} ARID {PRESENT 1 WIDTH 4} ARADDR {PRESENT 1 WIDTH 64} ARLEN {PRESENT 1 WIDTH 8} ARSIZE {PRESENT 1 WIDTH 3} ARBURST {PRESENT 1 WIDTH 2} ARLOCK {PRESENT 1 WIDTH 1} ARCACHE {PRESENT 1 WIDTH 4} ARPROT {PRESENT 1 WIDTH 3} RID {PRESENT 1 WIDTH 4} RDATA {PRESENT 1 WIDTH 128} RRESP {PRESENT 1 WIDTH 2}}}}} \
] [get_ips $ipName]

generate_target {instantiation_template} [get_files ./$ipName.srcs/sources_1/ip/$ipName/$ipName.xci]
generate_target all [get_files ./$ipName.srcs/sources_1/ip/$ipName/$ipName.xci]

create_ip_run [get_files -of_objects [get_fileset sources_1] ./$ipName.srcs/sources_1/ip/$ipName/$ipName.xci]
launch_run -jobs 8 ${ipName}_synth_1
wait_on_run ${ipName}_synth_1
