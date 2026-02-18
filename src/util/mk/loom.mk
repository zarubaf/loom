# SPDX-License-Identifier: Apache-2.0
# loom.mk — Loom Makefile variables
#
# Include this from your test/project Makefile:
#   LOOM_HOME ?= /path/to/loom
#   include $(LOOM_HOME)/src/util/mk/loom.mk

LOOM_HOME ?= $(error LOOM_HOME is not set)

LOOMC := $(LOOM_HOME)/build/src/tools/loomc
LOOMX := $(LOOM_HOME)/build/src/tools/loomx

CC ?= cc

# Flags for compiling user DPI into a shared object
LOOM_DPI_CFLAGS := -shared -fPIC -g -O0
