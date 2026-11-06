# Sourced by build-deps-lib.sh and tests/build-esplus-ecore-test.sh.
# opus_sources DIR: the libopus decoder+encoder .c files (relative to DIR), plain C, fixed point:
# CELT_SOURCES + SILK_SOURCES + SILK_SOURCES_FIXED + OPUS_SOURCES from the *_sources.mk files (no SIMD, float, dnn) + the encoder's analysis files (as in the fork's CMake target).
opus_sources() {
  for v in CELT_SOURCES:celt SILK_SOURCES:silk SILK_SOURCES_FIXED:silk OPUS_SOURCES:opus; do
    # join backslash-continued lines, take the one "VAR = ..." line
    sed -e ':a' -e '/\\$/N; s/\\\n//; ta' "$1/${v#*:}_sources.mk" | sed -n "s/^${v%:*} *= *//p"
  done | { cat; printf '%s\n' src/analysis.c src/mlp.c src/mlp_data.c; } | tr -s ' \t' '\n\n' | grep '\.c$' | sort -u
}
