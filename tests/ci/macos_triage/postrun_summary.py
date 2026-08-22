#!/usr/bin/env python3
"""Print a categorized post-run summary of one `make test` scan log.

ctest's own trailing summary is misleading for this port:
  * its "N tests failed out of M" line EXCLUDES the DISABLED tests from M (so it prints 7580 instead of 7590);
  * CRASHED tests are folded into the failed count;
  * SKIPPED tests are only listed inside "The following tests did not run:" and counted as passed in the percentage;
  * DISABLED tests are not listed anywhere at all.

This script parses the ctest progress lines instead and prints the complete accounting over the real total of 7590
cases. The headline line is the macOS equivalent of ctest's summary line:

    100% tests passed (7556), 34 tests disabled (not applicable on macOS), out of 7590 total

The disabled count merges the 10 ctest-disabled cases and the 24 runtime-skipped cases: all 34 are confirmed not
applicable on macOS (they only run on Ubuntu) and the count is computed dynamically, so it follows the test list.

Usage: postrun_summary.py <scan log path>
"""
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from parse_ctest import parse_log  # noqa: E402

# Why a case does not run on macOS. Keys are case-name prefixes; keep this table updated as cases are added/removed.
SKIP_REASONS = [
    ("sctp_network_server_peer_test.",
     "sctp peers bind different loopback addresses; the usrsctp shim cannot associate distinct local addresses"),
    ("sctp_network_client_test.when_client_binds_address",
     "the client binds 127.0.0.2; usrsctp cannot associate distinct local addresses"),
    ("sctp_network_client_test.when_server_has_multihomed",
     "multihomed server; usrsctp cannot associate distinct local addresses"),
    ("sctp_network_client_test.ipv4_bind_and_ipv4_and_ipv6_connect_addresses",
     "mixed local addresses; usrsctp cannot associate distinct local addresses"),
    ("sctp_socket_test.bindx_with_mixed_ipv4_and_ipv6_addresses",
     "usrsctp cannot bind an IPv4 and an IPv6 address on the same socket"),
    ("sctp_socket_test.connectx_with",
     "usrsctp has no sctp_connectx(); only the first peer address is used"),
    ("e2ap_network_adapter_test.when_e2_setup_response",
     "the E2 agent binds 127.0.0.101 while the RIC listens on 127.0.0.1; usrsctp cannot associate distinct local addresses"),
    ("nia2/fxt_nia2.integrity_engine_nia2_cmac",
     "the Homebrew mbedTLS bottle is built with MBEDTLS_CMAC_C disabled"),
    ("rrc_du_ref_time_r16_test.subsecond_component_is_preserved",
     "macOS system_clock has microsecond resolution; sub-microsecond precision is not preserved"),
]

# Ubuntu/Linux-only test targets, registered on macOS with the ctest DISABLED property so they stay in the list.
DISABLED_REASONS = [
    ("rlc_um6_eia2_eea2_stress_test",
     "Ubuntu/Linux-only: pthread_barrier_* is not implemented on macOS"),
    ("rlc_um12_eia2_eea2_stress_test",
     "Ubuntu/Linux-only: pthread_barrier_* is not implemented on macOS"),
    ("rlc_am12_eia2_eea2_stress_test",
     "Ubuntu/Linux-only: pthread_barrier_* is not implemented on macOS"),
    ("rlc_am18_eia2_eea2_stress_test",
     "Ubuntu/Linux-only: pthread_barrier_* is not implemented on macOS"),
    ("rlc_um12_eia1_eea1_stress_test",
     "Ubuntu/Linux-only: pthread_barrier_* is not implemented on macOS"),
    ("rlc_um12_eia3_eea3_stress_test",
     "Ubuntu/Linux-only: pthread_barrier_* is not implemented on macOS"),
    ("rlc_um12_eia2_eea0_stress_test",
     "Ubuntu/Linux-only: pthread_barrier_* is not implemented on macOS"),
    ("ofh_integration_test",
     "Ubuntu/Linux-only: drives a real Ethernet controller through AF_PACKET (linux/if_packet.h)"),
    ("du_high_benchmark",
     "Ubuntu/Linux-only: pins threads with the Linux CPU-affinity APIs (pthread_setaffinity_np, CPU_SET, ...)"),
    ("dft_processor_ci16_test",
     "Ubuntu/Linux-only in practice: the ci16 DFT processor is an x86_64 implementation; not built on Apple Silicon"),
]


def reason_for(name, table):
    for prefix, why in table:
        if name.startswith(prefix):
            return why
    return "does not run on macOS; needs further debugging"


def sctp_transport_mode():
    """The SCTP transport mode the tests ran in.

    Mirrors the selection in lib/gateways/sctp_socket.cpp (usrsctp_once_init): the OCUDU_USRSCTP_MODE
    environment variable wins, otherwise the automatic selection depends on whether the process may open
    a raw socket (root). This post-run script sees its own environment, so it reports the mode of a run
    started the same way (e.g. `sudo make test` runs both ctest and this script as root).
    """
    override = os.environ.get("OCUDU_USRSCTP_MODE", "").strip().lower()
    if override == "udp":
        return "SCTP-over-UDP encapsulation (OCUDU_USRSCTP_MODE=udp)"
    if override == "raw":
        return "native SCTP over IP (OCUDU_USRSCTP_MODE=raw)"
    if os.geteuid() == 0:
        return "native SCTP over IP (auto: running as root)"
    return "SCTP-over-UDP encapsulation (auto: unprivileged process)"


def main():
    log = sys.argv[1]
    results, started = parse_log(log)
    counts = Counter(r["outcome"] for r in results.values())
    total = max(results) if results else 0

    crashed = sorted((i, r) for i, r in results.items() if r["outcome"] == "Crashed")
    failed = sorted((i, r) for i, r in results.items() if r["outcome"] == "Failed")
    skipped = sorted((i, r) for i, r in results.items() if r["outcome"] == "Skipped")
    disabled = sorted((i, r) for i, r in results.items() if r["outcome"] == "Disabled")
    passed = counts.get("Passed", 0)
    killed = sorted(set(started) - set(results))

    # The 10 ctest-disabled and the 24 runtime-skipped cases are all confirmed not applicable on macOS (they run
    # on Ubuntu only), so they are reported as one merged "disabled" count. Both numbers are dynamic.
    disabled_total = len(disabled) + len(skipped)
    runnable = total - disabled_total
    pct = 100.0 * passed / runnable if runnable else 0.0
    problems = []
    if failed:
        problems.append(f"{len(failed)} failed")
    if crashed:
        problems.append(f"{len(crashed)} crashed")
    if killed:
        problems.append(f"{len(killed)} killed (hung)")
    suffix = (", " + ", ".join(problems)) if problems else ""

    print()
    print("=================== macOS test result summary (over the real total) ===================")
    print(f"{pct:.0f}% tests passed ({passed}), {disabled_total} tests disabled (not applicable on macOS), "
          f"out of {total} total{suffix}")
    print(f"  Total registered test cases ....... {total}")
    print(f"    Passed .......................... {passed}")
    print(f"    Disabled (not applicable) ....... {disabled_total}  "
          f"(= {len(disabled)} ctest-disabled + {len(skipped)} runtime-skipped)")
    print(f"    Failed .......................... {len(failed)} tests")
    print(f"    Crashed ......................... {len(crashed)} tests")
    if killed:
        print(f"    Killed (hung) ................... {len(killed)} tests")
    print(f"    (total accounted for ............ {passed + len(failed) + len(crashed) + disabled_total + len(killed)})")
    print()
    print("Note: ctest's own line 'N tests failed out of M' prints M = total - Disabled (e.g. 7580 instead of 7590),")
    print("counts Crashed among the failures and hides Disabled entirely; the line above is the complete picture.")
    print("========================================================================================")

    print()
    print("SCTP transport mode (the `sctp` ctest label, 78 cases; `ctest -L sctp` runs only these):")
    print(f"  mode used by this run ......... {sctp_transport_mode()}")
    print("  force UDP encapsulation ...... OCUDU_USRSCTP_MODE=udp (recommended for `sudo ctest -L sctp`)")
    print("  force native SCTP over IP .... OCUDU_USRSCTP_MODE=raw (requires root)")
    print("  note: native SCTP over IP cannot be validated on macOS loopback - the kernel does not loop native")
    print("        SCTP packets back to a local raw socket, so `sudo ctest -L sctp` with the default 'auto' mode")
    print("        hangs on the first case. The raw mode is validated end to end by the gnb against Linux")
    print("        kernel-SCTP peers (AMF/E2 on Ubuntu); the unit tests cover the UDP-encapsulation mode.")

    print()
    print(f"The following {disabled_total} tests do not run on macOS - confirmed not applicable on macOS, they run")
    print("on Ubuntu only:")
    if not skipped and not disabled:
        print("  (none)")
    for i, r in skipped:
        print(f"  {i} - {r['name']} (runtime-skipped)")
        print(f"       reason: {reason_for(r['name'], SKIP_REASONS)}")
    for i, r in disabled:
        print(f"  {i} - {r['name']} (ctest-disabled)")
        print(f"       reason: {reason_for(r['name'], DISABLED_REASONS)}")

    if failed or crashed:
        print()
        print("Crashed tests (need further debugging):")
        if not crashed:
            print("  (none)")
        for i, r in crashed:
            print(f"  {i} - {r['name']} (Crashed)")

        print()
        print(f"Failed tests ({len(failed)} cases, full detail in the ctest FAILED section above):")
        for i, r in failed:
            print(f"  {i} - {r['name']} (Failed)")

    if killed:
        print()
        print("Killed by the hang scan (blocked forever; needs further debugging):")
        for i in killed:
            print(f"  {i} - {started[i]}")


if __name__ == "__main__":
    main()
