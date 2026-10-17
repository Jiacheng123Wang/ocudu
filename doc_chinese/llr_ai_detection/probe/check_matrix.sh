#!/usr/bin/env bash
# The §1.5.3 conflict matrix, exercised against the REAL binary -- every row, legal and illegal.
#
# Why this exists. The matrix is enforced in three places (the CLI11 value checks, the validator's cross-parameter
# check, and resolve_phy_pipeline, which the factories also call), and a unit test can only prove that the resolver
# returns the right answer for a request it was handed. What a flight depends on is the whole path: the knobs as
# they are typed, the config file, the validator and the startup log. On 2026-10-16 that difference cost a flight:
# `--pusch_receiver_backend=ai` with a valid model ran the CLASSICAL receiver for the whole leg and nothing in the
# pre-flight noticed (`--dryrun` exits 0 for configurations the app then refuses - see the note below).
#
# So each row here starts the real binary, without sudo and without a phone (the resolution runs before the radio
# is opened), and asserts on what the app did:
#
#   * ILLEGAL rows must print "Invalid configuration: ..." (or the CLI11 check) and exit, and must NOT print an
#     effective-configuration line. A row that starts is a hole in the matrix.
#   * LEGAL rows must print "receiver=" with the expected value and must NOT print "Invalid configuration".
#
# The last legal row is the boundary: a model that EXISTS but is not a bundle is not refused (existence is the
# validator's question, binding is the arm's), and the arm reports AI ARM INACTIVE. That row is here so the
# boundary is measured rather than assumed.
#
# Usage: check_matrix.sh [seconds-per-legal-row]
set -u
cd "$HOME/dev/ocudu"
W=$PWD/doc_chinese/llr_ai_detection/work_tmp
CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz_llr_collect_p0default.yml
MODEL=$W/probe/d3_256qam.mlmodelc
SECS=${1:-6}
NOT_A_BUNDLE=/tmp/d3_not_a_bundle.$$.mlmodelc
# The model the build binds when the knob is absent (CMake OCUDU_DEPTH3_DEFAULT_MODEL). Read from the binary
# rather than assumed, so this check follows a build that points its default elsewhere.
# --help prints the value of the KNOB, which is empty by design (the default is applied by the resolver), so the
# build's default is read from the binary itself.
DEFAULT_MODEL=$(strings build/apps/gnb/gnb 2>/dev/null | grep -o '/[^ "]*\.mlmodelc' | head -1)
DEFAULT_MODEL=${DEFAULT_MODEL:-$W/probe/d3_256qam.mlmodelc}

if [ ! -x build/apps/gnb/gnb ]; then echo "no gnb binary"; exit 2; fi
if pgrep -x gnb >/dev/null; then echo "a gNB is already running: this check starts its own; stop that one first"; exit 2; fi
if [ ! -d "$MODEL" ]; then echo "no model at $MODEL"; exit 2; fi
mkdir -p "$NOT_A_BUNDLE"   # exists, but the loader will not find a bundle in it

pass=0
fail=0

# run_row <expect: refuse|accept> <expected receiver, when accept> <label> [extra knobs...]
run_row() {
  local expect=$1 want_receiver=$2 label=$3
  shift 3
  local out log
  out=$(mktemp -t matrix_XXXXXX)
  log=$(mktemp -t matrix_XXXXXX)

  ./build/apps/gnb/gnb -c "$CONFIG" --log.filename="$log" "$@" > "$out" 2>&1 &
  local pid=$!
  # A refusal exits on its own within a second or two; a legal row keeps running and is stopped here. Two
  # interrupts (the first graceful, the second forced) so no row can leave a gNB behind.
  local waited=0
  while [ "$waited" -lt "$SECS" ] && kill -0 "$pid" 2>/dev/null; do sleep 1; waited=$((waited + 1)); done
  if kill -0 "$pid" 2>/dev/null; then
    kill -INT "$pid" 2>/dev/null; sleep 1; kill -INT "$pid" 2>/dev/null
    for _ in $(seq 1 5); do kill -0 "$pid" 2>/dev/null || break; sleep 1; done
    kill -0 "$pid" 2>/dev/null && kill -9 "$pid" 2>/dev/null
  fi
  wait "$pid" 2>/dev/null
  local rc=$?

  local refusal receiver
  # A refusal can arrive from the configuration validator or from the command line itself, and the CLI's message
  # has its own shape (`--pusch_reporting_source: network not in {classic,net}`). Both are refusals; leaving the
  # CLI one out made this harness report "accepted a configuration the matrix refuses" for a value the binary had
  # in fact rejected (measured 2026-10-16).
  refusal=$(grep -ahm1 -E "Invalid configuration|Invalid PUSCH receiver backend|not in \{" "$out" "$log" 2>/dev/null | cut -c1-170)
  # The line the translator writes, and ONLY that line: the refusal for "ai without a model" quotes the very
  # string "receiver=ai" in its own explanation, so a bare `grep receiver=` reads a refusal as a resolution.
  # (That false positive is what this script printed on its first run - the row was refused correctly and the
  # check called it a contradiction.)
  receiver=$(grep -ahm1 -E "\[phy_pipeline\][[:space:]]+receiver=" "$log" "$out" 2>/dev/null |
             sed 's/.*receiver=//' | cut -c1-60)

  last_banner=$(grep -ahm1 -E "AI ARM (ACTIVE|INACTIVE)|IS THE IDENTITY|WITHOUT a model" "$out" "$log" 2>/dev/null |
                cut -c1-200)
  # The bound models moved out of the banner and into the coverage block, so the assertion that a run bound a
  # PARTICULAR model reads the coverage lines (deduplicated: the factory prints them once per construction).
  last_coverage=$(grep -ah 'computes Qm=' "$out" "$log" 2>/dev/null | sort -u | cut -c1-200)
  local ok=1 why=""
  if [ "$expect" = refuse ]; then
    [ -n "$refusal" ] || { ok=0; why="accepted a configuration the matrix refuses"; }
    [ -z "$receiver" ] || { ok=0; why="refused AND resolved (receiver=$receiver) - which is it?"; }
    [ "$rc" != 0 ] || { ok=0; why="exit code 0 on a refusal"; }
  else
    [ -z "$refusal" ] || { ok=0; why="refused: $refusal"; }
    case "$receiver" in
      "$want_receiver"*) ;;
      *) ok=0; why="receiver='${receiver:-<none printed>}', wanted '$want_receiver'" ;;
    esac
  fi

  if [ "$ok" = 1 ]; then
    pass=$((pass + 1))
    printf '  PASS  %-58s %s\n' "$label" "${refusal:+refused: ${refusal:0:80}}${receiver:+receiver=$receiver}"
  else
    fail=$((fail + 1))
    printf '  FAIL  %-58s %s\n' "$label" "$why"
  fi
  rm -f "$out" "$log"
}

echo "=== §1.5.3 matrix against $(grep -o '"[0-9a-f]\{10\}"' build/hashes.h | head -1 | tr -d '"') (${SECS}s per legal row) ==="
echo "-- illegal: must be refused, with the reason in the log"
run_row refuse "" "cpu + ai" \
  --expert_phy.phy_pipeline=cpu --expert_phy.pusch_receiver_backend=ai --expert_phy.pusch_receiver_model="$MODEL"
run_row refuse "" "cpu_gpu + ai" \
  --expert_phy.phy_pipeline=cpu_gpu --expert_phy.pusch_receiver_backend=ai --expert_phy.pusch_receiver_model="$MODEL"
run_row refuse "" "gpu + ai, model path does not exist" \
  --expert_phy.phy_pipeline=gpu --expert_phy.pusch_receiver_backend=ai --expert_phy.pusch_receiver_model=/nonexistent/d3.mlmodelc
run_row refuse "" "gpu + a CPU channel estimator" \
  --expert_phy.phy_pipeline=gpu --expert_phy.pusch_channel_estimator_algo=cpu
run_row refuse "" "bogus backend value" \
  --expert_phy.phy_pipeline=gpu --expert_phy.pusch_receiver_backend=bogus
echo "-- legal: must resolve, and the startup line must say to what"
run_row accept classic "gpu + auto" --expert_phy.phy_pipeline=gpu
run_row accept classic "gpu + classic" \
  --expert_phy.phy_pipeline=gpu --expert_phy.pusch_receiver_backend=classic
run_row accept classic "cpu_gpu + classic" \
  --expert_phy.phy_pipeline=cpu_gpu --expert_phy.pusch_receiver_backend=classic
run_row accept ai "gpu + ai + model" \
  --expert_phy.phy_pipeline=gpu --expert_phy.pusch_receiver_backend=ai --expert_phy.pusch_receiver_model="$MODEL"
# R6's reporting source. `net` needs an arm that computes, so it is refused wherever the receiver resolved to
# anything else -- otherwise the run would print reporting=net while the classical estimator's numbers reached the
# scheduler, which is the shape of failure this matrix exists to refuse. The value itself is checked by the CLI.
run_row refuse "" "reporting net without an ai receiver" \
  --expert_phy.phy_pipeline=gpu --expert_phy.pusch_reporting_source=net
run_row refuse "" "reporting net + classic receiver" \
  --expert_phy.phy_pipeline=gpu --expert_phy.pusch_receiver_backend=classic --expert_phy.pusch_reporting_source=net
run_row accept ai "gpu + ai + model + reporting net" \
  --expert_phy.phy_pipeline=gpu --expert_phy.pusch_receiver_backend=ai --expert_phy.pusch_receiver_model="$MODEL" \
  --expert_phy.pusch_reporting_source=net
run_row refuse "" "reporting source, not a value it accepts" \
  --expert_phy.phy_pipeline=gpu --expert_phy.pusch_reporting_source=network
# "ai" with NO model: legal, and it must bind the build's default -- not the identity, which is the failure the
# check exists for (a run whose line says receiver=ai while the classical chain computes).
run_row accept ai "gpu + ai + no model (the build's default)" \
  --expert_phy.phy_pipeline=gpu --expert_phy.pusch_receiver_backend=ai
if [ -n "$DEFAULT_MODEL" ] && [ -n "$last_coverage" ]; then
  case "$last_coverage" in
    *"$(basename "$DEFAULT_MODEL")"*)
      pass=$((pass + 1)); printf '  PASS  %-58s %s\n' "  ... and it bound $(basename "$DEFAULT_MODEL")" "$(echo "$last_coverage" | head -1 | sed 's/.*computes //' | cut -c1-80)" ;;
    *)
      fail=$((fail + 1)); printf '  FAIL  %-58s %s\n' "  ... and it bound $(basename "$DEFAULT_MODEL")" "coverage: ${last_coverage:0:110}" ;;
  esac
else
  printf '  SKIP  %-58s %s\n' "  ... and it bound the default" "the binary prints no default model in --help"
fi
run_row accept ai "gpu + ai + model + metal DFT (the separate arm)" \
  --expert_phy.phy_pipeline=gpu --expert_phy.pusch_receiver_backend=ai --expert_phy.pusch_receiver_model="$MODEL" \
  --expert_phy.pusch_dft_type=metal
echo "-- the boundary: a model that exists but is not a bundle is NOT refused by the matrix"
run_row accept ai "gpu + ai + a path that is not a bundle (arm reports INACTIVE)" \
  --expert_phy.phy_pipeline=gpu --expert_phy.pusch_receiver_backend=ai --expert_phy.pusch_receiver_model="$NOT_A_BUNDLE"

# ---- the per-modulation slots: which modulations a run COMPUTES is read from the coverage line ----
# WHY (2026-10-16): the fallback knob is filled by the build's default, so the command line alone does not say
# which modulations a run covers -- `--pusch_receiver_model_qpsk=<qpsk>` ALSO computes 256QAM, and a minimal
# `gpu + ai` computes 256QAM alone while its model was called "mixed". These rows assert the coverage the app
# prints, and the refusal a slot gets when its bundle declares another modulation.
run_coverage_row() {
  local label=$1 want_computes=$2 want_degrades=$3
  shift 3
  local out log
  out=$(mktemp -t cov_XXXXXX); log=$(mktemp -t cov_XXXXXX)
  ./build/apps/gnb/gnb -c "$CONFIG" --log.filename="$log" "$@" > "$out" 2>&1 &
  local pid=$!
  sleep "$SECS"
  kill -INT "$pid" 2>/dev/null; sleep 1; kill -INT "$pid" 2>/dev/null
  for _ in $(seq 1 10); do kill -0 "$pid" 2>/dev/null || break; sleep 1; done
  kill -0 "$pid" 2>/dev/null && kill -9 "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null

  # UNIQUE lines: the app builds the PUSCH factory twice (pre-existing, visible in the flown legs' logs as two
  # "AI ARM ACTIVE" banners), so counting raw lines would double every number and, worse, would make a run that
  # covered a modulation TWICE look like one that covered two more.
  local computes degrades n why="" ok=1
  # The per-modulation COVERAGE lines only: a conditioned bundle also prints a summary line
  # ("contract=modcond: ONE bundle computes Qm={2,4,6,8}"), which the loose pattern `computes Qm=` counted as a fifth
  # covered modulation. The per-Qm lines have a digit right after the '='.
  computes=$(grep -ahE '[[:space:]]computes Qm=[0-9]' "$out" | sort -u | wc -l | tr -d ' ')
  degrades=$(grep -ahE '[[:space:]]degrades Qm=[0-9]' "$out" | sort -u | wc -l | tr -d ' ')
  n=$(grep -ahm1 -o 'AI ARM ACTIVE over [0-9]* model' "$out" | grep -o '[0-9]*')
  [ "${n:-0}" = "$want_computes" ] || { ok=0; why="banner says ${n:-no} model(s), wanted $want_computes"; }
  [ "$computes" = "$want_computes" ]  || { ok=0; why="$why; $computes computes line(s), wanted $want_computes"; }
  [ "$degrades" = "$want_degrades" ]  || { ok=0; why="$why; $degrades degrades line(s), wanted $want_degrades"; }
  if [ "$ok" = 1 ]; then
    pass=$((pass + 1)); printf '  PASS  %-58s computes %s, degrades %s\n' "$label" "$computes" "$degrades"
  else
    fail=$((fail + 1)); printf '  FAIL  %-58s %s\n' "$label" "$why"
    grep -a 'computes Qm=\|degrades Qm=' "$out" | sed 's/^/        /' | cut -c1-120
  fi
  rm -f "$out" "$log"
}

run_refusal_row() {
  local label=$1 want=$2
  shift 2
  local out log
  out=$(mktemp -t ref_XXXXXX); log=$(mktemp -t ref_XXXXXX)
  ./build/apps/gnb/gnb -c "$CONFIG" --log.filename="$log" "$@" > "$out" 2>&1 &
  local pid=$!
  sleep "$SECS"
  kill -INT "$pid" 2>/dev/null; sleep 1; kill -INT "$pid" 2>/dev/null
  for _ in $(seq 1 10); do kill -0 "$pid" 2>/dev/null || break; sleep 1; done
  kill -0 "$pid" 2>/dev/null && kill -9 "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
  if grep -aq "$want" "$out"; then
    pass=$((pass + 1)); printf '  PASS  %-58s %s\n' "$label" "$(grep -ahm1 "$want" "$out" | sed 's/.*AI depth-3: //' | cut -c1-70)"
  else
    fail=$((fail + 1)); printf '  FAIL  %-58s no line matching %s\n' "$label" "$want"
  fi
  rm -f "$out" "$log"
}

echo "-- the per-modulation slots: the coverage must be READ, not inferred"
# The build's default bundle is the MODULATION-CONDITIONED one since 2026-10-17 (design section 19.148), so ONE
# artifact serves all four modulations: this row asserted "1 compute, 3 degrades" while the default was a single
# 256QAM bundle, and it was the only thing red in the matrix after the artifact was repointed.
run_coverage_row "gpu + ai, no slot (the conditioned default: all four)" 4 0 \
  --expert_phy.phy_pipeline=gpu --expert_phy.pusch_receiver_backend=ai
# A per-modulation slot CANNOT coexist with the conditioned default: the two contracts need different input planes, so
# the arm refuses the second one by name (design section 19.112). The slot binds alone and the other three degrade.
run_coverage_row "gpu + ai + qpsk slot (the conditioned default is refused)" 1 3 \
  --expert_phy.phy_pipeline=gpu --expert_phy.pusch_receiver_backend=ai \
  --expert_phy.pusch_receiver_model_qpsk="$W/probe/d3_qpsk.mlmodelc"
run_coverage_row "gpu + ai + three slots (the conditioned default is refused)" 3 1 \
  --expert_phy.phy_pipeline=gpu --expert_phy.pusch_receiver_backend=ai \
  --expert_phy.pusch_receiver_model_qpsk="$W/probe/d3_qpsk.mlmodelc" \
  --expert_phy.pusch_receiver_model_16qam="$W/probe/d3_16qam.mlmodelc" \
  --expert_phy.pusch_receiver_model_64qam="$W/probe/d3_64qam.mlmodelc"
run_refusal_row "a slot whose bundle declares another modulation" "slot asks for Qm=2 but the bundle declares Qm=6" \
  --expert_phy.phy_pipeline=gpu --expert_phy.pusch_receiver_backend=ai \
  --expert_phy.pusch_receiver_model_qpsk="$W/probe/d3_64qam.mlmodelc"

rmdir "$NOT_A_BUNDLE" 2>/dev/null
echo
echo "=== $pass passed, $fail failed ==="
[ "$fail" = 0 ] || exit 1
