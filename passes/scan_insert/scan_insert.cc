// SPDX-License-Identifier: Apache-2.0
/*
 * scan_insert - Yosys pass for scan chain insertion
 *
 * This pass inserts scan chain multiplexers on all flip-flops in the design,
 * enabling state capture and restore for FPGA emulation.
 *
 * For each flip-flop, a mux is inserted:
 *   - When loom_scan_enable=0: normal D input passes through
 *   - When loom_scan_enable=1: loom_scan_in (from previous FF's Q) passes through
 *
 * The chain connects: loom_scan_in -> FF1.D -> FF1.Q -> FF2.D -> ... -> loom_scan_out
 *
 * Generates a protobuf scan map file that maps scan chain bit positions to
 * original flip-flop names grouped by variable.
 */

#include "kernel/yosys.h"
#include "kernel/sigtools.h"
#include "loom_snapshot.pb.h"
#include <fstream>
#include <sstream>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct ScanInsertPass : public Pass {
    ScanInsertPass() : Pass("scan_insert", "Insert scan chains into the design") {}

    void help() override {
        log("\n");
        log("    scan_insert [options] [selection]\n");
        log("\n");
        log("Insert scan chain multiplexers on all flip-flops.\n");
        log("\n");
        log("    -chain_length N\n");
        log("        Maximum flip-flops per chain (default: all in one chain)\n");
        log("\n");
        log("    -map <file.pb>\n");
        log("        Write scan chain mapping to protobuf file.\n");
        log("        Maps bit positions to original flip-flop names.\n");
        log("\n");
        log("    -check_equiv\n");
        log("        Verify functional equivalence after scan insertion.\n");
        log("        The design with scan_enable=0 should be equivalent to the\n");
        log("        original design. Uses inductive equivalence checking.\n");
        log("\n");
    }

    // Check if a cell is a flip-flop type
    static bool is_ff(RTLIL::Cell *cell) {
        return cell->type.in(
            ID($dff), ID($dffe), ID($adff), ID($adffe),
            ID($sdff), ID($sdffe), ID($sdffce),
            ID($dffsr), ID($dffsre), ID($aldff), ID($aldffe)
        );
    }

    // Check if a FF is a memory output register that was merged by memory_dff.
    // After memory_dff, merged FFs have Q outputs connected to $ffmerge_disconnected wires.
    // We skip these to avoid breaking BRAM inference in downstream tools (e.g., Vivado).
    static bool is_memory_output_ff(RTLIL::Cell *cell) {
        if (!cell->hasPort(ID::Q))
            return false;

        RTLIL::SigSpec q = cell->getPort(ID::Q);
        for (auto bit : q) {
            if (!bit.wire)
                continue;
            // Check if wire name contains "ffmerge_disconnected" - indicates merged memory FF
            std::string wire_name = bit.wire->name.str();
            if (wire_name.find("ffmerge_disconnected") != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    // Get the original HDL path for a wire, using the hdlname attribute.
    // Falls back to the Yosys wire name with leading backslash stripped.
    static std::string get_hdl_name(RTLIL::Wire *wire) {
        if (wire->has_attribute(ID::hdlname)) {
            // hdlname stores space-separated hierarchical components
            std::string hdl = wire->get_string_attribute(ID::hdlname);
            // Replace spaces with dots for hierarchical path
            for (auto &ch : hdl) {
                if (ch == ' ') ch = '.';
            }
            return hdl;
        }
        // Fallback: strip leading backslash from Yosys name
        std::string name = wire->name.str();
        if (!name.empty() && name[0] == '\\')
            name = name.substr(1);
        return name;
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override {
        log_header(design, "Executing SCAN_INSERT pass.\n");

        int chain_length = 0;
        bool check_equiv = false;
        std::string map_file;

        size_t argidx;
        for (argidx = 1; argidx < args.size(); argidx++) {
            if (args[argidx] == "-chain_length" && argidx + 1 < args.size()) {
                chain_length = atoi(args[++argidx].c_str());
                continue;
            }
            if (args[argidx] == "-map" && argidx + 1 < args.size()) {
                map_file = args[++argidx];
                continue;
            }
            if (args[argidx] == "-check_equiv") {
                check_equiv = true;
                continue;
            }
            break;
        }
        extra_args(args, argidx, design);

        // Collect variables across all modules
        loom::ScanMap scan_map;
        int total_chain_bits = 0;

        for (auto module : design->selected_modules()) {
            log("Processing module %s\n", log_id(module));

            std::string mod_name = module->name.str();
            if (mod_name[0] == '\\') mod_name = mod_name.substr(1);

            if (check_equiv) {
                run_scan_insert_with_equiv_check(module, chain_length, design,
                                                  scan_map, total_chain_bits, mod_name);
            } else {
                run_scan_insert(module, chain_length,
                                scan_map, total_chain_bits, mod_name);
            }
        }

        scan_map.set_chain_length(total_chain_bits);

        // Write mapping file if requested
        if (!map_file.empty() && scan_map.variables_size() > 0) {
            write_scan_map(map_file, scan_map);
        }
    }

    void run_scan_insert(RTLIL::Module *module, int /* chain_length */,
                         loom::ScanMap &scan_map, int &chain_pos,
                         const std::string &mod_name) {
        // Collect all flip-flop cells, skipping memory output registers
        std::vector<RTLIL::Cell*> dffs;
        int skipped_mem_ffs = 0;
        for (auto cell : module->cells()) {
            if (is_ff(cell)) {
                if (is_memory_output_ff(cell)) {
                    log("  Skipping memory output FF: %s\n", log_id(cell));
                    skipped_mem_ffs++;
                    continue;
                }
                dffs.push_back(cell);
            }
        }

        if (dffs.empty()) {
            if (skipped_mem_ffs > 0) {
                log("  No flip-flops to scan (skipped %d memory output FFs).\n", skipped_mem_ffs);
            } else {
                log("  No flip-flops found, skipping.\n");
            }
            return;
        }

        log("  Found %zu flip-flop(s) to scan", dffs.size());
        if (skipped_mem_ffs > 0) {
            log(" (skipped %d memory output FFs)", skipped_mem_ffs);
        }
        log("\n");

        // Add scan ports (loom_ prefix for all generated signals)
        RTLIL::Wire *scan_en = module->addWire(ID(loom_scan_enable), 1);
        scan_en->port_input = true;

        RTLIL::Wire *scan_in = module->addWire(ID(loom_scan_in), 1);
        scan_in->port_input = true;

        RTLIL::Wire *scan_out = module->addWire(ID(loom_scan_out), 1);
        scan_out->port_output = true;

        // Track the previous Q output to chain to next FF
        RTLIL::SigSpec prev_q = RTLIL::SigSpec(scan_in);

        // Process each flip-flop
        for (auto dff : dffs) {
            // Get the D and Q signals
            RTLIL::SigSpec orig_d = dff->getPort(ID::D);
            RTLIL::SigSpec q = dff->getPort(ID::Q);
            int width = GetSize(orig_d);

            log("  Processing %s (width=%d)\n", log_id(dff), width);

            // Resolve the HDL name via the Q output wire's hdlname attribute
            std::string var_name;
            for (int i = 0; i < GetSize(q); i++) {
                RTLIL::SigBit bit = q[i];
                if (bit.wire) {
                    var_name = get_hdl_name(bit.wire);
                    break;
                }
            }
            if (var_name.empty()) {
                // Fallback to cell name
                var_name = dff->name.str();
                if (var_name[0] == '\\') var_name = var_name.substr(1);
            }

            // Prepend module name for hierarchical path
            std::string full_name = mod_name + "." + var_name;

            // Record one ScanVariable per FF (grouped, not per-bit)
            auto *var = scan_map.add_variables();
            var->set_name(full_name);
            var->set_width(width);
            var->set_offset(chain_pos);

            // Propagate enum member metadata from wire attribute to protobuf
            for (int i = 0; i < GetSize(q); i++) {
                RTLIL::SigBit bit = q[i];
                if (bit.wire && bit.wire->has_attribute(ID(loom_enum_members))) {
                    std::string members_str = bit.wire->get_string_attribute(ID(loom_enum_members));
                    // Parse "Name:Val,Name:Val,..." format
                    std::istringstream stream(members_str);
                    std::string token;
                    while (std::getline(stream, token, ',')) {
                        auto colon = token.find(':');
                        if (colon == std::string::npos) continue;
                        auto *em = var->add_enum_members();
                        em->set_name(token.substr(0, colon));
                        em->set_value(std::stoull(token.substr(colon + 1)));
                    }
                    break;
                }
            }

            chain_pos += width;

            // For multi-bit FFs, we do bit-serial scan:
            // Each bit of the FF is connected individually in the chain
            // This allows proper bit-by-bit capture and restore

            // Create intermediate wire for mux output
            RTLIL::Wire *mux_out = module->addWire(NEW_ID, width);

            // Build serial scan chain: each bit connects to the previous bit's Q
            RTLIL::SigSpec scan_data;
            for (int i = 0; i < width; i++) {
                if (i == 0) {
                    // First bit of this FF connects to previous FF's last bit
                    scan_data.append(prev_q[GetSize(prev_q) - 1]);
                } else {
                    // Subsequent bits connect to this FF's previous bit
                    scan_data.append(q[i - 1]);
                }
            }

            // Add mux: sel=scan_enable, A=orig_d (normal), B=scan_data (scan mode)
            module->addMux(NEW_ID, orig_d, scan_data, RTLIL::SigSpec(scan_en), mux_out);

            // Reconnect FF's D input to mux output
            dff->setPort(ID::D, RTLIL::SigSpec(mux_out));

            // Update prev_q to this FF's Q for the next iteration
            prev_q = q;
        }

        // Connect the last FF's Q MSB to scan_out (end of serial chain)
        module->connect(RTLIL::SigSpec(scan_out), RTLIL::SigBit(prev_q[GetSize(prev_q) - 1]));

        // Update port list
        module->fixup_ports();

        // Stamp chain length on the module so emu_top can read it
        module->set_string_attribute(ID(loom_scan_chain_length), std::to_string(chain_pos));

        log("  Inserted scan chain with %zu element(s), %d bits total\n", dffs.size(), chain_pos);
        log("  Added ports: loom_scan_enable (in), loom_scan_in (in), loom_scan_out (out)\n");
    }

    void run_scan_insert_with_equiv_check(RTLIL::Module *module, int chain_length,
                                           RTLIL::Design *design,
                                           loom::ScanMap &scan_map, int &chain_pos,
                                           const std::string &mod_name) {
        std::string orig_name = module->name.str();
        std::string gold_name = orig_name + "_gold";
        std::string gate_name = orig_name + "_gate";

        log("  Equivalence checking enabled\n");

        // Step 1: Create gold copy (original design before scan insertion)
        log("  Creating gold reference: %s\n", gold_name.c_str());
        RTLIL::Module *gold = module->clone();
        gold->name = RTLIL::IdString(gold_name);
        design->add(gold);

        // Step 2: Run scan insertion on the original module
        run_scan_insert(module, chain_length, scan_map, chain_pos, mod_name);

        // Step 3: Create gate copy and tie off scan ports
        log("  Creating gate copy with scan ports tied off: %s\n", gate_name.c_str());
        RTLIL::Module *gate = module->clone();
        gate->name = RTLIL::IdString(gate_name);
        design->add(gate);

        // Tie off scan_enable=0 and scan_in=0 in the gate module
        tie_off_scan_ports(gate);

        // Step 4: Run equivalence checking
        log("  Running equivalence check: %s vs %s\n", gold_name.c_str(), gate_name.c_str());

        bool equiv_passed = run_equiv_check(design, gold_name, gate_name);

        // Step 5: Clean up temporary modules
        design->remove(design->module(RTLIL::IdString(gold_name)));
        design->remove(design->module(RTLIL::IdString(gate_name)));

        if (equiv_passed) {
            log("  Equivalence check PASSED\n");
        } else {
            log_error("  Equivalence check FAILED - scan insertion may have altered functionality\n");
        }
    }

    void write_scan_map(const std::string &filename, const loom::ScanMap &scan_map) {
        std::ofstream f(filename, std::ios::binary);
        if (!f.is_open()) {
            log_error("Cannot open scan map file '%s' for writing\n", filename.c_str());
        }

        if (!scan_map.SerializeToOstream(&f)) {
            log_error("Failed to serialize scan map to '%s'\n", filename.c_str());
        }

        f.close();
        log("Wrote scan chain mapping to '%s' (%d variables, %u bits)\n",
            filename.c_str(), scan_map.variables_size(), scan_map.chain_length());
    }

    void tie_off_scan_ports(RTLIL::Module *module) {
        // Find scan ports
        RTLIL::Wire *scan_en = module->wire(ID(loom_scan_enable));
        RTLIL::Wire *scan_in = module->wire(ID(loom_scan_in));
        RTLIL::Wire *scan_out = module->wire(ID(loom_scan_out));

        SigMap sigmap(module);

        // Replace all uses of scan_enable and scan_in with constant 0
        for (auto cell : module->cells()) {
            for (auto &conn : cell->connections()) {
                if (!cell->input(conn.first))
                    continue;

                RTLIL::SigSpec sig = conn.second;
                bool modified = false;

                for (int i = 0; i < GetSize(sig); i++) {
                    RTLIL::SigBit bit = sig[i];
                    if (bit.wire == scan_en || bit.wire == scan_in) {
                        sig[i] = RTLIL::State::S0;
                        modified = true;
                    }
                }

                if (modified) {
                    cell->setPort(conn.first, sig);
                }
            }
        }

        // Remove the scan ports
        if (scan_en) {
            scan_en->port_input = false;
        }
        if (scan_in) {
            scan_in->port_input = false;
        }
        if (scan_out) {
            scan_out->port_output = false;
        }

        module->fixup_ports();
    }

    bool run_equiv_check(RTLIL::Design *design, const std::string &gold_name, const std::string &gate_name) {
        std::string equiv_name = "equiv_check";

        try {
            // Prepare designs for equivalence checking
            // Convert async FFs to sync logic for SAT solving
            Pass::call(design, stringf("async2sync %s", gold_name.c_str()));
            Pass::call(design, stringf("async2sync %s", gate_name.c_str()));

            // Optimize to clean up the designs
            Pass::call(design, stringf("opt_clean %s", gold_name.c_str()));
            Pass::call(design, stringf("opt_clean %s", gate_name.c_str()));

            // Create equivalence checking module
            Pass::call(design, stringf("equiv_make %s %s %s", gold_name.c_str(), gate_name.c_str(), equiv_name.c_str()));

            // Try simple combinational equivalence first
            Pass::call(design, stringf("equiv_simple %s", equiv_name.c_str()));

            // Run inductive equivalence (for sequential circuits)
            Pass::call(design, stringf("equiv_induct %s", equiv_name.c_str()));

            // Check status - this will set error count
            Pass::call(design, stringf("equiv_status -assert %s", equiv_name.c_str()));

            // Clean up equiv module
            if (design->module(RTLIL::IdString("\\" + equiv_name))) {
                design->remove(design->module(RTLIL::IdString("\\" + equiv_name)));
            }

            return true;
        } catch (...) {
            // Clean up equiv module on failure
            if (design->module(RTLIL::IdString("\\" + equiv_name))) {
                design->remove(design->module(RTLIL::IdString("\\" + equiv_name)));
            }
            return false;
        }
    }
};

ScanInsertPass ScanInsertPass_singleton;

PRIVATE_NAMESPACE_END
