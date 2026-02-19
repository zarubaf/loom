# SPDX-License-Identifier: Apache-2.0
# Loom — AXI-Lite clock domain crossing IP

set part $::env(XILINX_PART)
set board $::env(XILINX_BOARD_PART)

set ipName xlnx_axi_clock_converter

create_project $ipName . -force -part $part
set_property board_part $board [current_project]

create_ip -name axi_clock_converter -vendor xilinx.com -library ip -version 2.1 -module_name $ipName

set_property -dict [list \
  CONFIG.PROTOCOL {AXI4LITE} \
  CONFIG.ADDR_WIDTH {20} \
  CONFIG.DATA_WIDTH {32} \
] [get_ips $ipName]

generate_target {instantiation_template} [get_files ./$ipName.srcs/sources_1/ip/$ipName/$ipName.xci]
generate_target all [get_files ./$ipName.srcs/sources_1/ip/$ipName/$ipName.xci]

create_ip_run [get_files -of_objects [get_fileset sources_1] ./$ipName.srcs/sources_1/ip/$ipName/$ipName.xci]
launch_run -jobs 8 ${ipName}_synth_1
wait_on_run ${ipName}_synth_1
