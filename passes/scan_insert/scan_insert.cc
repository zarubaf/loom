/*
 * scan_insert - Yosys pass for scan chain insertion
 *
 * This pass inserts scan chain multiplexers on all flip-flops in the design,
 * enabling state capture and restore for FPGA emulation.
 *
 * For each flip-flop, a mux is inserted:
 *   - When scan_enable=0: normal D input passes through
 *   - When scan_enable=1: scan_in (from previous FF's Q) passes through
 *
 * The chain connects: scan_in -> FF1.D -> FF1.Q -> FF2.D -> ... -> scan_out
 */

#include "kernel/yosys.h"
#include "kernel/sigtools.h"

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

    void execute(std::vector<std::string> args, RTLIL::Design *design) override {
        log_header(design, "Executing SCAN_INSERT pass.\n");

        int chain_length = 0;
        bool check_equiv = false;

        size_t argidx;
        for (argidx = 1; argidx < args.size(); argidx++) {
            if (args[argidx] == "-chain_length" && argidx + 1 < args.size()) {
                chain_length = atoi(args[++argidx].c_str());
                continue;
            }
            if (args[argidx] == "-check_equiv") {
                check_equiv = true;
                continue;
            }
            break;
        }
        extra_args(args, argidx, design);

        for (auto module : design->selected_modules()) {
            log("Processing module %s\n", log_id(module));

            if (check_equiv) {
                run_scan_insert_with_equiv_check(module, chain_length, design);
            } else {
                run_scan_insert(module, chain_length);
            }
        }
    }

    void run_scan_insert(RTLIL::Module *module, int /* chain_length */) {
        // Collect all flip-flop cells
        std::vector<RTLIL::Cell*> dffs;
        for (auto cell : module->cells()) {
            if (is_ff(cell)) {
                dffs.push_back(cell);
            }
        }

        if (dffs.empty()) {
            log("  No flip-flops found, skipping.\n");
            return;
        }

        log("  Found %zu flip-flop(s)\n", dffs.size());

        // Add scan ports
        RTLIL::Wire *scan_en = module->addWire(ID(\\scan_enable), 1);
        scan_en->port_input = true;

        RTLIL::Wire *scan_in = module->addWire(ID(\\scan_in), 1);
        scan_in->port_input = true;

        RTLIL::Wire *scan_out = module->addWire(ID(\\scan_out), 1);
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

            // For multi-bit FFs, we need to handle scan bit-by-bit or in parallel
            // For simplicity, we do parallel scan (all bits shift together)
            // This requires scan_in to be expanded to match width

            // Create intermediate wire for mux output
            RTLIL::Wire *mux_out = module->addWire(NEW_ID, width);

            // Expand prev_q to match width if needed (repeat the signal)
            RTLIL::SigSpec scan_data;
            if (GetSize(prev_q) < width) {
                // For parallel scan: replicate scan input or use serial
                // Here we do serial: take LSB of prev_q for all bits shifted in
                scan_data = RTLIL::SigSpec();
                for (int i = 0; i < width; i++) {
                    scan_data.append(prev_q[i % GetSize(prev_q)]);
                }
            } else {
                scan_data = prev_q.extract(0, width);
            }

            // Add mux: sel=scan_enable, A=orig_d (normal), B=scan_data (scan mode)
            module->addMux(NEW_ID, orig_d, scan_data, RTLIL::SigSpec(scan_en), mux_out);

            // Reconnect FF's D input to mux output
            dff->setPort(ID::D, RTLIL::SigSpec(mux_out));

            // Update prev_q to this FF's Q for the next iteration
            prev_q = q;
        }

        // Connect the last FF's Q to scan_out (take LSB for single-bit output)
        module->connect(RTLIL::SigSpec(scan_out), RTLIL::SigBit(prev_q[0]));

        // Update port list
        module->fixup_ports();

        log("  Inserted scan chain with %zu element(s)\n", dffs.size());
        log("  Added ports: scan_enable (in), scan_in (in), scan_out (out)\n");
    }

    void run_scan_insert_with_equiv_check(RTLIL::Module *module, int chain_length, RTLIL::Design *design) {
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
        run_scan_insert(module, chain_length);

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

    void tie_off_scan_ports(RTLIL::Module *module) {
        // Find scan ports
        RTLIL::Wire *scan_en = module->wire(ID(\\scan_enable));
        RTLIL::Wire *scan_in = module->wire(ID(\\scan_in));
        RTLIL::Wire *scan_out = module->wire(ID(\\scan_out));

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
