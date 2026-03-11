// SPDX-License-Identifier: Apache-2.0
// Loom DPI Service Implementation

#include "loom_dpi_service.h"
#include "loom_log.h"

namespace loom {

static Logger logger = make_logger("dpi");

// ============================================================================
// DpiService Implementation
// ============================================================================

void DpiService::register_func(int func_id, std::string_view name, int n_args,
                                int ret_width, int out_arg_words, bool call_at_init,
                                bool read_only, DpiCallback callback) {
    funcs_.push_back({
        .func_id = func_id,
        .name = std::string(name),
        .n_args = n_args,
        .ret_width = ret_width,
        .out_arg_words = out_arg_words,
        .call_at_init = call_at_init,
        .read_only = read_only,
        .callback = std::move(callback)
    });
    logger.debug("Registered function '%.*s' (id=%d, %d args, %d-bit return, %d out words, init=%d, ro=%d)",
              static_cast<int>(name.size()), name.data(), func_id, n_args, ret_width, out_arg_words, call_at_init, read_only);
}

const DpiFunc* DpiService::find_func(int func_id) const {
    for (const auto& func : funcs_) {
        if (func.func_id == func_id) {
            return &func;
        }
    }
    return nullptr;
}

const DpiFunc* DpiService::find_func_by_id(int func_id) const {
    return find_func(func_id);
}

int DpiService::drain_fifo(Context& ctx) {
    if (ctx.fifo_entry_words() == 0)
        return 0;  // No FIFO in this design

    int drained = 0;
    while (true) {
        auto empty = ctx.fifo_is_empty();
        if (!empty.ok()) {
            if (empty.error() == Error::Shutdown)
                return static_cast<int>(Error::Shutdown);
            logger.error("FIFO status read failed");
            return -1;
        }
        if (empty.value())
            break;  // FIFO empty

        // Pop entry: read data words, then write pop command
        auto entry = ctx.fifo_pop_entry();
        if (!entry.ok()) {
            if (entry.error() == Error::Shutdown)
                return static_cast<int>(Error::Shutdown);
            logger.error("FIFO pop failed");
            return -1;
        }

        const auto& words = entry.value();
        if (words.empty()) break;

        // Parse: func_id in word[0][7:0], args in word[1..N-1]
        int func_id = words[0] & 0xFF;
        const DpiFunc* func = find_func(func_id);
        if (!func) {
            logger.error("FIFO: unknown function ID %d", func_id);
            error_count_++;
            drained++;
            continue;
        }

        if (!func->callback) {
            logger.error("FIFO: no callback for '%s' (id=%d)", func->name.c_str(), func_id);
            error_count_++;
            drained++;
            continue;
        }

        // Build args from FIFO words [1..N-1]
        std::vector<uint32_t> args(ctx.max_dpi_args(), 0);
        for (uint32_t w = 1; w < words.size() && (w - 1) < args.size(); w++) {
            args[w - 1] = words[w];
        }

        std::span<const uint32_t> args_span(args.data(), args.size());
        std::vector<uint32_t> out_args_buf;  // empty for read-only
        std::span<uint32_t> out_args(out_args_buf);
        func->callback(args_span, out_args);

        if (drained < 20 || (drained % 10000 == 0)) {
            logger.debug("FIFO[%d] '%s' drained#%d", func_id, func->name.c_str(), drained);
        }

        drained++;
        call_count_++;
    }

    return drained;
}

int DpiService::service_once(Context& ctx) {
    // Set context so user DPI functions can call vpi_control etc.
    current_ctx_ = &ctx;

    // Drain FIFO before servicing regfile calls (preserves ordering)
    int fifo_rc = drain_fifo(ctx);
    if (fifo_rc == static_cast<int>(Error::Shutdown))
        return fifo_rc;

    // Poll for pending DPI calls
    auto poll_result = ctx.dpi_poll();
    if (!poll_result.ok()) {
        if (poll_result.error() == Error::Shutdown) {
            return static_cast<int>(Error::Shutdown);
        }
        logger.error("Poll failed");
        return static_cast<int>(poll_result.error());
    }

    uint32_t pending_mask = poll_result.value();
    if (pending_mask == 0) {
        return 0;  // No pending calls
    }

    int serviced = 0;

    // Service each pending call
    for (size_t func_id = 0; func_id < funcs_.size() && func_id < 32; func_id++) {
        if (!(pending_mask & (1u << func_id))) {
            continue;
        }

        const DpiFunc* func = find_func(static_cast<int>(func_id));
        if (!func) {
            logger.error("Unknown function ID: %zu", func_id);
            ctx.dpi_error(static_cast<uint32_t>(func_id));
            error_count_++;
            continue;
        }

        if (!func->callback) {
            logger.error("No callback for function '%s' (id=%d)",
                      func->name.c_str(), func->func_id);
            ctx.dpi_error(static_cast<uint32_t>(func_id));
            error_count_++;
            continue;
        }

        // Get call details
        auto call_result = ctx.dpi_get_call(static_cast<uint32_t>(func_id));
        if (!call_result.ok()) {
            if (call_result.error() == Error::Shutdown) {
                return static_cast<int>(Error::Shutdown);
            }
            logger.error("Failed to get call for '%s'", func->name.c_str());
            error_count_++;
            continue;
        }

        const DpiCall& call = call_result.value();

        // Call the user function.
        // Pass all arg register words — the wrapper indexes by hardware
        // offset, not logical argument count.
        std::span<const uint32_t> args(call.args.data(), call.args.size());
        // Allocate buffer for output open array data
        std::vector<uint32_t> out_args_buf(func->out_arg_words, 0);
        std::span<uint32_t> out_args(out_args_buf);
        uint64_t result = func->callback(args, out_args);

        // Log: first 20, then every 10000th
        if (call_count_ < 20 || (call_count_ % 10000 == 0)) {
            logger.debug("DPI[%zu] '%s' result=0x%llx out_words=%d call#%llu",
                func_id, func->name.c_str(), (unsigned long long)result,
                func->out_arg_words, (unsigned long long)call_count_);
        }

        // Write output open array data back to regfile arg registers
        for (int i = 0; i < func->out_arg_words; i++) {
            auto wr_result = ctx.dpi_write_arg(static_cast<uint32_t>(func_id), i, out_args_buf[i]);
            if (!wr_result.ok()) {
                logger.error("Failed to write output arg %d for '%s'", i, func->name.c_str());
                error_count_++;
                break;
            }
        }

        // Complete the call
        auto complete_result = ctx.dpi_complete(static_cast<uint32_t>(func_id), result);
        if (!complete_result.ok()) {
            if (complete_result.error() == Error::Shutdown) {
                return static_cast<int>(Error::Shutdown);
            }
            logger.error("Failed to complete call for '%s'", func->name.c_str());
            error_count_++;
            continue;
        }

        serviced++;
        call_count_++;
    }

    return serviced;
}

DpiExitCode DpiService::run(Context& ctx, int /*timeout_ms*/) {
    current_ctx_ = &ctx;

    bool use_irq = (mode_ == DpiMode::Interrupt) && ctx.has_irq_support();
    logger.info("Entering service loop (n_funcs=%zu, mode=%s)",
                funcs_.size(), use_irq ? "interrupt" : "polling");

    while (true) {
        // --- Wait for interrupt (interrupt mode only) ---
        if (use_irq) {
            auto irq = ctx.wait_irq();
            if (!irq.ok()) {
                if (irq.error() == Error::Shutdown) {
                    logger.info("Shutdown received");
                    current_ctx_ = nullptr;
                    return DpiExitCode::Shutdown;
                }
                if (irq.error() == Error::Interrupted) {
                    continue;  // Signal received, re-check loop condition
                }
                logger.error("wait_irq failed");
                current_ctx_ = nullptr;
                return DpiExitCode::Error;
            }
        }

        // --- Service all pending DPI calls ---
        int total_serviced = 0;
        while (true) {
            int rc = service_once(ctx);
            if (rc == static_cast<int>(Error::Shutdown)) {
                logger.info("Shutdown received");
                current_ctx_ = nullptr;
                return DpiExitCode::Shutdown;
            }
            if (rc < 0) {
                current_ctx_ = nullptr;
                return DpiExitCode::Error;
            }
            if (rc == 0) break;  // No more pending calls
            total_serviced += rc;
        }

        // --- Check emulation state ---
        auto state_result = ctx.get_state();
        if (!state_result.ok()) {
            if (state_result.error() == Error::Shutdown) {
                logger.info("Shutdown received");
                current_ctx_ = nullptr;
                return DpiExitCode::Shutdown;
            }
            current_ctx_ = nullptr;
            return DpiExitCode::Error;
        }

        if (state_result.value() == State::Error) {
            logger.error("Emulation error state");
            current_ctx_ = nullptr;
            return DpiExitCode::EmuError;
        }
        if (state_result.value() == State::Frozen) {
            // Final FIFO drain to flush any remaining entries
            drain_fifo(ctx);
            logger.info("Emulation frozen, test complete");
            current_ctx_ = nullptr;
            return DpiExitCode::Complete;
        }
    }
}

void DpiService::print_stats() const {
    logger.info("Statistics:");
    logger.info("  Total calls serviced: %llu", static_cast<unsigned long long>(call_count_));
    logger.info("  Errors: %llu", static_cast<unsigned long long>(error_count_));
    logger.info("  Registered functions: %zu", funcs_.size());
    for (const auto& func : funcs_) {
        logger.info("    [%d] %s (%d args, %d-bit return)",
                 func.func_id, func.name.c_str(), func.n_args, func.ret_width);
    }
}

// Global instance
DpiService& global_dpi_service() {
    static DpiService instance;
    return instance;
}

} // namespace loom
