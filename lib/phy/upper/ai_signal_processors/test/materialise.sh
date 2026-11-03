#!/bin/sh
# Rebuild the 10,000 capture files of ruler_s2_amc_5000 from a corpus directory.
#
#   ./materialise.sh /Users/jiachengwang/ai_depth3/corpus/s2_amc_ping_1102a
#
# Only <unit>.txt and <unit>.bin are copied -- those are the two files ul_chain_replay
# opens. The _h.bin and _ce.txt beside them in a capture directory are analysis products,
# not replay inputs.
set -eu

CORPUS=${1:?usage: $0 <corpus-dir> [dest]}
DEST=${2:-ruler_s2_amc_5000_corpus}
HERE=$(cd "$(dirname "$0")" && pwd)

mkdir -p "$DEST"
n=0
while read -r u; do
  for ext in .txt .bin; do
    src="$CORPUS/$u$ext"
    if [ ! -f "$src" ]; then
      echo "missing: $src" >&2
      exit 1
    fi
    cp "$src" "$DEST/"
  done
  n=$((n + 1))
done < "$HERE/ruler_s2_amc_5000_units.txt"

echo "copied $n units ($((n * 2)) files) into $DEST"
echo "verify with: cd $HERE && shasum -a 256 -c ruler_s2_amc_5000_SHA256SUMS"
