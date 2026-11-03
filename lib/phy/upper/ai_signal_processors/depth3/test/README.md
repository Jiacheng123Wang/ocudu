# depth3/test — contract tests

This directory pins the depth-3 **contract**: how a capture becomes the packed plane, how coded bits are
scattered into the label plane, and what the shipped bundles actually declare. Every test is pure Python
plus numpy — **no** TensorFlow, Core ML, radio or captures — so they run anywhere the project builds,
including machines without Apple silicon.

## The three tests

| File | What it pins | Why it is worth pinning |
|---|---|---|
| `test_plane_contract.py` | `pack_input` ↔ `unpack_input` round trip: plane shape and finiteness, a positive reference amplitude, the plane's DM-RS mask matching the sequence that was subtracted, the grid recovered by scaling back by `r` matching the input on every allocated subcarrier, and the pattern surviving in the metadata | Packing is where the grid, the sequence, the allocation and the metadata meet; once the two sides drift apart everything still runs and every number is quietly wrong |
| `test_label_plane.py` | For both DM-RS patterns: bits land only at `symbol * 8 + bit`, `llr_gather_cond` is its exact inverse, `llr_valid_mask_cond` covers exactly the scattered positions, and **2,7,11 carries fewer data REs than 2,11** (864 versus 792) | The field failure was "the pattern the label assumes is not the pattern the reception has" (design §19.211). A regression that reused one pattern's mask would be caught by nothing else |
| `test_shipped_bundle_contract.py` | The default bundle declares `layout=modcond`, `nsc=612`, `head_stride=8`, `qm_set=2,4,6,8`, `snr_range=12,48` **and a calibration** (per modulation: slope 0.8–1.6, offset −8..+3); the rollback bundle declares the same contract but **no** calibration | A missing declaration, or a calibration with the **sign inverted**, still loads and only fails in the field (§46.13ter: the noise-space offset declared as-is raised a reception published at 17.07 dB to 20.83, and attach failed on every leg that carried it) |

## Running them

```bash
# Directly (77 means "dependencies missing", which CTest records as a skip, not a failure)
python3 lib/phy/upper/ai_signal_processors/depth3/test/test_label_plane.py

# Or through the project's test suite
cd build && ctest -R ai_signal_processors_depth3 -V
```

Registered in `tests/unittests/ai_signal_processors/depth3/CMakeLists.txt`
(`SKIP_RETURN_CODE 77`, `TIMEOUT 120`, `LABELS ai_signal_processors;depth3;python`).

## What is deliberately not here

The heavier siblings stay in `doc_chinese/llr_ai_detection/probe/`, because they need a radio or Core ML
and do not belong in a unit-test run:

| Tool | Needs |
|---|---|
| `attach_ab.py`, `leg_replay.sh`, `eval_narrow_c32.sh` | Capture corpus, a compiled bundle, Core ML (macOS) |
| `cell_report.py`, `bit_agreement.py`, `reporting_range.py`, `arms_summary.py` | Only CSVs, but they serve the live A/B and their input comes from flights |
| `fly_arm.sh`, `capture_attach.sh`, `fit_sinr_cal.sh` | A radio and the flight environment |

The design document named in `depth3/README.md` is the authority for those tools and their criteria.

## The fourth test, added 2026-11-03

`test_control_bearing_decode.py` pins the **decoder** side of the same contract the other three pin on
the plane side: a codeword that carries uplink control decodes, and the wrong element map is refused.

It exists because the coded-BLER path raised on the first control-bearing sample of a sixty-epoch
run, and that raise was the harmless outcome. A HARQ-ACK of one or two bits **punctures** its reserved
elements rather than shortening the code -- `nof_ul_sch_bits` keeps them (`lib/ran/pusch/ulsch_info.cpp`)
and the demultiplexer **zeroes** their soft bits (`ocuduvec::zero` in `ulsch_demultiplex_impl.cpp`) --
so `nof_bits` is the number of coded bits that were on air and the codeword is longer. A decoder that
treats the two as the same number reads a plausible BLER instead of failing.

It decodes one sample from each family (two-bit ACK: eight punctured elements; four-bit ACK: none) with
perfect LLRs and requires zero block errors, then requires a plane gathered over the whole allocation to
be refused. Mutating the erasure insertion out, or the comparison back to all bits, makes it fail.
Still pure Python plus numpy: it imports `coded_bler.py`, which is why that module is separate from the
trainer (design section 28).
