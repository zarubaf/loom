# SPDX-License-Identifier: Apache-2.0
# Loom — XDMA IP generation (PCIe X2 Gen3, AXI-Lite master 1 MB @ 125 MHz)

set part $::env(XILINX_PART)
set board $::env(XILINX_BOARD_PART)

set ipName xlnx_xdma

create_project $ipName . -force -part $part
set_property board_part $board [current_project]

create_ip -name xdma -vendor xilinx.com -library ip -version 4.1 -module_name $ipName

set_property -dict [list \
  CONFIG.PCIE_BOARD_INTERFACE {pci_express_x2} \
  CONFIG.SYS_RST_N_BOARD_INTERFACE {pcie_perstn} \
  CONFIG.axi_data_width {128_bit} \
  CONFIG.axilite_master_en {true} \
  CONFIG.axilite_master_scale {Megabytes} \
  CONFIG.axilite_master_size {1} \
  CONFIG.axil_master_64bit_en {true} \
  CONFIG.axisten_freq {125} \
  CONFIG.coreclk_freq {250} \
  CONFIG.en_gt_selection {true} \
  CONFIG.mode_selection {Advanced} \
  CONFIG.pciebar2axibar_axil_master {0x00000000} \
  CONFIG.pf0_device_id {9034} \
  CONFIG.pl_link_cap_max_link_speed {8.0_GT/s} \
  CONFIG.pl_link_cap_max_link_width {X2} \
  CONFIG.plltype {QPLL1} \
  CONFIG.xdma_axilite_slave {false} \
  CONFIG.xdma_rnum_chnl {1} \
  CONFIG.xdma_wnum_chnl {1} \
  CONFIG.xdma_rnum_rids {2} \
  CONFIG.xdma_wnum_rids {2} \
] [get_ips $ipName]

generate_target {instantiation_template} [get_files ./$ipName.srcs/sources_1/ip/$ipName/$ipName.xci]
generate_target all [get_files ./$ipName.srcs/sources_1/ip/$ipName/$ipName.xci]

create_ip_run [get_files -of_objects [get_fileset sources_1] ./$ipName.srcs/sources_1/ip/$ipName/$ipName.xci]
launch_run -jobs 8 ${ipName}_synth_1
wait_on_run ${ipName}_synth_1
