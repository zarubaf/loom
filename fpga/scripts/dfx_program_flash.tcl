# SPDX-License-Identifier: Apache-2.0
# Loom — Program full bitstream to SPI flash (one-time shell persistence)
#
# After this, the FPGA loads the static shell + initial RM on every power-on.
# New DUTs are loaded as partial bitstreams via dfx_program_rm.tcl.
#
# Flash part: Micron MT25QU01G (1Gb, x4 SPI) — standard on Alveo U250.

set work_dir $::env(WORK_DIR)
set bit_file $work_dir/results/full.bit
set mcs_file $work_dir/results/shell.mcs

# Generate MCS from bitstream
write_cfgmem \
  -format mcs \
  -interface SPIx4 \
  -size 256 \
  -loadbit "up 0x00000000 $bit_file" \
  -force \
  $mcs_file

open_hw_manager
connect_hw_server
open_hw_target

set device [lindex [get_hw_devices] 0]
current_hw_device $device
refresh_hw_device $device
puts "Programming flash on device: $device"

# Create config memory for MT25QU01G (Alveo U250 SPI flash)
create_hw_cfgmem \
  -hw_device $device \
  [lindex [get_cfgmem_parts {mt25qu01g-spi-x1_x2_x4}] 0]

set cfgmem [get_hw_cfgmem -of_objects $device]
set_property PROGRAM.ADDRESS_RANGE  {use_file} $cfgmem
set_property PROGRAM.FILES          [list $mcs_file] $cfgmem
set_property PROGRAM.PRM_FILE       {} $cfgmem
set_property PROGRAM.UNUSED_PIN_TERMINATION {pull-none} $cfgmem
set_property PROGRAM.BLANK_CHECK    0 $cfgmem
set_property PROGRAM.ERASE          1 $cfgmem
set_property PROGRAM.CFG_PROGRAM    1 $cfgmem
set_property PROGRAM.VERIFY         1 $cfgmem

program_hw_cfgmem $cfgmem

# Trigger FPGA to reload from flash
boot_hw_device $device
puts "Flash programmed. FPGA reloading from flash..."

close_hw_target
disconnect_hw_server
close_hw_manager
