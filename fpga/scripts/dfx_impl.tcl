# SPDX-License-Identifier: Apache-2.0
# Loom — Full DFX implementation (first-time build)
#
# Loads static_synth.dcp + rm_synth.dcp, defines pblock, runs full P&R.
# Outputs:
#   static_routed.dcp  — locked static checkpoint (reused for all future RM builds)
#   full.bit           — full bitstream (program to SPI flash for persistence)
#   rm_partial.bit     — partial bitstream for the initial RM (JTAG load)
#
# RM_NAME defaults to "rm".

source [file join [file dirname [info script]] reports.tcl]

set work_dir  $::env(WORK_DIR)
set run_dir   $::env(LOOM_RUN_DIR)
set board_dir $::env(BOARD_DIR)
set rm_name   [expr {[info exists ::env(RM_NAME)] ? $::env(RM_NAME) : "rm"}]

# Open static synthesis checkpoint
open_checkpoint $work_dir/results/static_synth.dcp

# Load the RM into the reconfigurable cell
read_checkpoint -cell u_emu_top $work_dir/results/${rm_name}_synth.dcp

# DFX pblock — defines RP area and marks cell as reconfigurable
read_xdc $board_dir/u250_dfx.xdc

# Implementation-only constraints
read_xdc $board_dir/u250_implementation.xdc

# ----------------------------------------------------------------
# Implementation
# ----------------------------------------------------------------
opt_design
place_design -directive ExtraNetDelay_high
phys_opt_design
route_design -directive AggressiveExplore
phys_opt_design -directive AggressiveExplore

# ----------------------------------------------------------------
# Reports
# ----------------------------------------------------------------
loom::reports_impl $run_dir dfx_impl

# ----------------------------------------------------------------
# Outputs
# ----------------------------------------------------------------
file mkdir $run_dir/results

# Locked static checkpoint — reused for every future RM build
write_checkpoint -force $run_dir/results/static_routed.dcp

# Full bitstream — program to SPI flash for persistent shell
write_bitstream -force $run_dir/results/full.bit

# Partial bitstream for the initial RM
write_bitstream -force -cell u_emu_top $run_dir/results/${rm_name}_partial.bit

# Write design hash sidecar so loomx can verify the bitstream was accepted.
set manifest [file join [file dirname $::env(TRANSFORMED_V)] loom_manifest.toml]
if {[file exists $manifest]} {
    set fh [open $manifest r]
    set content [read $fh]
    close $fh
    if {[regexp {hash\s*=\s*"([0-9a-f]+)"} $content _ design_hash]} {
        set fh [open $run_dir/results/${rm_name}_partial.hash w]
        puts -nonewline $fh $design_hash
        close $fh
    }
}

# static_routed.dcp (written above) is the full routed design — it serves
# as the pr_verify baseline for all future dfx_rm builds.
