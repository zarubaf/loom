# SPDX-License-Identifier: Apache-2.0
# Loom — Clock generator IP (300 MHz input, 50 MHz default output, DRP reconfigurable)

set part $::env(XILINX_PART)
set board $::env(XILINX_BOARD_PART)

set ipName xlnx_clk_gen

create_project $ipName . -force -part $part
set_property board_part $board [current_project]

create_ip -name clk_wiz -vendor xilinx.com -library ip -module_name $ipName

set_property -dict [list \
  CONFIG.USE_DYN_RECONFIG {true} \
  CONFIG.PRIM_SOURCE {Global_buffer} \
  CONFIG.PRIM_IN_FREQ {300.000} \
  CONFIG.CLKOUT1_REQUESTED_OUT_FREQ {50.000} \
  CONFIG.NUM_OUT_CLKS {1} \
] [get_ips $ipName]

generate_target {instantiation_template} [get_files ./$ipName.srcs/sources_1/ip/$ipName/$ipName.xci]
generate_target all [get_files ./$ipName.srcs/sources_1/ip/$ipName/$ipName.xci]

create_ip_run [get_files -of_objects [get_fileset sources_1] ./$ipName.srcs/sources_1/ip/$ipName/$ipName.xci]
launch_run -jobs 8 ${ipName}_synth_1
wait_on_run ${ipName}_synth_1
