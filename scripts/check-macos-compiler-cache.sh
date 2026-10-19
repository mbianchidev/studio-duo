#!/usr/bin/env bash
set -euo pipefail

fail() {
    printf 'macOS compiler cache check failed: %s\n' "$*" >&2
    exit 1
}

[[ $# -eq 0 ]] || fail "Usage: $0"
[[ "$(uname -s)" == Darwin ]] || fail "This check requires macOS."
cache="$(command -v ccache)" || fail "Install ccache before running this check."
compiler="$(xcrun --find clang++)" || fail "Apple Clang is unavailable."
work="$(mktemp -d /tmp/studio-duo-cache-check.XXXXXX)"
cleanup() {
    result=$?
    trap - EXIT
    if ! rm -rf -- "$work"; then
        printf 'Could not remove the isolated compiler cache: %s\n' "$work" >&2
        result=1
    fi
    exit "$result"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

export CCACHE_DIR="$work/cache"
export CCACHE_CONFIGPATH=/dev/null
export CCACHE_COMPILERCHECK=content
export CCACHE_REMOTE_STORAGE=""

cat > "$work/probe.cpp" <<'CPP'
#if defined(__arm64__) || defined(__aarch64__)
#include "arm-value.h"
#elif defined(__x86_64__)
#include "intel-value.h"
#else
#error Unsupported probe architecture
#endif

int studioDuoCompilerCacheProbe()
{
    return STUDIO_DUO_CACHE_PROBE_VALUE;
}
CPP
printf '#define STUDIO_DUO_CACHE_PROBE_VALUE 11\n' > "$work/arm-value.h"
printf '#define STUDIO_DUO_CACHE_PROBE_VALUE 17\n' > "$work/intel-value.h"

check_object() {
    "$cache" "$compiler" -std=c++20 -O3 -arch arm64 -arch x86_64 \
        -mmacosx-version-min=12.0 -c "$work/probe.cpp" -o "$work/cached.o"
    "$compiler" -std=c++20 -O3 -arch arm64 -arch x86_64 \
        -mmacosx-version-min=12.0 -c "$work/probe.cpp" -o "$work/reference.o"
    for architecture in arm64 x86_64; do
        lipo "$work/cached.o" -verify_arch "$architecture"
    done
    cmp -s "$work/cached.o" "$work/reference.o" \
        || fail "Cached universal object differs after $1."
}

# Do not let timestamp-safety bypasses turn the probe into uncached compilation.
sleep 2
check_object "the initial compilation"
check_object "an unchanged cache lookup"
printf '#define STUDIO_DUO_CACHE_PROBE_VALUE 23\n' > "$work/intel-value.h"
sleep 2
check_object "an Intel-only header change"
printf '#define STUDIO_DUO_CACHE_PROBE_VALUE 29\n' > "$work/arm-value.h"
sleep 2
check_object "an ARM-only header change"

"$cache" --print-stats | awk '
    $1 == "direct_cache_hit" || $1 == "preprocessed_cache_hit" { hits += $2 }
    $1 == "cache_miss" { misses += $2 }
    END { exit !(hits >= 1 && misses >= 3) }
' || fail "The probe must record both cache reuse and architecture-specific misses."
printf 'Verified universal compiler cache reuse and independent Intel/ARM header invalidation.\n'
