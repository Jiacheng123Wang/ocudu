# macOS test triage tools

Helper scripts for iterating `make test` on the macOS port of ocudu. They are part of the source tree
(`tests/ci/macos_triage`) so they survive `rm -rf build`; all run artefacts (logs, hung-test ledger, aggregated
results) are written to `<build>/macos_triage`, which is a generated directory.

## Scripts

| script | purpose |
|---|---|
| `scan.sh` | Runs `make test ARGS="-I <start>,"` from a given ctest index. Watches the ctest output stream: if nothing is printed for `stall_secs` (default 120) or one case runs longer than `max_test_secs` (default 600), the whole process tree is SIGKILLed, the hung case (from the last `Start <n>:` line) is recorded in `hung_tests.tsv`, and the scan exits with code 2 so it can be resumed at the next index after the case is fixed/skipped. When `make test` finishes, it prints the categorized summary (see below). |
| `postrun_summary.py` | Parses one scan log and prints the macOS summary line over the real total: `100% tests passed (7556), 34 tests disabled (not applicable on macOS), out of 7590 total`. The 34 merges the 10 ctest-disabled and the 24 runtime-skipped cases (all confirmed not applicable on macOS; they run on Ubuntu only) and is computed dynamically. ctest's own trailing summary is misleading here: it excludes Disabled from the denominator (printing e.g. "out of 7580" for a 7590-case suite), folds Crashed into the failed count and does not list Disabled cases at all. |
| `aggregate.py` | Merges all `run_from_*.log` files into `<build>/macos_triage/results.tsv` / `results.json` (one row per ctest index, later runs override earlier ones). |
| `make_summary.py` | Regenerates `SUMMARY.md` **in this directory** (versioned) from `results.json` plus curated metadata (parity restoration, fixed failure clusters, SCTP fixes, modified test sources). The per-case "not applicable on macOS" reasons are imported from `postrun_summary.py` (single source of truth). |
| `parse_ctest.py` | Shared ctest progress-line parser used by the scripts above. |
| `probe.sh` / `gtest_timeout.sh` | Re-run a single ctest case / gtest filter with a hard wall-clock cap, sampling the stacks on timeout: used to tell a true hang from a merely slow test. |
| `map_modified.py` | Maps modified test source files to the ctest cases they produce (used to compile the "modified for macOS" lists). |

## Documentation in this directory

| file | how it is maintained |
|---|---|
| `SUMMARY.md` | **Generated**: run `python3 tests/ci/macos_triage/aggregate.py` (rebuilds `results.json` from the scan logs in `build/macos_triage/logs`), then `python3 tests/ci/macos_triage/make_summary.py` (writes `SUMMARY.md` next to the script). The curated tables (parity, failure clusters, SCTP fixes, modified sources) live in `make_summary.py` and must be edited there, not in the .md file. |
| `triage_notes.md` | **Hand-written** notes: how the macOS test port differs from Ubuntu (with the platform-confinement table), the list of all tests that do not run on macOS (34, with per-case reasons), and the improvement TODOs. Update it manually after new findings. |

## Usage

```sh
cd build
../tests/ci/macos_triage/scan.sh 1            # full pass; or resume with the index after a hung case
../tests/ci/macos_triage/probe.sh <exact_ctest_name> [cap_secs]
../tests/ci/macos_triage/gtest_timeout.sh <binary> <gtest_filter> [cap_secs]
```

`scan.sh` honours `BUILD_DIR` (defaults to `<repo>/build`).

## Test-host prerequisites (macOS)

Linux routes the whole `127.0.0.0/8` to loopback, macOS only carries `127.0.0.1`. The gateway/f1u/cu_up/e2
tests bind other loopback addresses, so add the aliases once per boot:

```sh
sudo ifconfig lo0 alias 127.0.0.2 up
sudo ifconfig lo0 alias 127.0.0.3 up
sudo ifconfig lo0 alias 127.0.1.1 up
sudo ifconfig lo0 alias 127.0.0.101 up
```

The aliases do not survive a reboot. To re-add them automatically at boot, install the bundled
LaunchDaemon once (see `lo0_aliases/INSTALL.md`); without it, expect the gateway/f1u/cu_up tests to
fail with `Can't assign requested address` after every reboot (18 cases on the 2026-09-01 scan).

## SCTP transport mode (`OCUDU_USRSCTP_MODE`)

The usrsctp shim (`lib/gateways/sctp_socket.cpp`) selects the SCTP transport automatically per process:

* **auto** (default): native SCTP over IP when the process may open a raw socket (root), otherwise SCTP-over-UDP
  encapsulation (RFC 6951, port 9899+), which works unprivileged.
* **udp**: force SCTP-over-UDP encapsulation.
* **raw**: force native SCTP over IP (needs root; without it association attempts fail).

`sudo ctest -L sctp` without an override switches the suite to raw mode, and macOS does not loop native SCTP
packets back to a local raw socket, so the cases hang on the first association. Keep the unit tests in UDP mode:

```sh
sudo OCUDU_USRSCTP_MODE=udp ctest -L sctp
```

The raw (native SCTP over IP) mode is what the gnb uses to talk to Linux kernel-SCTP peers (AMF, RIC); it cannot
be validated on macOS loopback and is covered by the end-to-end setup instead. `postrun_summary.py` reports which
mode a scan ran in and prints the same guidance after every `make test`.

See `triage_notes.md` (same directory) for the port differences, the 34 not-run cases and the improvement TODOs.
