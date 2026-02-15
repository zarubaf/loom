// SPDX-License-Identifier: Apache-2.0
/*
 * mem_scan_insert - Yosys pass for memory scan chain insertion
 *
 * This pass detects SRAM instances in the design and prepares them for
 * memory state capture/restore during FPGA emulation.
 *
 * For each SRAM:
 *   - Adds scan interface signals for address and data
 *   - Inserts muxes to switch between normal operation and scan mode
 *
 * Generates a JSON mapping file that describes the memory configuration
 * for use by the memory scan controller.
 */

#include "kernel/yosys.h"
#include "kernel/sigtools.h"
#include <fstream>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

// Memory element for mapping
struct MemoryElement {
    std::string inst_name;      // Instance name
    std::string module_name;    // SRAM module name
    int depth;                  // Number of words
    int width;                  // Bits per word
    int scan_order;             // Order in scan chain (0 = first)
};

struct MemScanInsertPass : public Pass {
    MemScanInsertPass() : Pass("mem_scan_insert", "Insert memory scan chains into the design") {}

    void help() override {
        log("\n");
        log("    mem_scan_insert [options] [selection]\n");
        log("\n");
        log("Insert memory scan interface for SRAM state capture.\n");
        log("\n");
        log("    -pattern <name>\n");
        log("        Module name pattern to match for SRAM detection (default: sram)\n");
        log("\n");
        log("    -map <file.json>\n");
        log("        Write memory configuration to JSON file.\n");
        log("\n");
    }

    // Extract integer parameter from a cell
    static int get_param_int(RTLIL::Cell *cell, IdString param, int default_val) {
        if (cell->hasParam(param)) {
            return cell->getParam(param).as_int();
        }
        return default_val;
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override {
        log_header(design, "Executing MEM_SCAN_INSERT pass.\n");

        std::string pattern = "sram";
        std::string map_file;

        size_t argidx;
        for (argidx = 1; argidx < args.size(); argidx++) {
            if (args[argidx] == "-pattern" && argidx + 1 < args.size()) {
                pattern = args[++argidx];
                continue;
            }
            if (args[argidx] == "-map" && argidx + 1 < args.size()) {
                map_file = args[++argidx];
                continue;
            }
            break;
        }
        extra_args(args, argidx, design);

        std::vector<MemoryElement> all_memories;

        for (auto module : design->selected_modules()) {
            log("Processing module %s\n", log_id(module));

            std::vector<MemoryElement> memories;
            run_mem_scan_insert(module, pattern, memories);

            // Add module prefix for hierarchical names
            std::string mod_name = module->name.str();
            if (mod_name[0] == '\\') mod_name = mod_name.substr(1);

            for (auto &mem : memories) {
                mem.inst_name = mod_name + "." + mem.inst_name;
                all_memories.push_back(mem);
            }
        }

        // Write mapping file if requested
        if (!map_file.empty() && !all_memories.empty()) {
            write_mem_map(map_file, all_memories);
        }
    }

    void run_mem_scan_insert(RTLIL::Module *module, const std::string &pattern, std::vector<MemoryElement> &memories) {
        // Collect all cells that match the pattern
        std::vector<RTLIL::Cell*> srams;
        for (auto cell : module->cells()) {
            std::string cell_type = cell->type.str();
            if (cell_type[0] == '\\') cell_type = cell_type.substr(1);

            if (cell_type.find(pattern) != std::string::npos) {
                srams.push_back(cell);
            }
        }

        if (srams.empty()) {
            log("  No memory instances matching '%s' found, skipping.\n", pattern.c_str());
            return;
        }

        log("  Found %zu memory instance(s)\n", srams.size());

        // Add memory scan enable port
        RTLIL::Wire *scan_en = module->addWire(ID(loom_mem_scan_enable), 1);
        scan_en->port_input = true;

        int mem_idx = 0;
        int total_bits = 0;

        for (auto sram : srams) {
            // Get actual port widths from the cell connections
            RTLIL::SigSpec orig_addr = sram->getPort(ID(addr_i));
            RTLIL::SigSpec orig_req = sram->getPort(ID(req_i));
            RTLIL::SigSpec rdata = sram->getPort(ID(rdata_o1));

            int addr_width = GetSize(orig_addr);
            int width = GetSize(rdata);
            int depth = 1 << addr_width;  // Estimate depth from address width

            std::string inst_name = sram->name.str();
            if (inst_name[0] == '\\') inst_name = inst_name.substr(1);

            log("  Processing %s: depth~%d, width=%d, addr_width=%d\n",
                inst_name.c_str(), depth, width, addr_width);

            // Record memory info
            MemoryElement mem;
            mem.inst_name = inst_name;
            mem.module_name = sram->type.str();
            if (mem.module_name[0] == '\\') mem.module_name = mem.module_name.substr(1);
            mem.depth = depth;
            mem.width = width;
            mem.scan_order = mem_idx;
            memories.push_back(mem);

            // Add scan interface ports for this memory
            std::string prefix = "loom_mem" + std::to_string(mem_idx);

            // Scan address input (from controller)
            RTLIL::Wire *scan_addr = module->addWire(RTLIL::IdString("\\" + prefix + "_scan_addr"), addr_width);
            scan_addr->port_input = true;

            // Scan request input (from controller)
            RTLIL::Wire *scan_req = module->addWire(RTLIL::IdString("\\" + prefix + "_scan_req"), 1);
            scan_req->port_input = true;

            // Scan read data output (to controller)
            RTLIL::Wire *scan_rdata = module->addWire(RTLIL::IdString("\\" + prefix + "_scan_rdata"), width);
            scan_rdata->port_output = true;

            // Create muxed signals (same width as original)
            RTLIL::Wire *mux_addr = module->addWire(NEW_ID, addr_width);
            RTLIL::Wire *mux_req = module->addWire(NEW_ID, 1);

            // Add address mux: scan_enable ? scan_addr : normal_addr
            module->addMux(NEW_ID, orig_addr, RTLIL::SigSpec(scan_addr), RTLIL::SigSpec(scan_en), mux_addr);

            // Add request mux: scan_enable ? scan_req : normal_req
            module->addMux(NEW_ID, orig_req, RTLIL::SigSpec(scan_req), RTLIL::SigSpec(scan_en), mux_req);

            // Reconnect SRAM ports to muxed signals
            sram->setPort(ID(addr_i), RTLIL::SigSpec(mux_addr));
            sram->setPort(ID(req_i), RTLIL::SigSpec(mux_req));

            // During scan, force we_i to 0 (read-only)
            RTLIL::SigSpec orig_we = sram->getPort(ID(we_i));
            RTLIL::Wire *mux_we = module->addWire(NEW_ID, 1);
            module->addMux(NEW_ID, orig_we, RTLIL::State::S0, RTLIL::SigSpec(scan_en), mux_we);
            sram->setPort(ID(we_i), RTLIL::SigSpec(mux_we));

            // Connect rdata to scan output port
            module->connect(RTLIL::SigSpec(scan_rdata), rdata);

            total_bits += depth * width;
            mem_idx++;
        }

        // Update port list
        module->fixup_ports();

        log("  Inserted memory scan interface for %zu memories, %d bits total\n",
            srams.size(), total_bits);
    }

    void write_mem_map(const std::string &filename, const std::vector<MemoryElement> &memories) {
        std::ofstream f(filename);
        if (!f.is_open()) {
            log_error("Could not open file '%s' for writing.\n", filename.c_str());
        }

        int total_bits = 0;
        for (const auto &mem : memories) {
            total_bits += mem.depth * mem.width;
        }

        f << "{\n";
        f << "  \"total_bits\": " << total_bits << ",\n";
        f << "  \"num_memories\": " << memories.size() << ",\n";
        f << "  \"memories\": [\n";

        for (size_t i = 0; i < memories.size(); i++) {
            const auto &mem = memories[i];
            f << "    {\n";
            f << "      \"instance\": \"" << mem.inst_name << "\",\n";
            f << "      \"module\": \"" << mem.module_name << "\",\n";
            f << "      \"depth\": " << mem.depth << ",\n";
            f << "      \"width\": " << mem.width << ",\n";
            f << "      \"scan_order\": " << mem.scan_order << ",\n";
            f << "      \"total_bits\": " << (mem.depth * mem.width) << "\n";
            f << "    }";
            if (i < memories.size() - 1) f << ",";
            f << "\n";
        }

        f << "  ]\n";
        f << "}\n";
        f.close();

        log("Wrote memory map to '%s' (%d bits)\n", filename.c_str(), total_bits);
    }
} MemScanInsertPass;

PRIVATE_NAMESPACE_END
