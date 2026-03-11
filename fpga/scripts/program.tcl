# SPDX-License-Identifier: Apache-2.0
# Loom — Unified FPGA programming script
#
# MODE=jtag         Full JTAG load (volatile, flash unchanged)
# MODE=jtag-partial Partial JTAG load (RP only, static shell survives)
# MODE=flash        Write full.bit to SPI flash (persistent across power cycles)
#
# BIT_FILE          Path to the bitstream file

set mode     $::env(MODE)
set bit_file $::env(BIT_FILE)

if {![file exists $bit_file]} {
    puts "ERROR: BIT_FILE not found: $bit_file"
    exit 1
}

if {$mode eq "flash"} {
    set mcs_file [file rootname $bit_file].mcs
    puts "Generating MCS: $mcs_file"
    write_cfgmem \
        -format mcs \
        -interface SPIx4 \
        -size 128 \
        -loadbit "up 0x01002000 $bit_file" \
        -force \
        $mcs_file
}

open_hw_manager
connect_hw_server -allow_non_jtag
open_hw_target

set device [lindex [get_hw_devices xcu250_0] 0]
current_hw_device $device
refresh_hw_device -update_hw_probes false $device
puts "Device: $device  Mode: $mode  File: $bit_file"

if {$mode eq "jtag"} {
    set_property PROGRAM.FILE $bit_file $device
    program_hw_devices $device
    puts "JTAG load complete (volatile)."

} elseif {$mode eq "jtag-partial"} {
    set_property PROGRAM.FILE $bit_file $device
    program_hw_devices $device
    puts "Partial reconfiguration complete."

} elseif {$mode eq "flash"} {
    set mcs_file [file rootname $bit_file].mcs
    set part [lindex [get_cfgmem_parts {mt25qu01g-spi-x1_x2_x4}] 0]
    if {$part eq ""} {
        puts "ERROR: cfgmem part mt25qu01g-spi-x1_x2_x4 not found in this Vivado install."
        puts "Run: get_cfgmem_parts *mt25qu01g* to find the correct part name."
        exit 1
    }
    create_hw_cfgmem -hw_device $device $part
    set_property PROGRAM.BLANK_CHECK 0 [get_property PROGRAM.HW_CFGMEM $device]
    set_property PROGRAM.ERASE       1 [get_property PROGRAM.HW_CFGMEM $device]
    set_property PROGRAM.CFG_PROGRAM 1 [get_property PROGRAM.HW_CFGMEM $device]
    set_property PROGRAM.VERIFY      1 [get_property PROGRAM.HW_CFGMEM $device]
    set_property PROGRAM.CHECKSUM    0 [get_property PROGRAM.HW_CFGMEM $device]
    refresh_hw_device $device
    set_property PROGRAM.ADDRESS_RANGE          {use_file}       [get_property PROGRAM.HW_CFGMEM $device]
    set_property PROGRAM.FILES                  [list $mcs_file] [get_property PROGRAM.HW_CFGMEM $device]
    set_property PROGRAM.PRM_FILE               {}               [get_property PROGRAM.HW_CFGMEM $device]
    set_property PROGRAM.UNUSED_PIN_TERMINATION {pull-none}      [get_property PROGRAM.HW_CFGMEM $device]
    set_property PROGRAM.BLANK_CHECK            0                [get_property PROGRAM.HW_CFGMEM $device]
    set_property PROGRAM.ERASE                  1                [get_property PROGRAM.HW_CFGMEM $device]
    set_property PROGRAM.CFG_PROGRAM            1                [get_property PROGRAM.HW_CFGMEM $device]
    set_property PROGRAM.VERIFY                 1                [get_property PROGRAM.HW_CFGMEM $device]
    set_property PROGRAM.CHECKSUM               0                [get_property PROGRAM.HW_CFGMEM $device]
    # Load the Vivado-generated SPI programming core onto the FPGA before accessing flash
    puts "[clock format [clock seconds] -format {%H:%M:%S}] Loading SPI programming core..."
    create_hw_bitstream -hw_device $device [get_property PROGRAM.HW_CFGMEM_BITFILE $device]
    program_hw_devices $device
    refresh_hw_device $device
    puts "[clock format [clock seconds] -format {%H:%M:%S}] Programming flash (erase + program + verify — ~10 min)..."
    program_hw_cfgmem -verbose -hw_cfgmem [get_property PROGRAM.HW_CFGMEM $device]
    puts "[clock format [clock seconds] -format {%H:%M:%S}] Flash done. Rebooting FPGA from flash..."
    boot_hw_device $device
    puts "[clock format [clock seconds] -format {%H:%M:%S}] Flash programmed. FPGA reloading from flash..."

} else {
    puts "ERROR: unknown MODE '$mode'. Use: jtag / jtag-partial / flash"
    exit 1
}

close_hw_target
disconnect_hw_server
close_hw_manager
