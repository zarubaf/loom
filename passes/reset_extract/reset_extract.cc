// SPDX-License-Identifier: Apache-2.0
/*
 * reset_extract - Yosys pass for extracting reset values and stripping resets
 *
 * This pass extracts reset values from all flip-flops and stores them as wire
 * attributes for scan-based initialization.  Async resets ($adff, $adffe,
 * $dffsr, $dffsre) are stripped to plain $dff/$dffe, eliminating the reset
 * distribution tree on the FPGA.  Sync resets ($sdff, $sdffe, $sdffce) have
 * their values recorded but are left intact — their SRST port logic becomes
 * dead once the reset signal is tied inactive.
 *
 * When -rst is given, the named reset port is driven to constant 1 (inactive
 * for active-low) and removed as a port.  A subsequent `opt` pass will
 * propagate the constant through all sync-reset logic and eliminate the dead
 * nodes, so the reset tree disappears entirely from the DUT.
 *
 * After this pass:
 *   - $adff  → $dff,  $adffe  → $dffe
 *   - $dffsr → $dff,  $dffsre → $dffe
 *   - $sdff, $sdffe, $sdffce  → unchanged (cleaned up by `opt`)
 *   - Every FF's Q wire carries `loom_reset_value` attribute (RTLIL::Const)
 *   - Module has `loom_resets_extracted = "1"` attribute
 *   - Reset port removed (driven to constant inactive)
 */

#include "kernel/yosys.h"
#include "kernel/sigtools.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct ResetExtractPass : public Pass {
    ResetExtractPass() : Pass("reset_extract", "Extract reset values and strip async resets") {}

    void help() override {
        log("\n");
        log("    reset_extract [options] [selection]\n");
        log("\n");
        log("Extract reset values from all flip-flops and store as wire attributes.\n");
        log("Async resets are stripped; sync resets are kept (cleaned up by opt).\n");
        log("\n");
        log("    -rst <signal>\n");
        log("        Name of the reset signal (default: rst_ni).\n");
        log("        The port is driven to constant inactive and removed.\n");
        log("\n");
        log("After this pass, every FF Q wire has a `loom_reset_value` attribute\n");
        log("containing the reset value as an RTLIL::Const.  The module is stamped\n");
        log("with `loom_resets_extracted = 1`.  Run `opt` afterwards to propagate\n");
        log("the constant reset signal and eliminate dead sync-reset logic.\n");
        log("\n");
    }

    // Derive reset value for $dffsr/$dffsre.
    // These cells are rare in practice; default to all-zeros.
    static RTLIL::Const derive_dffsr_reset_value(RTLIL::Cell *cell) {
        int width = cell->getParam(ID::WIDTH).as_int();
        return RTLIL::Const(RTLIL::State::S0, width);
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override {
        log_header(design, "Executing RESET_EXTRACT pass.\n");

        std::string rst_name = "rst_ni";

        size_t argidx;
        for (argidx = 1; argidx < args.size(); argidx++) {
            if (args[argidx] == "-rst" && argidx + 1 < args.size()) {
                rst_name = args[++argidx];
                continue;
            }
            break;
        }
        extra_args(args, argidx, design);

        for (auto module : design->selected_modules()) {
            log("Processing module %s\n", log_id(module));
            process_module(module, rst_name);
        }
    }

    void process_module(RTLIL::Module *module, const std::string &rst_name) {
        int async_stripped = 0;
        int sync_kept = 0;
        int no_reset = 0;

        std::vector<RTLIL::Cell*> cells_to_process;
        for (auto cell : module->cells())
            cells_to_process.push_back(cell);

        for (auto cell : cells_to_process) {
            RTLIL::IdString type = cell->type;

            // ---- Async reset FFs: extract value, STRIP ----

            if (type == ID($adff)) {
                RTLIL::Const arst_val = cell->getParam(ID::ARST_VALUE);
                set_reset_attr(cell, arst_val);
                strip_adff_to_dff(module, cell);
                async_stripped++;
                continue;
            }

            if (type == ID($adffe)) {
                RTLIL::Const arst_val = cell->getParam(ID::ARST_VALUE);
                set_reset_attr(cell, arst_val);
                strip_adffe_to_dffe(module, cell);
                async_stripped++;
                continue;
            }

            if (type == ID($dffsr)) {
                RTLIL::Const rst_val = derive_dffsr_reset_value(cell);
                set_reset_attr(cell, rst_val);
                strip_dffsr_to_dff(module, cell);
                async_stripped++;
                continue;
            }

            if (type == ID($dffsre)) {
                RTLIL::Const rst_val = derive_dffsr_reset_value(cell);
                set_reset_attr(cell, rst_val);
                strip_dffsre_to_dffe(module, cell);
                async_stripped++;
                continue;
            }

            // ---- Async load DFFs ($aldff/$aldffe) ----
            // AD port is the async load data. If constant, treat like $adff.
            // If driven by a DPI call, mark for host-side execution.

            if (type == ID($aldff) || type == ID($aldffe)) {
                RTLIL::SigSpec ad = cell->getPort(ID::AD);

                if (ad.is_fully_const()) {
                    // Constant async load value — same as $adff with ARST_VALUE
                    set_reset_attr(cell, ad.as_const());
                    if (type == ID($aldff))
                        strip_aldff_to_dff(module, cell);
                    else
                        strip_aldffe_to_dffe(module, cell);
                    async_stripped++;
                    continue;
                }

                // Check if AD is driven by a $__loom_dpi_call cell
                RTLIL::Cell *dpi_cell = find_driving_dpi_call(module, ad);
                if (dpi_cell) {
                    // Verify DPI args are all constants
                    RTLIL::SigSpec dpi_args = dpi_cell->getPort(ID(ARGS));
                    if (!dpi_args.is_fully_const()) {
                        log_error("DPI call '%s' in reset block has non-constant arguments. "
                                  "Only constant arguments are supported for reset-time DPI calls.\n",
                                  dpi_cell->get_string_attribute(ID(loom_dpi_func)).c_str());
                    }

                    // Mark DPI cell for host execution (not hardware bridge)
                    dpi_cell->set_bool_attribute(ID(loom_dpi_reset), true);
                    dpi_cell->set_bool_attribute(ID(keep), true);

                    // Store DPI function name on Q wire for scan_insert
                    RTLIL::SigSpec q = cell->getPort(ID::Q);
                    int width = cell->getParam(ID::WIDTH).as_int();
                    for (auto &bit : q) {
                        if (bit.wire) {
                            bit.wire->set_string_attribute(ID(loom_reset_dpi_func),
                                dpi_cell->get_string_attribute(ID(loom_dpi_func)));
                            bit.wire->attributes[ID(loom_reset_value)] = RTLIL::Const(RTLIL::State::S0, width);
                            break;
                        }
                    }

                    if (type == ID($aldff))
                        strip_aldff_to_dff(module, cell);
                    else
                        strip_aldffe_to_dffe(module, cell);
                    async_stripped++;
                } else {
                    // Non-constant, non-DPI $aldff: unsupported
                    log_error("Unsupported $aldff cell %s: AD port is not constant and not driven by DPI call.\n",
                              log_id(cell));
                }
                continue;
            }

            // ---- Sync reset FFs: extract value, KEEP ----

            if (type == ID($sdff)) {
                RTLIL::Const srst_val = cell->getParam(ID::SRST_VALUE);
                set_reset_attr(cell, srst_val);
                sync_kept++;
                continue;
            }

            if (type == ID($sdffe)) {
                RTLIL::Const srst_val = cell->getParam(ID::SRST_VALUE);
                set_reset_attr(cell, srst_val);
                sync_kept++;
                continue;
            }

            if (type == ID($sdffce)) {
                RTLIL::Const srst_val = cell->getParam(ID::SRST_VALUE);
                set_reset_attr(cell, srst_val);
                sync_kept++;
                continue;
            }

            // ---- No-reset FFs: skip (default 0) ----

            if (type == ID($dff) || type == ID($dffe)) {
                no_reset++;
                continue;
            }
        }

        if (async_stripped + sync_kept > 0) {
            module->set_string_attribute(ID(loom_resets_extracted), "1");

            // Tie the reset signal to constant inactive and remove the port.
            // For active-low rst_ni: drive to 1.  A subsequent `opt` pass
            // propagates this constant through all remaining sync-reset
            // logic, eliminating the entire reset tree from the DUT.
            RTLIL::Wire *rst_wire = module->wire(RTLIL::escape_id(rst_name));
            if (rst_wire && rst_wire->port_input) {
                rst_wire->port_input = false;
                // Active-low reset: inactive = 1
                module->connect(RTLIL::SigSpec(rst_wire),
                                RTLIL::SigSpec(RTLIL::State::S1, rst_wire->width));
                module->fixup_ports();
                log("  Removed reset port '%s' (tied to constant 1)\n", rst_name.c_str());
            }
        }

        log("  Async resets stripped: %d\n", async_stripped);
        log("  Sync resets kept: %d\n", sync_kept);
        log("  No-reset FFs: %d\n", no_reset);
    }

    // Set loom_reset_value attribute on the Q output wire
    void set_reset_attr(RTLIL::Cell *cell, const RTLIL::Const &reset_val) {
        RTLIL::SigSpec q = cell->getPort(ID::Q);
        for (auto &bit : q) {
            if (bit.wire) {
                bit.wire->attributes[ID(loom_reset_value)] = reset_val;
                break;  // Set on the first wire (covers the whole variable)
            }
        }
    }

    // $adff → $dff: remove ARST port and ARST_POLARITY/ARST_VALUE params
    void strip_adff_to_dff(RTLIL::Module *module, RTLIL::Cell *cell) {
        log("  Stripping %s: $adff → $dff (ARST_VALUE=%s)\n",
            log_id(cell), cell->getParam(ID::ARST_VALUE).as_string().c_str());

        RTLIL::Cell *new_cell = module->addCell(NEW_ID, ID($dff));
        new_cell->setParam(ID::WIDTH, cell->getParam(ID::WIDTH));
        new_cell->setParam(ID::CLK_POLARITY, cell->getParam(ID::CLK_POLARITY));
        new_cell->setPort(ID::CLK, cell->getPort(ID::CLK));
        new_cell->setPort(ID::D, cell->getPort(ID::D));
        new_cell->setPort(ID::Q, cell->getPort(ID::Q));

        module->remove(cell);
    }

    // $adffe → $dffe: remove ARST port and ARST_POLARITY/ARST_VALUE params
    void strip_adffe_to_dffe(RTLIL::Module *module, RTLIL::Cell *cell) {
        log("  Stripping %s: $adffe → $dffe (ARST_VALUE=%s)\n",
            log_id(cell), cell->getParam(ID::ARST_VALUE).as_string().c_str());

        RTLIL::Cell *new_cell = module->addCell(NEW_ID, ID($dffe));
        new_cell->setParam(ID::WIDTH, cell->getParam(ID::WIDTH));
        new_cell->setParam(ID::CLK_POLARITY, cell->getParam(ID::CLK_POLARITY));
        new_cell->setParam(ID::EN_POLARITY, cell->getParam(ID::EN_POLARITY));
        new_cell->setPort(ID::CLK, cell->getPort(ID::CLK));
        new_cell->setPort(ID::EN, cell->getPort(ID::EN));
        new_cell->setPort(ID::D, cell->getPort(ID::D));
        new_cell->setPort(ID::Q, cell->getPort(ID::Q));

        module->remove(cell);
    }

    // $dffsr → $dff: remove SET/CLR ports and related params
    void strip_dffsr_to_dff(RTLIL::Module *module, RTLIL::Cell *cell) {
        log("  Stripping %s: $dffsr → $dff\n", log_id(cell));

        RTLIL::Cell *new_cell = module->addCell(NEW_ID, ID($dff));
        new_cell->setParam(ID::WIDTH, cell->getParam(ID::WIDTH));
        new_cell->setParam(ID::CLK_POLARITY, cell->getParam(ID::CLK_POLARITY));
        new_cell->setPort(ID::CLK, cell->getPort(ID::CLK));
        new_cell->setPort(ID::D, cell->getPort(ID::D));
        new_cell->setPort(ID::Q, cell->getPort(ID::Q));

        module->remove(cell);
    }

    // $dffsre → $dffe: remove SET/CLR ports and related params, keep EN
    void strip_dffsre_to_dffe(RTLIL::Module *module, RTLIL::Cell *cell) {
        log("  Stripping %s: $dffsre → $dffe\n", log_id(cell));

        RTLIL::Cell *new_cell = module->addCell(NEW_ID, ID($dffe));
        new_cell->setParam(ID::WIDTH, cell->getParam(ID::WIDTH));
        new_cell->setParam(ID::CLK_POLARITY, cell->getParam(ID::CLK_POLARITY));
        new_cell->setParam(ID::EN_POLARITY, cell->getParam(ID::EN_POLARITY));
        new_cell->setPort(ID::CLK, cell->getPort(ID::CLK));
        new_cell->setPort(ID::EN, cell->getPort(ID::EN));
        new_cell->setPort(ID::D, cell->getPort(ID::D));
        new_cell->setPort(ID::Q, cell->getPort(ID::Q));

        module->remove(cell);
    }

    // $aldff → $dff: remove ALOAD/AD ports and ALOAD_POLARITY param
    void strip_aldff_to_dff(RTLIL::Module *module, RTLIL::Cell *cell) {
        log("  Stripping %s: $aldff → $dff\n", log_id(cell));

        RTLIL::Cell *new_cell = module->addCell(NEW_ID, ID($dff));
        new_cell->setParam(ID::WIDTH, cell->getParam(ID::WIDTH));
        new_cell->setParam(ID::CLK_POLARITY, cell->getParam(ID::CLK_POLARITY));
        new_cell->setPort(ID::CLK, cell->getPort(ID::CLK));
        new_cell->setPort(ID::D, cell->getPort(ID::D));
        new_cell->setPort(ID::Q, cell->getPort(ID::Q));

        module->remove(cell);
    }

    // $aldffe → $dffe: remove ALOAD/AD ports and ALOAD_POLARITY param, keep EN
    void strip_aldffe_to_dffe(RTLIL::Module *module, RTLIL::Cell *cell) {
        log("  Stripping %s: $aldffe → $dffe\n", log_id(cell));

        RTLIL::Cell *new_cell = module->addCell(NEW_ID, ID($dffe));
        new_cell->setParam(ID::WIDTH, cell->getParam(ID::WIDTH));
        new_cell->setParam(ID::CLK_POLARITY, cell->getParam(ID::CLK_POLARITY));
        new_cell->setParam(ID::EN_POLARITY, cell->getParam(ID::EN_POLARITY));
        new_cell->setPort(ID::CLK, cell->getPort(ID::CLK));
        new_cell->setPort(ID::EN, cell->getPort(ID::EN));
        new_cell->setPort(ID::D, cell->getPort(ID::D));
        new_cell->setPort(ID::Q, cell->getPort(ID::Q));

        module->remove(cell);
    }

    // Find $__loom_dpi_call cell driving a signal
    RTLIL::Cell *find_driving_dpi_call(RTLIL::Module *module, RTLIL::SigSpec sig) {
        SigMap sigmap(module);
        sig = sigmap(sig);
        for (auto cell : module->cells()) {
            if (cell->type != ID($__loom_dpi_call)) continue;
            if (!cell->hasPort(ID(RESULT))) continue;
            RTLIL::SigSpec result = sigmap(cell->getPort(ID(RESULT)));
            if (GetSize(result) > 0 && result == sig)
                return cell;
        }
        return nullptr;
    }
};

ResetExtractPass ResetExtractPass_singleton;

PRIVATE_NAMESPACE_END
