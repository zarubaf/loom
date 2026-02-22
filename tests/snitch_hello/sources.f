// Snitch Hello World — source filelist for yosys-slang / Verilator
// Include paths
+incdir+../../third_party/snitch/common_cells/include
+incdir+../../third_party/snitch/snitch_cluster/hw/snitch/include
+incdir+../../third_party/snitch/snitch_cluster/hw/reqrsp_interface/include

// Packages (dependency order)
../../third_party/snitch/riscv-dbg/src/dm_pkg.sv
../../third_party/snitch/axi/src/axi_pkg.sv
../../third_party/snitch/cvfpu/src/fpnew_pkg.sv
../../third_party/snitch/common_cells/src/cf_math_pkg.sv
../../third_party/snitch/snitch_cluster/hw/reqrsp_interface/src/reqrsp_pkg.sv
../../third_party/snitch/snitch_cluster/hw/snitch/src/snitch_pma_pkg.sv
../../third_party/snitch/snitch_cluster/hw/snitch/src/riscv_instr.sv
../../third_party/snitch/snitch_cluster/hw/snitch/src/snitch_pkg.sv

// Common cells (modules used by snitch core + LSU)
../../third_party/snitch/common_cells/src/fifo_v3.sv
../../third_party/snitch/common_cells/src/stream_fifo.sv
../../third_party/snitch/common_cells/src/id_queue.sv
../../third_party/snitch/common_cells/src/lzc.sv
../../third_party/snitch/common_cells/src/delta_counter.sv
../../third_party/snitch/common_cells/src/popcount.sv

// Snitch core
../../third_party/snitch/snitch_cluster/hw/snitch/src/snitch_regfile_ff.sv
../../third_party/snitch/snitch_cluster/hw/snitch/src/snitch_l0_tlb.sv
../../third_party/snitch/snitch_cluster/hw/snitch/src/snitch_lsu.sv
../../third_party/snitch/snitch_cluster/hw/snitch/src/snitch.sv

// Wrapper (DUT)
snitch_hello_top.sv
