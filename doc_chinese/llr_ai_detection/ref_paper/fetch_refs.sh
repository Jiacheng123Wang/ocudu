#!/usr/bin/env bash
# Download the reference papers cited by doc_chinese/llr_ai_detection/ into
# doc_chinese/llr_ai_detection/ref_paper/, naming each file by the paper's own title
# (fetched from the arXiv API, not typed by hand, so the name cannot drift from the paper).
#
# Not committed: see the note in doc_chinese/.gitignore - third-party PDFs, tens of MB, and
# nothing above 2 MB is a document by this tree's own rule. The committed artefact is README.md.
set -u

OUT="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$OUT"
API="http://export.arxiv.org/api/query"
LOG="$OUT/download_log.txt"
: > "$LOG"

# arXiv IDs cited by the workstream's memos, plan and survey files.
IDS=(
  2503.16594 2411.07600 2609.07805 2609.07843 2606.22283 2606.17090
  2605.26157 2608.12918 2605.28451 2609.32237
  2005.01494 2010.16283 2507.10409 2606.29345
  1911.13055 2609.04004 2408.04182 2208.05186 1011.2113 2601.16586
  2102.12756 1903.02865 1706.01151 2212.07816 2505.15848
  2506.15176 2506.21093 2404.05538 2510.12941 2210.14103
  2502.05952 2102.02993 2409.02912 2602.04652 2609.31177
  2409.05610 2510.01533 2507.18538
)

ok=0; fail=0
for id in "${IDS[@]}"; do
  # 1) title, straight from the API
  meta=$(curl -sS -L --max-time 60 "$API?id_list=$id" 2>/dev/null)
  title=$(printf '%s' "$meta" | python3 -c '
import sys,re,html
s=sys.stdin.read()
m=re.search(r"<entry>.*?<title>(.*?)</title>",s,re.S)
print(html.unescape(re.sub(r"\s+"," ",m.group(1))).strip() if m else "")
')
  if [ -z "$title" ]; then
    printf 'MISS  %-12s (no API title)\n' "$id" | tee -a "$LOG"; fail=$((fail+1)); continue
  fi
  # Sanitise for a filesystem: ": " reads as a path separator in Finder, "/" cannot appear at all.
  name=$(printf '%s' "$title" | sed -e 's|/|-|g' -e 's|: |-|g' -e 's|:$||g' -e 's|"||g')
  dest="$OUT/$name.pdf"
  if [ -s "$dest" ]; then
    printf 'SKIP  %-12s %s\n' "$id" "$name" | tee -a "$LOG"; ok=$((ok+1)); continue
  fi
  # 2) the PDF
  code=$(curl -sS -L --max-time 180 -o "$dest.part" -w '%{http_code}' "https://arxiv.org/pdf/$id" 2>/dev/null)
  if [ "$code" = "200" ] && [ -s "$dest.part" ] && head -c 4 "$dest.part" | grep -q '%PDF'; then
    mv "$dest.part" "$dest"
    size=$(du -h "$dest" | cut -f1)
    printf 'OK    %-12s %-6s %s\n' "$id" "$size" "$name" | tee -a "$LOG"; ok=$((ok+1))
  else
    rm -f "$dest.part"
    printf 'FAIL  %-12s http=%s  %s\n' "$id" "$code" "$name" | tee -a "$LOG"; fail=$((fail+1))
  fi
  sleep 1.5   # arXiv asks for a polite rate
done
printf '\n=== %d downloaded, %d failed ===\n' "$ok" "$fail" | tee -a "$LOG"
