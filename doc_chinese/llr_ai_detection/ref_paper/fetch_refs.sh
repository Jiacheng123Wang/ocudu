#!/usr/bin/env bash
# Download the reference papers cited by doc_chinese/llr_ai_detection/ into
# doc_chinese/llr_ai_detection/ref_paper/, naming each file by the paper's own title
# (fetched from the arXiv API, not typed by hand, so the name cannot drift from the paper).
#
# Not committed: see the note in doc_chinese/.gitignore - third-party PDFs, tens of MB, and
# nothing above 2 MB is a document by this tree's own rule. The committed artefacts are
# README.md (the curated guide), survey_citations.md (generated) and download_log.txt.
#
# Idempotent and resumable: an existing non-empty dest is skipped, so re-running after an
# interruption only fetches what is still missing. Titles are fetched in batches of 40
# rather than one request per ID -- arXiv rate-limits per request, and one-per-ID earned a
# run of "no API title" misses that silently skipped whole papers.
set -u

OUT="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$OUT"
API="http://export.arxiv.org/api/query"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

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
  2508.06275 2509.13786 2603.09644
  1607.04793 1701.05931 1702.00832 1706.07043 1707.03384 1802.04741 1805.07631
  1805.09317 1807.00801 1809.01022 1809.09336 1812.05929 1812.10044 1902.06939
  1906.04610 1907.01512 1907.09439 2002.02750 2004.05062 2006.16015 2008.08264
  2009.02591 2009.05261 2012.08405 2101.04726 2102.03828 2105.05044 2106.16079
  2107.11958 2201.03866 2201.11779 2203.11854 2204.05350 2210.03888 2211.06054
  2212.03839 2302.02436 2304.07696 2305.07309 2305.15543 2308.12591 2312.02601
  2401.07515 2405.13413 2406.18993 2501.09761 2501.14861 2502.09326 2503.20500
  2505.06671 2505.12736 2508.12892 2509.12694 2509.18574 2510.22608 2511.05502
  2604.03585 2604.27279 2605.29995 2607.12555 2608.22110 2609.28852 2609.30459
  1612.01183 1812.01571 2005.02262 2307.12575 2308.11335 2311.00226 2403.01098
  2404.06469 2409.00124 2411.04136 2411.10178 2505.06175 2506.00368 2506.13408
  2507.04040 2508.00587 2508.02314 2508.08790 2508.17960 2511.03923 2512.13263
  2512.16315 2601.10963 2602.11834 2603.14661 2604.00529 2605.01931 2605.13507
  2606.06239 2606.27466 2607.08717 2607.18285 2607.24669 2607.26016 2608.23582
  2609.14735 2609.25847
  1903.04766 1909.01683 2401.04077 2505.09076 2602.04728 2602.11951 2602.15458
)

LOG="$OUT/download_log.txt"
: > "$LOG"

# ---------------------------------------------------------------- titles (batched)
# Map ID -> title for every ID the API answers for. Batches keep the request count low;
# a failed batch is retried with backoff before being given up on.
declare -A TITLE=()
BATCH=40
for ((i = 0; i < ${#IDS[@]}; i += BATCH)); do
  chunk=("${IDS[@]:i:BATCH}")
  list=$(IFS=,; echo "${chunk[*]}")
  for attempt in 1 2 3 4; do
    if curl -sS -L --max-time 120 "$API?id_list=$list&max_results=$BATCH" -o "$TMP/meta.xml" 2>/dev/null \
       && [ -s "$TMP/meta.xml" ] && grep -q '<entry>' "$TMP/meta.xml"; then
      break
    fi
    sleep $((attempt * 5))
  done
  python3 - "$TMP/meta.xml" <<'PY' > "$TMP/titles.txt"
import re, sys, html
try:
    s = open(sys.argv[1], encoding="utf-8", errors="replace").read()
except OSError:
    sys.exit(0)
for e in re.findall(r"<entry>(.*?)</entry>", s, re.S):
    i = re.search(r"<id>http://arxiv\.org/abs/([0-9.]+?)(?:v\d+)?</id>", e, re.S)
    t = re.search(r"<title>(.*?)</title>", e, re.S)
    if not i or not t:
        continue
    title = html.unescape(re.sub(r"\s+", " ", t.group(1))).strip()
    if title:
        print(f"{i.group(1)}\t{title}")
PY
  while IFS=$'\t' read -r id title; do
    [ -n "${id:-}" ] && TITLE["$id"]="$title"
  done < "$TMP/titles.txt"
  printf 'titles: %d/%d resolved\n' "${#TITLE[@]}" "${#IDS[@]}"
  sleep 3   # arXiv asks for a polite rate
done

# ---------------------------------------------------------------- PDFs
ok=0; skip=0; fail=0
for id in "${IDS[@]}"; do
  title="${TITLE[$id]:-}"
  if [ -z "$title" ]; then
    printf 'MISS\t%s\t(no API title)\n' "$id" | tee -a "$LOG"; fail=$((fail+1)); continue
  fi
  # Sanitise for a filesystem: ": " reads as a path separator in Finder, "/" cannot appear at all.
  name=$(printf '%s' "$title" | sed -e 's|/|-|g' -e 's|: |-|g' -e 's|:$||g' -e 's|"||g')
  dest="$OUT/$name.pdf"
  if [ -s "$dest" ]; then
    # Tab-delimited so the title survives parsing: a space-delimited log truncates every
    # multi-word title at its first space, and the title IS the filename.
    printf 'SKIP\t%s\t%s\n' "$id" "$name" | tee -a "$LOG"; skip=$((skip+1)); continue
  fi
  code=""
  for attempt in 1 2 3; do
    code=$(curl -sS -L --max-time 180 -o "$dest.part" -w '%{http_code}' "https://arxiv.org/pdf/$id" 2>/dev/null)
    if [ "$code" = "200" ] && [ -s "$dest.part" ] && head -c 4 "$dest.part" | grep -q '%PDF'; then
      break
    fi
    sleep $((attempt * 4))
  done
  if [ "$code" = "200" ] && [ -s "$dest.part" ] && head -c 4 "$dest.part" | grep -q '%PDF'; then
    mv "$dest.part" "$dest"
    printf 'OK\t%s\t%s\n' "$id" "$name" | tee -a "$LOG"; ok=$((ok+1))
  else
    rm -f "$dest.part"
    printf 'FAIL\t%s\thttp=%s\n' "$id" "$code" | tee -a "$LOG"; fail=$((fail+1))
  fi
  sleep 1.5
done
printf '\n=== %d downloaded, %d already present, %d failed (of %d) ===\n' \
  "$ok" "$skip" "$fail" "${#IDS[@]}" | tee -a "$LOG"
