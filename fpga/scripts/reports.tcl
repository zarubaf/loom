# SPDX-License-Identifier: Apache-2.0
# Loom — shared TCL helpers
#
# Procs (source this file, then call):
#   loom::read_ips               — read all Loom IP cores
#   loom::read_shell_rtl         — read static-shell RTL (no DUT)
#   loom::read_rm_rtl            — read RM RTL (no shell, no DUT)
#   loom::reports_synth $run_dir $pfx  — post-synthesis reports
#   loom::reports_impl  $run_dir $pfx  — post-route reports

namespace eval loom {

proc read_ips {} {
    set ip_dir $::env(IP_DIR)
    read_ip $ip_dir/xlnx_xdma/xlnx_xdma.srcs/sources_1/ip/xlnx_xdma/xlnx_xdma.xci
    read_ip $ip_dir/xlnx_clk_gen/xlnx_clk_gen.srcs/sources_1/ip/xlnx_clk_gen/xlnx_clk_gen.xci
    read_ip $ip_dir/xlnx_cdc/xlnx_cdc.srcs/sources_1/ip/xlnx_cdc/xlnx_cdc.xci
    read_ip $ip_dir/xlnx_decoupler/xlnx_decoupler.srcs/sources_1/ip/xlnx_decoupler/xlnx_decoupler.xci
}

proc read_shell_rtl {} {
    set s $::env(LOOM_SRC)
    read_verilog -sv \
        $s/rtl/loom_axi4_err_slv.sv \
        $s/rtl/loom_axil_demux.sv \
        $s/rtl/loom_emu_ctrl.sv \
        $s/rtl/loom_dpi_regfile.sv \
        $s/rtl/loom_scan_ctrl.sv \
        $s/rtl/loom_axil_firewall.sv \
        $s/rtl/loom_icap_ctrl.sv \
        $s/rtl/loom_shell.sv
}

proc read_rm_rtl {} {
    set s $::env(LOOM_SRC)
    read_verilog -sv \
        $s/rtl/loom_axil_demux.sv \
        $s/rtl/loom_emu_ctrl.sv \
        $s/rtl/loom_dpi_regfile.sv \
        $s/rtl/loom_scan_ctrl.sv
}

# Print the timing closure result and the maximum achievable frequency.
# Call after route_design. Uses the worst setup path across all clocks.
proc report_achieved_freq {rpt_dir prefix} {
    catch {
        set path [lindex [get_timing_paths -max_paths 1 -nworst 1 -setup -quiet] 0]
        if {$path eq ""} {
            puts "report_achieved_freq: no timing paths found"
            return
        }
        set wns       [get_property SLACK $path]
        set start_pin [get_property STARTPOINT_PIN $path]
        set clk       [lindex [get_clocks -of_objects [get_pins $start_pin]] 0]
        set period    [get_property PERIOD $clk]
        set t_ach     [expr {$period - $wns}]
        set f_ach     [expr {1000.0 / $t_ach}]
        set met       [expr {$wns >= 0 ? "MET" : "NOT MET"}]
        puts "-----------------------------------------------------------"
        puts "Timing $met  |  clock: $clk  |  target: [format %.3f $period] ns ([format %.1f [expr {1000.0/$period}]] MHz)"
        puts "             |  WNS: [format %+.3f $wns] ns  ->  achievable: [format %.3f $t_ach] ns ([format %.1f $f_ach] MHz)"
        puts "-----------------------------------------------------------"
        set f [open $rpt_dir/${prefix}_freq.txt w]
        puts $f "target_period_ns   [format %.3f $period]"
        puts $f "target_freq_mhz    [format %.1f [expr {1000.0/$period}]]"
        puts $f "wns_ns             [format %+.3f $wns]"
        puts $f "achieved_period_ns [format %.3f $t_ach]"
        puts $f "achieved_freq_mhz  [format %.1f $f_ach]"
        puts $f "timing_met         [expr {$wns >= 0 ? 1 : 0}]"
        close $f
    } err
    if {$err ne ""} { puts "report_achieved_freq: $err (non-fatal)" }
}

# Lightweight mid-implementation checkpoint: timing summary + utilization.
# Call after opt_design / place_design / phys_opt_design with a stage suffix,
# e.g.: loom::reports_stage $run_dir $rm_name place
proc reports_stage {run_dir prefix stage} {
    set d $run_dir/reports
    file mkdir $d
    report_timing_summary -warn_on_violation \
                          -file $d/${prefix}_timing_${stage}.rpt
    report_utilization    -file $d/${prefix}_utilization_${stage}.rpt
}

proc reports_synth {run_dir prefix} {
    set d $run_dir/reports
    file mkdir $d
    report_utilization    -file $d/${prefix}_utilization.rpt
    report_timing_summary -warn_on_violation \
                          -file $d/${prefix}_timing.rpt
}

proc reports_impl {run_dir prefix} {
    set d $run_dir/reports
    file mkdir $d
    report_achieved_freq $d $prefix
    report_utilization         -file $d/${prefix}_utilization.rpt
    report_timing_summary      -warn_on_violation \
                               -file $d/${prefix}_timing.rpt
    report_timing              -warn_on_violation -nworst 20 -path_type full \
                               -slack_lesser_than 0 \
                               -file $d/${prefix}_timing_paths.rpt
    report_clock_interaction   -file $d/${prefix}_clock_interaction.rpt
    report_methodology         -file $d/${prefix}_methodology.rpt
    report_drc                 -file $d/${prefix}_drc.rpt
    report_route_status        -file $d/${prefix}_route_status.rpt
}

} ;# namespace loom
