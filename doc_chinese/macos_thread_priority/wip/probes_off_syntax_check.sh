#!/usr/bin/env bash
# DOES THE PROBES-OFF ARM STILL COMPILE?  The default build's blind spot, made checkable in ~10 seconds.
#
# WHY THIS EXISTS (2026-10-05). `ENABLE_FLOW_PROBES` is **OFF by default** (CMakeLists.txt) and every leg of the
# macos_thread_priority workstream is flown with it **ON**, so the arm that nobody compiles is the one the default
# build uses. A probe method added to `ul_pipeline_probe` without its no-op stub therefore breaks Linux/CI while
# macOS stays green - measured: the Ubuntu build failed on
#
#   lower_phy_baseband_processor.cpp:1609: error: no member named 'record_thread_cpu_boundary'
#
# because the call site is deliberately not inside a `#if defined(OCUDU_FLOW_PROBES)` guard (it files one window
# per slot for the thread a declaration has to be written for) and the no-op class in the `#else` arm never got
# the stub.
#
# WHAT IT DOES: for every source file that names one of the probes, take the compile flags its OWN target recorded
# in the build tree, remove `-DOCUDU_FLOW_PROBES`, and syntax-check it. No link, no full rebuild, no radio. The
# recipe is the one that reproduced the Ubuntu failure locally in 0.5 s.
#
# THE RULE IT ENFORCES (stated where the next addition will read it): any probe method CALLED FROM CODE THAT IS
# NOT INSIDE a probe guard must exist in BOTH arms with the same signature and defaults; methods whose call sites
# ARE guarded may omit the no-op stub, which is why the two arms are not mirrors of each other.
#
# usage:  bash probes_off_syntax_check.sh [build-dir]        (default: build)
# exit:   0 = every probe-using TU compiles with the probes OFF, 1 = at least one does not.
set -u

BUILD="${1:-build}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT" || exit 2

command -v c++ >/dev/null || { echo "no c++ in PATH" >&2; exit 2; }
[ -d "$BUILD" ] || { echo "no build dir '$BUILD': configure one first (the flags come from it)" >&2; exit 2; }

# The TUs that can break: the ones that name a probe. A file that merely includes the header transitively cannot
# call a missing method, so naming is the right filter - and it keeps this at ten seconds instead of an hour.
# A `while read` loop rather than `mapfile`: the flight Mac's /bin/bash is 3.2 and has no mapfile (the script is
# meant to run there too - it is the machine whose build nobody ever compiles with the probes OFF).
SRC=()
while IFS= read -r f; do
  SRC+=("$f")
done < <(git ls-files lib apps utils include | grep -E '\.(cpp|mm)$' | while read -r f; do
  grep -lqE 'ul_pipeline_probe|handoff_probe|macos_compat' "$f" 2>/dev/null && echo "$f"
done)

[ "${#SRC[@]}" -gt 0 ] || { echo "no probe-using sources found (wrong tree?)" >&2; exit 2; }

fail=0
checked=0
for src in "${SRC[@]}"; do
  base="$(basename "$src")"
  # The flags.make of the target that builds this file: found through the build.make whose object rule names the
  # FULL source path. Matching the basename instead picks up unrelated targets (a test, or another directory's
  # file of the same name) whose include set does not contain this file's headers, which shows up as a bogus
  # "fatal error: 'x.h' file not found" - measured while writing this script.
  dir=""
  while read -r bm; do
    grep -qF "$src" "$bm" 2>/dev/null || continue
    # ... AND an object RULE for it: a build.make also lists other targets' sources as prerequisites, and those
    # entries come with a different include set (that mismatch is what produced the bogus "file not found").
    grep -qE "(^|[ /])$(printf '%s' "$base" | sed 's/\./\\./g')\.o:" "$bm" 2>/dev/null || continue
    [ -f "$(dirname "$bm")/flags.make" ] && { dir="$(dirname "$bm")"; break; }
  done < <(find "$BUILD" -name build.make 2>/dev/null)

  if [ -z "$dir" ]; then
    printf '%-56s %s\n' "$src" "SKIP (not built in $BUILD)"
    continue
  fi

  # eval, not word splitting: per-target defines carry escaped quotes (OCUDU_DFT_METALLIB_PATH=\"...\") and a
  # plain expansion mangles them into a bogus -Werror diagnostic about a missing quote.
  # `.mm` is Objective-C++ and CMake records ITS flags under OBJCXX_*, not CXX_*: reading the wrong triplet gives
  # an empty include list, which reports as "fatal error: 'ocudu/...' file not found" - a false alarm about a
  # file that builds perfectly (measured while writing this script).
  case "$src" in
    *.mm|*.m) prefix=OBJCXX ;;
    *)        prefix=CXX ;;
  esac
  if ! grep -q "^${prefix}_FLAGS" "$dir/flags.make"; then
    printf '%-56s %s\n' "$src" "SKIP (no ${prefix}_FLAGS in its target)"
    continue
  fi
  defs="$(sed -n "s/^${prefix}_DEFINES = //p" "$dir/flags.make" | sed 's/-DOCUDU_FLOW_PROBES//')"
  incs="$(sed -n "s/^${prefix}_INCLUDES = //p" "$dir/flags.make")"
  flgs="$(sed -n "s/^${prefix}_FLAGS = //p" "$dir/flags.make")"

  checked=$((checked + 1))
  printf '%-56s ' "$src"
  if eval "c++ $defs $incs $flgs -fsyntax-only '$src'" 2>"/tmp/probes_off_$(basename "$src").err"; then
    echo "OK"
  else
    echo "FAIL"
    head -8 "/tmp/probes_off_$(basename "$src").err" | sed 's/^/      /'
    fail=1
  fi
done

echo
if [ "$fail" -eq 0 ]; then
  echo "probes-OFF arm: $checked TU(s) compile (the default configuration is safe)."
else
  echo "probes-OFF arm: BROKEN. A probe method that unguarded code calls is missing from the no-op class in"
  echo "ul_pipeline_probe.h's '#else // not OCUDU_FLOW_PROBES' arm - add the stub with the same signature."
fi
exit "$fail"
