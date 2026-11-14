#!/bin/bash
# usage: bash remote_verify.sh /mnt/corpus/tarballs/<leg>.tar
f=$1
if [ ! -f "$f" ]; then echo "STATUS=MISSING"; exit 3; fi
bytes=$(stat -c %s "$f")
tar -tf "$f" 2>/tmp/verify_tar_err.txt | awk '
  /(^|\/)\._/ { d++; next }
  /\/cap_[^\/]*\.txt$/ {
      a++
      if ($0 ~ /_ce\.txt$/) ce++
  }
  END { printf "ALLTXT=%d CE=%d NONCE=%d APPDOUBLE=%d\n", a, ce, a-ce, d }
'
rc=${PIPESTATUS[0]}
echo "TAR_RC=$rc"
echo "SIZE=$bytes"
if [ -s /tmp/verify_tar_err.txt ]; then echo "TAR_STDERR:"; head -5 /tmp/verify_tar_err.txt; fi
