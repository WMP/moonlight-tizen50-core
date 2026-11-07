# Sourced by build-deps-lib.sh and tests/build-esplus-ecore-test.sh.
# opus_sources DIR [dec]: the libopus .c files (relative to DIR), plain C, fixed point:
# CELT_SOURCES + SILK_SOURCES + SILK_SOURCES_FIXED + OPUS_SOURCES from the *_sources.mk files (no SIMD, float, dnn) + the encoder's analysis files (as in the fork's CMake target).
# With "dec": only what the decoder needs (the encoder-only files are left out; tests/build-esplus-ecore-test.sh links a decoder from this list).
# opus_cflags DIR: the defines and include dirs for these files (no ARM options: build-deps-lib.sh adds them).
opus_sources() {
  for v in CELT_SOURCES:celt SILK_SOURCES:silk SILK_SOURCES_FIXED:silk OPUS_SOURCES:opus; do
    # join backslash-continued lines, take the one "VAR = ..." line
    sed -e ':a' -e '/\\$/N; s/\\\n//; ta' "$1/${v#*:}_sources.mk" | sed -n "s/^${v%:*} *= *//p"
  done | { cat; printf '%s\n' src/analysis.c src/mlp.c src/mlp_data.c; } | tr -s ' \t' '\n\n' | grep '\.c$' | sort -u |
    if [ "$2" = dec ]; then grep -Ev "$OPUS_ENC_ONLY"; else cat; fi
}
OPUS_ENC_ONLY='^(celt/celt_encoder|silk/fixed/|silk/(enc_API|init_encoder|encode_|control_|check_control_input|NSQ|VAD|VQ_WMat_EC|NLSF_VQ|NLSF_encode|NLSF_del_dec_quant|process_NLSFs|quant_LTP_gains|stereo_(encode_pred|find_predictor|quant_pred|LR_to_MS)|HP_variable_cutoff|LP_variable_cutoff|ana_filt_bank_1)|src/(analysis|mlp|mlp_data|opus_encoder|opus_multistream_encoder|opus_projection_encoder))'
# opus_neon_sources DIR: the decoder's ARM NEON intrinsics files (the encoder's NSQ and warped_autocorrelation ones are left out).
# Build them with -DOPUS_ARM_MAY_HAVE_NEON_INTR -DOPUS_ARM_PRESUME_NEON_INTR (no run-time CPU detection, so no *_map.c and no armcpu.c).
opus_neon_sources() {
  for v in CELT_SOURCES_ARM_NEON_INTR:celt SILK_SOURCES_ARM_NEON_INTR:silk; do
    sed -e ':a' -e '/\\$/N; s/\\\n//; ta' "$1/${v#*:}_sources.mk" | sed -n "s/^${v%:*} *= *//p"
  done | tr -s ' \t' '\n\n' | grep '\.c$' | grep -v NSQ
}
opus_cflags() {
  echo "-std=gnu99 -DUSE_ALLOCA -DHAVE_LRINT -DHAVE_LRINTF -DFIXED_POINT -DOPUS_BUILD -I$1 -I$1/include -I$1/src -I$1/silk -I$1/celt -I$1/silk/fixed"
}
