# tools/ — this workflow's development tools

**Rule (user, 2026-11-06): tools live here, data lives in `~/ai_depth3/`.**

`~/ai_depth3/` holds the large, numerous artefacts — captures, datasets, dumps, bundles, logs — and a directory of twenty
thousand files is not a place to keep scripts: it makes them slow to grep and impossible to version. Everything that is
*code* belongs in this directory, in git, next to the documents that describe it.

## What is here

| tool | what it does |
|---|---|
| `fly.sh` | fly one bundle: **console and file at once** (`script` gives the app a pty so it line-buffers, `tee` records it), logs under `~/ai_depth3/flight_logs/<leg>/<model>_<stamp>.log`; arguments `[model] [config] [leg] [seconds]` |
| `fly_attach.sh` | the same for an attach-focused run, timing it and summarising the attach markers |
| `probe_snapshot.sh` | one snapshot to a probe score: export, compile, calibrate, replay, score (2–3 min) |
| `probe_watch.sh` | run the probe on a snapshot as soon as its training run produces one |
| `measure_arm.py` | one arm over a unit list against a corpus and a bundle, in the air chain's pipeline vocabulary |
| `measure_abs.py` | the same with an absolute bundle path (for probe-built bundles outside `bundles/`) |
| `measure_full.py` | the same over the full leg with progress lines and an ETA |
| `measure_arm_203.py` | the 203-flavoured copy whose corpus and interpreter paths differ |
| `narrow_models.py` | the narrow-band table (≤3 PRB) for a list of bundles — the attach-capability proxy |
| `plan.swift` | dumps Core ML's per-operation device placement and estimated cost for a compiled model |
| `backup_s2_to_211.sh` | archive each capture leg to 211 as its own tar, with the byte comparison that caught the `wc` padding bug |

## Hosts

The scripts take their data paths from the environment (`PY`, `CORPUS`, `BIN`, `UNITS_FILE`, `CPU_REF`) and the probe
sources `~/ai_depth3/bin/probe_env.sh` when it exists, so one copy serves both machines. `~/ai_depth3/bin/*` are thin
wrappers that exec the copies here, which keeps the old invocation paths working while the source of truth stays in git.

**The probe's CPU reference is a constant, not a csv (changed 2026-11-07).** `probe_snapshot.sh` used to print its
delta against a per-unit `clean_cpu.csv`; that file is the *ruler's* reference for the Metal front end, so slicing it to
the probe's 500 units yielded 930 016 bit — a reference 19% weaker than the CPU chain the AI arm replaces (the dump's
chain, **1 149 352** on the same units). Both probe baselines were read against it before that was noticed, so the file
was deleted (it was byte-identical to the `ruler_s2_amc_5000_clean_cpu.csv` archive, sha `17300b0b…`, which remains) and
the script now prints `cpu_ref 1,149,352` with its provenance, `UNITS_SHA` and an arm-only bootstrap interval.
Override with `CPU_REF=<bits>` only when the unit set itself changes, and say so in the log.

## fly.sh

```bash
fly.sh <leg>                                  # build's default bundle, pinned_mcs04.yml, unlimited
fly.sh <leg> <config.yml>                     # a specific air configuration
fly.sh <leg> <config.yml> <model>             # a specific bundle
fly.sh <leg> <config.yml> <model> 240         # stop after 240 s with ONE SIGINT
```
**The leg comes first and is the only required argument**, because every flight has one and it decides where the log goes;
everything after it has a default.
Logs land in `~/ai_depth3/flight_logs/<leg>/`, so every leg of a campaign keeps its own directory.

**Stop a flight with ONE Ctrl-C and wait for `Stopping...`.** The phase latencies and the block table are printed at
shutdown; a second Ctrl-C kills the process before it reports, and the log then holds a flight with no measurements. The
script checks for `[ul_pipeline]` afterwards and warns when it is missing, because a silent log is indistinguishable from a
run that measured nothing.
