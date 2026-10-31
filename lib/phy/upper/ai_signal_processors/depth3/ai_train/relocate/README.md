# Relocating dataset files that were written to the wrong directory

Three builds on the other machine were started before the queue script's output directory became a real setting, so their arrays land in `datasets_bin` -- the
sampled set, which is supposed to hold only quota-limited files -- instead of `datasets_full_w2`. The fix is to move them once each build finishes.

- `move_leg.sh <leg>` runs on the machine that has the files. For every `w2_harq_<leg>_*.npz` in `datasets_bin` it checks that the fresh file is a loadable
  archive with the required keys and that it is not smaller than a file of the same name already in the destination; if a replacement happens, the old file is
  moved to `datasets_full_w2_replaced_fragments/` rather than deleted.
- `check_npz.py <file>` prints one line per file: loadable or not, its shapes, and whether every required key is present.
- `monitor_misplaced.sh` polls every five minutes from the Mac, moves a leg once its build process is gone and its output is freshly written, then runs the
  destination load check and the full audit. It is idempotent and can be restarted at any time.

The gating on a fresh modification time is deliberate: stale leftovers from a previous day sit in the same directory with the same names, and only the
timestamp distinguishes them from the output of the run that just finished.
