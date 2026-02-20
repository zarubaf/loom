# SPDX-License-Identifier: Apache-2.0
# Loom — Vivado JTAG programming script

set work_dir $::env(WORK_DIR)
set bit_file $work_dir/results/loom_shell.bit

open_hw_manager
connect_hw_server
open_hw_target

# Find the first available device
set devices [get_hw_devices]
puts "Available devices: $devices"
if {[llength $devices] == 0} {
    puts "ERROR: No hw_devices found on JTAG chain"
    exit 1
}
set device [lindex $devices 0]
current_hw_device $device
puts "Programming device: $device"

set_property PROGRAM.FILE $bit_file $device
program_hw_devices $device

close_hw_target
disconnect_hw_server
close_hw_manager
