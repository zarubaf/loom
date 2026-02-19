# SPDX-License-Identifier: Apache-2.0
# Multisim read/write example — ported from Veloce to Loom
TOP      := top
DUT_SRC  := top.sv multisim_server_pull.sv multisim_server_pull_then_push.sv
DPI_SRCS := multisim/multisim_server.cpp multisim/socket_server/server.cpp
DPI_HDRS := multisim/multisim_server.h multisim/multisim_common.h \
            multisim/socket_server/server.h
DPI_CXXFLAGS := -Imultisim
LOOMC_FLAGS  := -D MULTISIM_EMULATION

include ../../src/util/mk/loom_test.mk
