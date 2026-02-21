// SPDX-License-Identifier: Apache-2.0
/*
 * mem_shadow - Yosys pass for BRAM shadow port insertion
 *
 * This pass adds shadow access ports to $mem_v2 cells, enabling random-access
 * read/write of memory contents via a unified interface. This is much faster
 * than serial scan for large memories.
 *
 * The pass:
 *   1. Finds all $mem_v2 cells (run after memory_collect, before memory_bram)
 *   2. Adds a shadow read/write port to each memory (internal wires)
 *   3. Creates unified shadow interface at module ports (single address/data bus)
 *   4. Generates loom_mem_ctrl module with address decode logic
 *   5. Instantiates controller and wires to memory shadow ports
 *   6. Extracts initial memory content from inline inits and $readmemh/$readmemb
 *   7. Emits MemMap protobuf for host driver
 *
 * Usage:
 *   read_slang design.sv
 *   proc; opt
 *   memory_collect; memory_dff
 *   mem_shadow -clk clk_i -map mem_map.pb
 */

#include "kernel/yosys.h"
#include "kernel/sigtools.h"
#include "kernel/mem.h"
#include "loom_snapshot.pb.h"
#include <fstream>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

// Memory info collected during pass
struct MemInfo {
    Mem *mem;
    std::string memid;
    int width;
    int depth;
    int abits;
    uint32_t base_addr;
    int shadow_rd_port_idx;
    int shadow_wr_port_idx;
    // Internal wires (NOT ports)
    RTLIL::Wire *shadow_addr;
    RTLIL::Wire *shadow_rdata;
    RTLIL::Wire *shadow_wdata;
    RTLIL::Wire *shadow_wen;
    RTLIL::Wire *shadow_ren;
    // Initial content
    std::vector<uint8_t> initial_content;
    bool has_initial_content = false;
    std::string init_file;
    bool init_file_hex = true;
};

struct MemShadowPass : public Pass {
    MemShadowPass() : Pass("mem_shadow", "Insert shadow access ports on memories") {}

    void help() override {
        log("\n");
        log("    mem_shadow [options] [selection]\n");
        log("\n");
        log("Add shadow read/write ports to all $mem_v2 cells for debug access.\n");
        log("\n");
        log("Run this pass after 'memory_collect' and 'memory_dff', but before\n");
        log("'memory_bram'. The shadow ports allow random-access read/write of\n");
        log("memory contents via a unified interface.\n");
        log("\n");
        log("    -map <file.pb>\n");
        log("        Write memory map to protobuf file for host driver.\n");
        log("\n");
        log("    -ctrl <module_name>\n");
        log("        Name for generated controller module (default: loom_mem_ctrl)\n");
        log("\n");
        log("    -clk <name>\n");
        log("        DUT clock signal name (default: clk_i)\n");
        log("\n");
    }

    static int ceil_log2(int n) {
        int bits = 0;
        n--;
        while (n > 0) { n >>= 1; bits++; }
        return bits < 1 ? 1 : bits;
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override {
        log_header(design, "Executing MEM_SHADOW pass.\n");

        std::string map_file;
        std::string ctrl_name = "loom_mem_ctrl";
        std::string clk_name = "clk_i";

        size_t argidx;
        for (argidx = 1; argidx < args.size(); argidx++) {
            if (args[argidx] == "-map" && argidx + 1 < args.size()) {
                map_file = args[++argidx];
                continue;
            }
            if (args[argidx] == "-ctrl" && argidx + 1 < args.size()) {
                ctrl_name = args[++argidx];
                continue;
            }
            if (args[argidx] == "-clk" && argidx + 1 < args.size()) {
                clk_name = args[++argidx];
                continue;
            }
            break;
        }
        extra_args(args, argidx, design);

        // Process each selected module
        for (auto module : design->selected_modules()) {
            if (module->get_bool_attribute(ID::blackbox))
                continue;

            log("Processing module %s\n", log_id(module));

            std::vector<MemInfo> memories;
            run_mem_shadow(module, memories, clk_name);

            if (!memories.empty()) {
                // Note: extract_init_content is called inside run_mem_shadow
                // because it needs access to the Mem objects before they go out of scope.

                // Generate controller module with actual logic
                generate_mem_ctrl(design, ctrl_name, memories);

                // Instantiate controller in design module
                instantiate_mem_ctrl(module, ctrl_name, memories, clk_name);

                // Set module attributes for emu_top auto-detection
                uint32_t total_addr_space = 0;
                int max_width = 0;
                for (const auto &mi : memories) {
                    int words_per_entry = (mi.width + 31) / 32;
                    if (words_per_entry < 1) words_per_entry = 1;
                    total_addr_space = mi.base_addr + mi.depth * words_per_entry * 4;
                    if (mi.width > max_width) max_width = mi.width;
                }
                int global_addr_bits = ceil_log2(total_addr_space);
                if (global_addr_bits < 2) global_addr_bits = 2;

                module->set_string_attribute(ID(loom_n_memories),
                    std::to_string(memories.size()));
                module->set_string_attribute(ID(loom_shadow_addr_bits),
                    std::to_string(global_addr_bits));
                module->set_string_attribute(ID(loom_shadow_data_bits),
                    std::to_string(max_width));
                module->set_string_attribute(ID(loom_shadow_total_bytes),
                    std::to_string(total_addr_space));

                // Write protobuf memory map
                if (!map_file.empty()) {
                    write_mem_map(map_file, memories);
                }
            }
        }
    }

    void run_mem_shadow(RTLIL::Module *module, std::vector<MemInfo> &memories,
                        const std::string &clk_name) {
        auto mems = Mem::get_all_memories(module);

        if (mems.empty()) {
            log("  No memories found.\n");
            return;
        }

        log("  Found %zu memories\n", mems.size());

        uint32_t next_addr = 0;

        for (auto &mem : mems) {
            MemInfo mi;
            mi.mem = &mem;
            mi.memid = mem.memid.str();
            if (mi.memid[0] == '\\') mi.memid = mi.memid.substr(1);
            mi.width = mem.width;
            mi.depth = mem.size;
            mi.abits = ceil_log2(mi.depth);
            mi.base_addr = next_addr;

            // Check for $readmemh/$readmemb metadata (set by frontend as module attrs)
            {
                auto file_attr = RTLIL::IdString("\\loom_readmem_file_" + mi.memid);
                auto hex_attr = RTLIL::IdString("\\loom_readmem_hex_" + mi.memid);
                auto readmem_file = module->get_string_attribute(file_attr);
                if (!readmem_file.empty()) {
                    mi.init_file = readmem_file;
                    mi.init_file_hex = module->get_bool_attribute(hex_attr);
                    log("  Memory %s: init_file=%s (%s)\n",
                        mi.memid.c_str(), mi.init_file.c_str(),
                        mi.init_file_hex ? "hex" : "bin");
                }
            }

            log("  Memory %s: depth=%d, width=%d, addr_bits=%d, base=0x%08x\n",
                mi.memid.c_str(), mi.depth, mi.width, mi.abits, mi.base_addr);

            // Calculate address space (word-addressed, 4 bytes per word for AXI alignment)
            int words_per_entry = (mi.width + 31) / 32;
            if (words_per_entry < 1) words_per_entry = 1;
            int total_words = mi.depth * words_per_entry;
            next_addr += total_words * 4;

            // Add shadow ports (internal wires)
            add_shadow_ports(module, mem, mi, clk_name);

            memories.push_back(mi);
        }

        // Extract initial content BEFORE emit (need valid Mem references)
        extract_init_content(module, memories);

        // Commit memory changes back to module
        for (auto &mem : mems) {
            mem.emit();
        }

        // Clear Mem pointers since the mems vector will go out of scope
        for (auto &mi : memories) {
            mi.mem = nullptr;
        }

        log("  Added shadow ports to %zu memories, address space: %u bytes\n",
            memories.size(), next_addr);
    }

    void add_shadow_ports(RTLIL::Module *module, Mem &mem, MemInfo &mi,
                          const std::string &clk_name) {
        std::string prefix = "loom_shadow_" + mi.memid;

        // Create shadow port wires (internal, NOT module ports)
        mi.shadow_addr  = module->addWire(RTLIL::IdString("\\" + prefix + "_addr"), mi.abits);
        mi.shadow_rdata = module->addWire(RTLIL::IdString("\\" + prefix + "_rdata"), mi.width);
        mi.shadow_wdata = module->addWire(RTLIL::IdString("\\" + prefix + "_wdata"), mi.width);
        mi.shadow_wen   = module->addWire(RTLIL::IdString("\\" + prefix + "_wen"), 1);
        mi.shadow_ren   = module->addWire(RTLIL::IdString("\\" + prefix + "_ren"), 1);

        // Find the DUT clock wire — shadow accesses happen while loom_en=0
        // (DUT frozen), so the DUT clock is sufficient.
        RTLIL::Wire *dut_clk = module->wire(RTLIL::escape_id(clk_name));
        if (!dut_clk) {
            log_error("DUT clock '%s' not found. Use -clk to specify.\n", clk_name.c_str());
        }

        // Add shadow read port
        MemRd rd_port;
        rd_port.clk_enable = true;
        rd_port.clk_polarity = true;
        rd_port.ce_over_srst = false;
        rd_port.clk = RTLIL::SigSpec(dut_clk);
        rd_port.en = RTLIL::SigSpec(mi.shadow_ren);
        rd_port.arst = RTLIL::State::S0;
        rd_port.srst = RTLIL::State::S0;
        rd_port.addr = RTLIL::SigSpec(mi.shadow_addr);
        rd_port.data = RTLIL::SigSpec(mi.shadow_rdata);
        rd_port.init_value = RTLIL::Const(RTLIL::State::Sx, mi.width);
        rd_port.arst_value = RTLIL::Const(RTLIL::State::Sx, mi.width);
        rd_port.srst_value = RTLIL::Const(RTLIL::State::Sx, mi.width);
        rd_port.transparency_mask.resize(mem.wr_ports.size());
        rd_port.collision_x_mask.resize(mem.wr_ports.size());
        mi.shadow_rd_port_idx = mem.rd_ports.size();
        mem.rd_ports.push_back(rd_port);

        // Add shadow write port
        MemWr wr_port;
        wr_port.wide_log2 = 0;
        wr_port.clk_enable = true;
        wr_port.clk_polarity = true;
        wr_port.clk = RTLIL::SigSpec(dut_clk);
        wr_port.en = RTLIL::SigSpec(mi.shadow_wen);
        for (int i = 1; i < mi.width; i++) {
            wr_port.en.append(RTLIL::SigSpec(mi.shadow_wen));
        }
        wr_port.addr = RTLIL::SigSpec(mi.shadow_addr);
        wr_port.data = RTLIL::SigSpec(mi.shadow_wdata);
        wr_port.priority_mask.resize(mem.wr_ports.size());
        mi.shadow_wr_port_idx = mem.wr_ports.size();
        mem.wr_ports.push_back(wr_port);

        // Update masks
        for (auto &rp : mem.rd_ports) {
            rp.transparency_mask.resize(mem.wr_ports.size());
            rp.collision_x_mask.resize(mem.wr_ports.size());
        }
        for (auto &wp : mem.wr_ports) {
            wp.priority_mask.resize(mem.wr_ports.size());
        }
    }

    void generate_mem_ctrl(RTLIL::Design *design, const std::string &ctrl_name,
                          std::vector<MemInfo> &memories) {
        RTLIL::Module *ctrl = design->addModule(RTLIL::IdString("\\" + ctrl_name));
        log("  Generating controller module: %s\n", ctrl_name.c_str());

        // Calculate unified interface widths
        uint32_t total_addr_space = 0;
        int max_width = 0;
        int max_abits = 0;
        for (const auto &mi : memories) {
            int words_per_entry = (mi.width + 31) / 32;
            if (words_per_entry < 1) words_per_entry = 1;
            uint32_t mem_size = mi.depth * words_per_entry * 4;
            total_addr_space = mi.base_addr + mem_size;
            if (mi.width > max_width) max_width = mi.width;
            if (mi.abits > max_abits) max_abits = mi.abits;
        }
        int global_addr_bits = ceil_log2(total_addr_space);
        if (global_addr_bits < 2) global_addr_bits = 2;

        // Unified interface ports (directly connected to top-level)
        RTLIL::Wire *clk = ctrl->addWire(ID(clk_i), 1);
        clk->port_input = true;

        RTLIL::Wire *addr = ctrl->addWire(ID(addr_i), global_addr_bits);
        addr->port_input = true;

        RTLIL::Wire *wdata = ctrl->addWire(ID(wdata_i), max_width);
        wdata->port_input = true;

        RTLIL::Wire *rdata = ctrl->addWire(ID(rdata_o), max_width);
        rdata->port_output = true;

        RTLIL::Wire *wen = ctrl->addWire(ID(wen_i), 1);
        wen->port_input = true;

        RTLIL::Wire *ren = ctrl->addWire(ID(ren_i), 1);
        ren->port_input = true;

        // Per-memory output ports
        std::vector<RTLIL::Wire*> mem_addr_wires;
        std::vector<RTLIL::Wire*> mem_rdata_wires;
        std::vector<RTLIL::Wire*> mem_wdata_wires;
        std::vector<RTLIL::Wire*> mem_wen_wires;
        std::vector<RTLIL::Wire*> mem_ren_wires;

        for (size_t i = 0; i < memories.size(); i++) {
            auto &mi = memories[i];
            std::string prefix = "mem" + std::to_string(i);

            RTLIL::Wire *mem_addr = ctrl->addWire(RTLIL::IdString("\\" + prefix + "_addr_o"), mi.abits);
            mem_addr->port_output = true;
            mem_addr_wires.push_back(mem_addr);

            RTLIL::Wire *mem_rdata = ctrl->addWire(RTLIL::IdString("\\" + prefix + "_rdata_i"), mi.width);
            mem_rdata->port_input = true;
            mem_rdata_wires.push_back(mem_rdata);

            RTLIL::Wire *mem_wdata = ctrl->addWire(RTLIL::IdString("\\" + prefix + "_wdata_o"), mi.width);
            mem_wdata->port_output = true;
            mem_wdata_wires.push_back(mem_wdata);

            RTLIL::Wire *mem_wen = ctrl->addWire(RTLIL::IdString("\\" + prefix + "_wen_o"), 1);
            mem_wen->port_output = true;
            mem_wen_wires.push_back(mem_wen);

            RTLIL::Wire *mem_ren = ctrl->addWire(RTLIL::IdString("\\" + prefix + "_ren_o"), 1);
            mem_ren->port_output = true;
            mem_ren_wires.push_back(mem_ren);
        }

        ctrl->fixup_ports();

        // Generate address decode logic
        // Create memory select signals
        std::vector<RTLIL::Wire*> mem_sel_wires;
        for (size_t i = 0; i < memories.size(); i++) {
            RTLIL::Wire *sel = ctrl->addWire(NEW_ID, 1);
            mem_sel_wires.push_back(sel);
        }

        // For each memory, generate:
        // 1. Address range comparator -> mem_sel[i]
        // 2. Local address extraction -> mem_addr_o
        // 3. Write data routing -> mem_wdata_o (truncated)
        // 4. Gated wen/ren -> mem_wen_o, mem_ren_o

        for (size_t i = 0; i < memories.size(); i++) {
            auto &mi = memories[i];

            // Calculate address range
            int words_per_entry = (mi.width + 31) / 32;
            if (words_per_entry < 1) words_per_entry = 1;
            uint32_t end_addr = mi.base_addr + mi.depth * words_per_entry * 4;

            // Address range check: base_addr <= addr < end_addr
            // We use comparison cells with proper signed/unsigned handling

            RTLIL::SigSpec addr_sig(addr);

            // Create comparison cells manually with correct parameters
            // addr >= base_addr (unsigned comparison)
            RTLIL::Wire *ge_result = ctrl->addWire(NEW_ID, 1);
            RTLIL::Cell *ge_cell = ctrl->addCell(NEW_ID, ID($ge));
            ge_cell->parameters[ID::A_SIGNED] = RTLIL::Const(0);
            ge_cell->parameters[ID::B_SIGNED] = RTLIL::Const(0);
            ge_cell->parameters[ID::A_WIDTH] = RTLIL::Const(global_addr_bits);
            ge_cell->parameters[ID::B_WIDTH] = RTLIL::Const(global_addr_bits);
            ge_cell->parameters[ID::Y_WIDTH] = RTLIL::Const(1);
            ge_cell->setPort(ID::A, addr_sig);
            ge_cell->setPort(ID::B, RTLIL::SigSpec(RTLIL::Const(mi.base_addr, global_addr_bits)));
            ge_cell->setPort(ID::Y, RTLIL::SigSpec(ge_result));

            // addr < end_addr (unsigned comparison)
            RTLIL::Wire *lt_result = ctrl->addWire(NEW_ID, 1);
            RTLIL::Cell *lt_cell = ctrl->addCell(NEW_ID, ID($lt));
            lt_cell->parameters[ID::A_SIGNED] = RTLIL::Const(0);
            lt_cell->parameters[ID::B_SIGNED] = RTLIL::Const(0);
            lt_cell->parameters[ID::A_WIDTH] = RTLIL::Const(global_addr_bits);
            lt_cell->parameters[ID::B_WIDTH] = RTLIL::Const(global_addr_bits);
            lt_cell->parameters[ID::Y_WIDTH] = RTLIL::Const(1);
            lt_cell->setPort(ID::A, addr_sig);
            lt_cell->setPort(ID::B, RTLIL::SigSpec(RTLIL::Const(end_addr, global_addr_bits)));
            lt_cell->setPort(ID::Y, RTLIL::SigSpec(lt_result));

            // sel = ge_result && lt_result
            ctrl->addAnd(NEW_ID, RTLIL::SigSpec(ge_result), RTLIL::SigSpec(lt_result), RTLIL::SigSpec(mem_sel_wires[i]));

            // Local address: (addr - base_addr) >> 2, then extract lower abits
            // Simplified: just extract bits [2+abits-1:2] from (addr - base_addr)
            RTLIL::Wire *local_addr_full = ctrl->addWire(NEW_ID, global_addr_bits);
            ctrl->addSub(NEW_ID, addr_sig, RTLIL::Const(mi.base_addr, global_addr_bits), local_addr_full, false);

            // Shift right by 2 (byte to word address) and extract lower abits
            if (mi.abits > 0) {
                RTLIL::SigSpec local_addr_shifted;
                for (int b = 0; b < mi.abits; b++) {
                    local_addr_shifted.append(RTLIL::SigBit(local_addr_full, b + 2));
                }
                ctrl->connect(RTLIL::SigSpec(mem_addr_wires[i]), local_addr_shifted);
            }

            // Write data: truncate global wdata to memory width
            if (mi.width <= max_width) {
                RTLIL::SigSpec wdata_truncated;
                for (int b = 0; b < mi.width; b++) {
                    wdata_truncated.append(RTLIL::SigBit(wdata, b));
                }
                ctrl->connect(RTLIL::SigSpec(mem_wdata_wires[i]), wdata_truncated);
            }

            // Gated write enable: wen_i && sel
            ctrl->addAnd(NEW_ID, RTLIL::SigSpec(wen), RTLIL::SigSpec(mem_sel_wires[i]), RTLIL::SigSpec(mem_wen_wires[i]));

            // Gated read enable: ren_i && sel
            ctrl->addAnd(NEW_ID, RTLIL::SigSpec(ren), RTLIL::SigSpec(mem_sel_wires[i]), RTLIL::SigSpec(mem_ren_wires[i]));
        }

        // Read data mux: select based on mem_sel, zero-pad to max_width
        // Build cascaded mux: rdata = sel[0] ? rdata0 : (sel[1] ? rdata1 : ... : 0)
        RTLIL::SigSpec rdata_result = RTLIL::Const(0, max_width);

        for (int i = memories.size() - 1; i >= 0; i--) {
            auto &mi = memories[i];
            RTLIL::Wire *mux_out = ctrl->addWire(NEW_ID, max_width);

            // Zero-pad memory rdata to max_width
            RTLIL::SigSpec padded_rdata;
            for (int b = 0; b < max_width; b++) {
                if (b < mi.width) {
                    padded_rdata.append(RTLIL::SigBit(mem_rdata_wires[i], b));
                } else {
                    padded_rdata.append(RTLIL::State::S0);
                }
            }

            ctrl->addMux(NEW_ID, rdata_result, padded_rdata, RTLIL::SigSpec(mem_sel_wires[i]), mux_out);
            rdata_result = RTLIL::SigSpec(mux_out);
        }

        ctrl->connect(RTLIL::SigSpec(rdata), rdata_result);

        log("  Controller: %zu memories, %d addr bits, %d data bits\n",
            memories.size(), global_addr_bits, max_width);
    }

    void instantiate_mem_ctrl(RTLIL::Module *module, const std::string &ctrl_name,
                              std::vector<MemInfo> &memories,
                              const std::string &clk_name) {
        // Calculate widths for unified interface
        uint32_t total_addr_space = 0;
        int max_width = 0;
        for (const auto &mi : memories) {
            int words_per_entry = (mi.width + 31) / 32;
            if (words_per_entry < 1) words_per_entry = 1;
            uint32_t mem_size = mi.depth * words_per_entry * 4;
            total_addr_space = mi.base_addr + mem_size;
            if (mi.width > max_width) max_width = mi.width;
        }
        int global_addr_bits = ceil_log2(total_addr_space);
        if (global_addr_bits < 2) global_addr_bits = 2;

        // Find the DUT clock wire for the controller
        RTLIL::Wire *dut_clk = module->wire(RTLIL::escape_id(clk_name));
        if (!dut_clk)
            log_error("DUT clock '%s' not found.\n", clk_name.c_str());

        RTLIL::Wire *shadow_addr = module->addWire(ID(loom_shadow_addr), global_addr_bits);
        shadow_addr->port_input = true;

        RTLIL::Wire *shadow_wdata = module->addWire(ID(loom_shadow_wdata), max_width);
        shadow_wdata->port_input = true;

        RTLIL::Wire *shadow_rdata = module->addWire(ID(loom_shadow_rdata), max_width);
        shadow_rdata->port_output = true;

        RTLIL::Wire *shadow_wen = module->addWire(ID(loom_shadow_wen), 1);
        shadow_wen->port_input = true;

        RTLIL::Wire *shadow_ren = module->addWire(ID(loom_shadow_ren), 1);
        shadow_ren->port_input = true;

        module->fixup_ports();

        // Instantiate controller
        RTLIL::Cell *ctrl_inst = module->addCell(ID(loom_mem_ctrl_inst),
                                                  RTLIL::IdString("\\" + ctrl_name));

        // Connect unified interface
        ctrl_inst->setPort(ID(clk_i), RTLIL::SigSpec(dut_clk));
        ctrl_inst->setPort(ID(addr_i), RTLIL::SigSpec(shadow_addr));
        ctrl_inst->setPort(ID(wdata_i), RTLIL::SigSpec(shadow_wdata));
        ctrl_inst->setPort(ID(rdata_o), RTLIL::SigSpec(shadow_rdata));
        ctrl_inst->setPort(ID(wen_i), RTLIL::SigSpec(shadow_wen));
        ctrl_inst->setPort(ID(ren_i), RTLIL::SigSpec(shadow_ren));

        // Connect per-memory shadow ports
        for (size_t i = 0; i < memories.size(); i++) {
            auto &mi = memories[i];
            std::string prefix = "mem" + std::to_string(i);

            ctrl_inst->setPort(RTLIL::IdString("\\" + prefix + "_addr_o"),
                              RTLIL::SigSpec(mi.shadow_addr));
            ctrl_inst->setPort(RTLIL::IdString("\\" + prefix + "_rdata_i"),
                              RTLIL::SigSpec(mi.shadow_rdata));
            ctrl_inst->setPort(RTLIL::IdString("\\" + prefix + "_wdata_o"),
                              RTLIL::SigSpec(mi.shadow_wdata));
            ctrl_inst->setPort(RTLIL::IdString("\\" + prefix + "_wen_o"),
                              RTLIL::SigSpec(mi.shadow_wen));
            ctrl_inst->setPort(RTLIL::IdString("\\" + prefix + "_ren_o"),
                              RTLIL::SigSpec(mi.shadow_ren));
        }

        log("  Instantiated %s in %s\n", ctrl_name.c_str(), log_id(module));
    }

    // Extract initial memory content using Mem::get_init_data()
    void extract_init_content(RTLIL::Module * /*module*/, std::vector<MemInfo> &memories) {
        for (auto &mi : memories) {
            auto &mem = *mi.mem;

            // get_init_data() returns a single Const covering all memory entries.
            // Bits are width*depth total. Uninitialized bits are State::Sx.
            RTLIL::Const init_data = mem.get_init_data();

            // Check if there's any valid (non-x) init data
            bool has_valid = false;
            for (int i = 0; i < GetSize(init_data); i++) {
                if (init_data[i] == RTLIL::State::S0 || init_data[i] == RTLIL::State::S1) {
                    has_valid = true;
                    break;
                }
            }

            if (!has_valid)
                continue;

            // Pack init data as LE bytes
            int bytes_per_entry = (mi.width + 7) / 8;
            mi.initial_content.resize(mi.depth * bytes_per_entry, 0);

            for (int entry = 0; entry < mi.depth; entry++) {
                int entry_offset = entry * bytes_per_entry;
                for (int b = 0; b < mi.width; b++) {
                    int bit_idx = entry * mi.width + b;
                    if (bit_idx < GetSize(init_data) &&
                        init_data[bit_idx] == RTLIL::State::S1) {
                        mi.initial_content[entry_offset + b / 8] |= (1 << (b % 8));
                    }
                }
            }
            mi.has_initial_content = true;
            log("  Memory %s: extracted %d entries of init data (%d bytes)\n",
                mi.memid.c_str(), mi.depth, (int)mi.initial_content.size());

            // Clear init data from the memory — it will be loaded at runtime
            // via the shadow port preload mechanism. Leaving `initial` blocks
            // in the output Verilog is wrong for FPGA synthesis.
            mem.clear_inits();
        }
    }

    // Write MemMap protobuf
    void write_mem_map(const std::string &filename, const std::vector<MemInfo> &memories) {
        loom::MemMap mem_map;

        uint32_t total_size = 0;
        int max_width = 0;
        for (const auto &mi : memories) {
            int words_per_entry = (mi.width + 31) / 32;
            if (words_per_entry < 1) words_per_entry = 1;
            total_size = mi.base_addr + mi.depth * words_per_entry * 4;
            if (mi.width > max_width) max_width = mi.width;
        }
        int global_addr_bits = ceil_log2(total_size);
        if (global_addr_bits < 2) global_addr_bits = 2;

        mem_map.set_total_bytes(total_size);
        mem_map.set_addr_bits(global_addr_bits);
        mem_map.set_data_bits(max_width);
        mem_map.set_num_memories(memories.size());

        for (const auto &mi : memories) {
            auto *entry = mem_map.add_memories();
            entry->set_name(mi.memid);
            entry->set_depth(mi.depth);
            entry->set_width(mi.width);
            entry->set_addr_bits(mi.abits);
            entry->set_base_addr(mi.base_addr);

            int words_per_entry = (mi.width + 31) / 32;
            if (words_per_entry < 1) words_per_entry = 1;
            uint32_t end_addr = mi.base_addr + mi.depth * words_per_entry * 4;
            entry->set_end_addr(end_addr);

            if (mi.has_initial_content) {
                entry->set_initial_content(
                    std::string(mi.initial_content.begin(), mi.initial_content.end()));
            }
            if (!mi.init_file.empty()) {
                entry->set_init_file(mi.init_file);
                entry->set_init_file_hex(mi.init_file_hex);
            }
        }

        std::ofstream f(filename, std::ios::binary);
        if (!f.is_open()) {
            log_error("Could not open file '%s' for writing.\n", filename.c_str());
        }
        if (!mem_map.SerializeToOstream(&f)) {
            log_error("Failed to serialize MemMap to '%s'.\n", filename.c_str());
        }
        f.close();

        log("Wrote memory map protobuf to '%s' (%zu memories, %u bytes addr space)\n",
            filename.c_str(), memories.size(), total_size);
    }
} MemShadowPass;

PRIVATE_NAMESPACE_END
