# Measurement outputs (ruler and full leg)

These are the per-unit CSVs behind every ruler and full-leg number in the PK and delivery documents. They used to live
in `/tmp` on the machines that ran the replay, which is cleared on reboot; the copies here are the record.

| File pattern | What it is |
|---|---|
| `ruler_<tag>.csv` | 5 000-unit ruler run of `<tag>` (columns: unit, status, bits) |
| `ruler_*_203.csv` | the same, measured on the 203 box |
| `full_<tag>.csv` | 50 000-unit full leg (same columns) |

## How a ruler is measured

    python3 ai_depth3/measure_arm.py ai_depth3/ruler_s2_amc_5000_units.txt <bundle>/<tag>.mlmodelc <out>.csv

A bundle is produced from a `.keras` weight file by

    train_depth3.py --export-only <model>.keras --export-probe <out>.mlpackage \\
        --modcond --layout spatial --c-hid 32 --blocks 18 --dense-blocks [model flags] --snr-range 0,48
    xcrun coremlcompiler compile <out>.mlpackage <out>/

## The weight files the CSVs refer to

`../models/` holds the composites that back the numbers in the documents (they are kept out of git by `models/.gitignore`
because they are megabytes each and are reproducible from the run checkpoints):

| Model | Recipe |
|---|---|
| `A_swa_tail.keras`, `A_ema08.keras` | `--mlp-expand 2` run, tail average of e37-e40 / EMA(0.8) over e1-e40 |
| `b16_swa_tail.keras` | clean batch-16 llrp run, average of e38-e40 |
| `symaux_swa_tail.keras`, `symaux_ema08_v2.keras` | symbol-head run, average of e38-e40 / EMA(0.8) |
| `126_swa_e20.keras`, `107_swa_e20.keras`, `s1_swa_e20.keras` | epoch-20 averages of the three arms |
| `c128b_swa30.keras`, `c128l_swa30.keras` | c128 without/with llrp, average of e28-e30 |
| `ema_decay_b0.6/0.8/0.9.keras` | EMA sweep over the s2 decay continuation |

Rebuild any of them with `../analysis/avg_weights.py` (flat average) or `../analysis/ema_scan.py` (EMA), from the
checkpoints in `<machine>:~/work/ai_phy_train/runs/<run>/snapshots/`.
