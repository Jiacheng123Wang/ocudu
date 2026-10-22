# Narrow-band buckets for `d3_modcond_c32h`

Eight compiled Core ML graphs of **the same weights** at eight input widths, so a reception is computed over the window its
allocation needs instead of the whole 51-PRB band. The trunk is fully convolutional, which is why one weight set builds at
any width; cropping the plane to the allocation plus four PRB of context reproduces the full-band outputs **bit for bit**
(measured: 100.00 % sign agreement, zero relative difference — `model_network_design.md` section 49).

| Width (subcarriers) | PRB this covers (`12·prb + 96 ≤ width`) | Compute vs 612 |
|---|---|---|
| 108 | up to 1 | 5.7× |
| 180 | 2–7 | 3.4× |
| 252 | 8–13 | 2.4× |
| 324 | 14–19 | 1.9× |
| 396 | 20–25 | 1.55× |
| 468 | 26–31 | 1.31× |
| 540 | 32–37 | 1.13× |
| 612 | 38–51 | 1.00× |

**Default behaviour**: the arm binds this directory automatically (sibling `buckets/` of whatever bundle it was given) and
uses the smallest width that fits. `OCUDU_DEPTH3_BUCKETS=none` turns bucketing off and uses only the full-band bundle;
`OCUDU_DEPTH3_BUCKETS=<a>,<b>,…` overrides the set with explicit paths.

**Measured on air** (2026-10-23, same cell, same handset, 300 s each): `forward p50` **820 µs → 457 µs** (−44 %) with
throughput 18.9 → 19.3 Mbit/s and CRC 99.0 % → 99.95 %, both inside run-to-run variation.

**Why eight files and not one.** Core ML compiles one graph per input shape, and the head of this graph reshapes by the
static width, so an enumerated-shape export would need a width-dynamic graph first. That is a follow-up, not a blocker: the
selector already treats the set as a set.
