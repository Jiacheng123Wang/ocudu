"""Extract only the units a harvest needs, straight out of a leg tarball.

WHY NOT JUST UNTAR THE LEG. A leg is 100k-200k small files; extracting all of them costs minutes of random I/O per leg and
fills the worker's disk with data the harvest will not touch. The harvest only replays units the previous dump marked
crc=KO, so this reads the tar once, sequentially, and writes the four files of each wanted unit (.txt, .bin, _h.bin,
_ce.txt). Everything else is skipped without being written.

Usage: extract_units.py <leg.tar.gz> <full_units.txt> <dest dir> [<leg name>] [--sidecar <sidecar_units.txt>]
  full_units.txt:    units whose captures are needed (4 files each) -- the candidates and their donors
  --sidecar:         units whose PDU sidecar is needed but whose captures are not -- the units the previous dump
                     already decoded, which the chain bookkeeping still has to see but never has to replay
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
    argv = [a for a in sys.argv[1:] if not a.startswith("--sidecar")]
    sidecar_arg = next((a for a in sys.argv[1:] if a.startswith("--sidecar")), None)
    tar_path, want_path, dest = argv[0], argv[1], argv[2]
    leg = argv[3] if len(argv) > 3 and not argv[3].startswith("--") else os.path.basename(tar_path).replace(".tar.gz", "")
    full = {line.strip() for line in open(want_path) if line.strip()}
    sidecar = {line.strip() for line in open(sidecar_arg.split("=", 1)[1]) if line.strip()} \
        if sidecar_arg and "=" in sidecar_arg else set()
    want = full | sidecar
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
            # SIDECARS ONLY FOR THE UNITS THE DUMP ALREADY DECODED: their .txt carries the rnti/harq/rv that the chain
            # bookkeeping needs, and their samples already carry labels, so their captures would be dead weight on the
            # worker's disk.
            if stem in sidecar and stem not in full and not base.endswith(".txt"):
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
