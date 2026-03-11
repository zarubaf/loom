# SPDX-License-Identifier: Apache-2.0
# Loom — Alveo U250 implementation constraints

set_property BITSTREAM.GENERAL.COMPRESS        TRUE [current_design]
set_property BITSTREAM.CONFIG.SPI_BUSWIDTH     4    [current_design]
set_property BITSTREAM.CONFIG.CONFIGRATE       85.0 [current_design]
set_property CONFIG_VOLTAGE                    1.8  [current_design]
set_property CFGBVS                            GND  [current_design]
