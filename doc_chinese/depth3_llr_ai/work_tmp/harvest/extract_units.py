"""Extract only the units a harvest needs, straight out of a leg tarball.

WHY NOT JUST UNTAR THE LEG. A leg is 100k-200k small files; extracting all of them costs minutes of random I/O per leg and
fills the worker's disk with data the harvest will not touch. The harvest only replays units the previous dump marked
crc=KO, so this reads the tar once, sequentially, and writes the four files of each wanted unit (.txt, .bin, _h.bin,
_ce.txt). Everything else is skipped without being written.

Usage: extract_units.py <leg.tar.gz> <wanted_units.txt> <dest dir> [<leg name>]
  wanted_units.txt: one unit name per line ("cap_...")
  dest dir receives <leg name>/<unit>.<ext> (leg name defaults to the tar's basename minus ".tar.gz")
"""
import os
import sys
import tarfile

# LONGEST SUFFIX FIRST. `cap_x_ce.txt` also ends in `.txt` and `cap_x_h.bin` also ends in `.bin`, so a shortest-first
# match strips the wrong tail, produces a stem that is not a unit name, and silently drops the file -- which showed up as
# "extracted 800 files for 400 units" (two per unit instead of four) and then as 400 replays that printed nothing.
SUFFIXES = ("_ce.txt", "_h.bin", ".txt", ".bin")


def main():
    tar_path, want_path, dest = sys.argv[1], sys.argv[2], sys.argv[3]
    leg = sys.argv[4] if len(sys.argv) > 4 else os.path.basename(tar_path).replace(".tar.gz", "")
    want = {line.strip() for line in open(want_path) if line.strip()}
    outdir = os.path.join(dest, leg)
    os.makedirs(outdir, exist_ok=True)

    written = 0
    with tarfile.open(tar_path, "r|gz") as tar:
        for member in tar:
            if not member.isfile():
                continue
            base = os.path.basename(member.name)
            stem = None
            for suf in SUFFIXES:
                if base.endswith(suf) and not base.startswith("._"):
                    stem = base[: -len(suf)]
                    break
            if stem is None or stem not in want:
                continue
            fh = tar.extractfile(member)
            if fh is None:
                continue
            with open(os.path.join(outdir, base), "wb") as out:
                out.write(fh.read())
            written += 1
    print(f"extracted {written} files for {len(want)} wanted units into {outdir}")


if __name__ == "__main__":
    main()
