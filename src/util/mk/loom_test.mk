# SPDX-License-Identifier: Apache-2.0
# loom_test.mk — Complete test recipe for Loom e2e tests
#
# Set variables before including this file:
#
#   TOP       := my_dut              # Top module name (required)
#   DUT_SRC   := my_dut.sv           # DUT source file(s) (required)
#   DPI_SRCS  := dpi_impl.c util.c   # User DPI C/C++ sources (optional)
#   DPI_HDRS  := util.h              # DPI header dependencies (optional)
#   DPI_CFLAGS   :=                  # Extra C compile flags (optional)
#   DPI_CXXFLAGS :=                  # Extra C++ compile flags (optional)
#   LOOMC_FLAGS  :=                  # Extra loomc flags, e.g. -clk/-rst (optional)
#   BUILD     := build               # Build directory (optional, default: build)
#
#   include path/to/loom_test.mk
#
# Provides targets: all, test, interactive, clean

# ---------- Derive LOOM_ROOT from this file's location ----------
_LOOM_TEST_MK_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
LOOM_ROOT ?= $(realpath $(_LOOM_TEST_MK_DIR)/../../..)
export LOOM_HOME := $(LOOM_ROOT)

include $(_LOOM_TEST_MK_DIR)/loom.mk
include $(_LOOM_TEST_MK_DIR)/loom_sim.mk

# ---------- Defaults ----------
BUILD ?= build
CC    ?= cc
CXX   ?= c++

# ---------- Loom include path (svdpi.h, etc.) ----------
LOOM_INCLUDE := $(LOOM_ROOT)/src/include

# ---------- Validation ----------
ifndef TOP
  $(error TOP is not set — specify the top module name)
endif
ifndef DUT_SRC
  $(error DUT_SRC is not set — specify the DUT source file(s))
endif

# ---------- DPI object compilation ----------
_DPI_C_SRCS   := $(filter %.c,$(DPI_SRCS))
_DPI_CXX_SRCS := $(filter %.cpp,$(DPI_SRCS))
_DPI_OBJS     := $(patsubst %.c,$(BUILD)/%.o,$(_DPI_C_SRCS)) \
                 $(patsubst %.cpp,$(BUILD)/%.o,$(_DPI_CXX_SRCS))
_DPI_LINK     := $(if $(_DPI_CXX_SRCS),$(CXX),$(CC))

# loomx flags: only pass -sv_lib when there are DPI sources
_LOOMX_DPI := $(if $(DPI_SRCS),-sv_lib $(BUILD)/dpi)

# ---------- Phony targets ----------
.PHONY: all test interactive clean

all: test

# ---------- Step 1: Transform DUT with loomc ----------
$(BUILD)/transformed.v: $(DUT_SRC)
	$(LOOMC) -top $(TOP) -work $(BUILD) $(LOOMC_FLAGS) $^

# ---------- Step 2: Build Verilator sim (pattern rule from loom_sim.mk) ----------
$(BUILD)/sim/obj_dir/Vloom_sim_top: $(BUILD)/transformed.v

# ---------- Step 3: Compile user DPI sources into libdpi.so ----------
ifdef DPI_SRCS

$(BUILD)/%.o: %.c $(DPI_HDRS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) -fPIC -g -O0 -I$(LOOM_INCLUDE) $(DPI_CFLAGS) -c -o $@ $<

$(BUILD)/%.o: %.cpp $(DPI_HDRS) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CXX) -fPIC -g -O0 -std=c++17 -I$(LOOM_INCLUDE) $(DPI_CXXFLAGS) -c -o $@ $<

_DPI_LDFLAGS :=
ifeq ($(shell uname -s),Darwin)
  _DPI_LDFLAGS += -undefined dynamic_lookup
endif

$(BUILD)/libdpi.so: $(_DPI_OBJS)
	$(_DPI_LINK) -shared $(_DPI_LDFLAGS) -o $@ $^

endif

$(BUILD):
	@mkdir -p $@

# ---------- Step 4: Test (script mode) ----------
_TEST_DEPS := $(BUILD)/sim/obj_dir/Vloom_sim_top $(if $(DPI_SRCS),$(BUILD)/libdpi.so)

test: $(_TEST_DEPS)
	@echo "run" > $(BUILD)/test_script.txt
	@echo "exit" >> $(BUILD)/test_script.txt
	$(LOOMX) -work $(BUILD) $(_LOOMX_DPI) -sim Vloom_sim_top -f $(BUILD)/test_script.txt

# ---------- Step 5: Interactive ----------
interactive: $(_TEST_DEPS)
	$(LOOMX) -work $(BUILD) $(_LOOMX_DPI) -sim Vloom_sim_top

# ---------- Clean ----------
clean:
	rm -rf $(BUILD) *.fst *.fst.hier
