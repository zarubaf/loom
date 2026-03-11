# SPDX-License-Identifier: Apache-2.0
# Loom — Load partial bitstream via JTAG (DFX RM swap)
#
# Programs ONLY the reconfigurable partition. The static shell is untouched.
# Usage: PARTIAL_BIT=path/to/rm_partial.bit vivado -source dfx_program_rm.tcl

set partial_bit $::env(PARTIAL_BIT)

open_hw_manager
connect_hw_server
open_hw_target

set device [lindex [get_hw_devices] 0]
current_hw_device $device
refresh_hw_device $device
puts "Loading partial bitstream: $partial_bit"
puts "Device: $device"

set_property PROGRAM.FILE    $partial_bit $device
set_property PROGRAM.PARTIAL 1           $device
program_hw_devices $device
puts "Partial reconfiguration complete."

close_hw_target
disconnect_hw_server
close_hw_manager
