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
        -size 256 \
        -loadbit "up 0x00000000 $bit_file" \
        -force \
        $mcs_file
}

open_hw_manager
connect_hw_server
open_hw_target

set device [lindex [get_hw_devices] 0]
current_hw_device $device
refresh_hw_device $device
puts "Device: $device  Mode: $mode  File: $bit_file"

if {$mode eq "jtag"} {
    set_property PROGRAM.FILE $bit_file $device
    program_hw_devices $device
    puts "JTAG load complete (volatile)."

} elseif {$mode eq "jtag-partial"} {
    set_property PROGRAM.FILE    $bit_file $device
    set_property PROGRAM.PARTIAL 1         $device
    program_hw_devices $device
    puts "Partial reconfiguration complete."

} elseif {$mode eq "flash"} {
    set mcs_file [file rootname $bit_file].mcs
    create_hw_cfgmem \
        -hw_device $device \
        [lindex [get_cfgmem_parts {mt25qu01g-spi-x1_x2_x4}] 0]
    set cfgmem [get_hw_cfgmem -of_objects $device]
    set_property PROGRAM.ADDRESS_RANGE          {use_file} $cfgmem
    set_property PROGRAM.FILES                  [list $mcs_file] $cfgmem
    set_property PROGRAM.PRM_FILE               {} $cfgmem
    set_property PROGRAM.UNUSED_PIN_TERMINATION {pull-none} $cfgmem
    set_property PROGRAM.BLANK_CHECK            0 $cfgmem
    set_property PROGRAM.ERASE                  1 $cfgmem
    set_property PROGRAM.CFG_PROGRAM            1 $cfgmem
    set_property PROGRAM.VERIFY                 1 $cfgmem
    program_hw_cfgmem $cfgmem
    boot_hw_device $device
    puts "Flash programmed. FPGA reloading from flash..."

} else {
    puts "ERROR: unknown MODE '$mode'. Use: jtag / jtag-partial / flash"
    exit 1
}

close_hw_target
disconnect_hw_server
close_hw_manager
