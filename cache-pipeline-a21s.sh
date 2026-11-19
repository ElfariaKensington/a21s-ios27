#!/bin/bash
# Cortex-A55 optimized cache-pipeline for Galaxy A21s (Exynos 850: 8x Cortex-A55 ARMv8.2-A).
# All 8 cores have native LSE, FP16, RDM, DotProd, LRCPC — so Step 2 (lse-rewrite) is skipped,
# and make-oracle (--mcpu=cortex-a55) + rewrite-grow rewrite ONLY ARMv8.3+/v8.4+ non-A55 ops.
set -euo pipefail
SRC=$1; OUT=$2
PIPE=/Users/elfaria/.gemini/antigravity/scratch/a21s-ios27/work/tools/ios27/fullroot/pipeline
HERE=$PIPE/cache
TOOLS=/Users/elfaria/.gemini/antigravity/scratch/a21s-ios27/work/tools/ios27
PY=${PY:-python3}
mkdir -p "$OUT"; cd "$OUT"
C=$OUT/c; PRE=$OUT/pre
echo "== 0 clone stock cache from SSD $(date -u +%T)"
rm -rf "$C" "$PRE" lse.log rewrite.log
cp -c -R "$SRC" "$C"
M=$C/dyld_shared_cache_arm64e
TD=${IOS27_PRIVATE:-$PIPE/../private}
unset TEXTDATA_SKIP; REV=()
[[ -f $TD/textdata-reviewed.list ]] && REV=(--reviewed "$TD/textdata-reviewed.list")
echo "== 0b skip map $(date -u +%T)"
if [[ -f $TD/undef.txt ]]; then
    $PY -I $HERE/textdata-guard.py map "$SRC/dyld_shared_cache_arm64e" --undef "$TD/undef.txt" ${REV[@]+"${REV[@]}"} --out "$OUT/skip.npy"
    export TEXTDATA_SKIP=$OUT/skip.npy
fi
echo "== 1 a64e $(date -u +%T)"; $PY -I $TOOLS/a64e/a64e-rewrite.py cache $M --skip libcorecrypto
echo "== 1b stub islands $(date -u +%T)"; $PY -I $HERE/stubscan.py $M --fix
echo "== 2 lse SKIPPED (all 8 cores are Cortex-A55 with native LSE)"
echo "== 4 a64e-rewrite2 (PACGA/LDRA wb) $(date -u +%T)"; $PY -I $TOOLS/speed/a64e-rewrite2.py cache $M
echo "== 5 cachepatch $(date -u +%T)"
CP="$PY -I $TOOLS/speed/cachepatch.py $C"
PATCHES=$TD/patches.txt
if [[ -f $PATCHES ]]; then while read -r va exp new rest; do [[ $va == \#* || -z $va ]] && continue; $CP $va $exp $new; done < "$PATCHES"; fi
cp -c -R "$C" "$PRE"
echo "== 6 rewrite-grow (ARMv8.3+ only) $(date -u +%T)"; $PY $HERE/aot/rewrite-grow.py cache $M --log "$OUT/rewrite.log" | tee "$OUT/rewrite.out" | tail -5
$PY $HERE/aot/fix-cs.py $M $PRE/dyld_shared_cache_arm64e | tee "$OUT/fixcs.out" | tail -3
echo "== 7 grow-check $(date -u +%T)"; $PY $HERE/aot/grow-check.py $M $PRE/dyld_shared_cache_arm64e "$OUT/rewrite.log" | tee "$OUT/grow-check.out" | tail -12
echo "== 8 text data $(date -u +%T)"
if [[ -n ${TEXTDATA_SKIP:-} ]]; then
    $PY -I $HERE/textdata-guard.py check "$SRC/dyld_shared_cache_arm64e" "$M" --undef "$TD/undef.txt" ${REV[@]+"${REV[@]}"} | tee "$OUT/textdata.out" | tail -5
    $PY -I $HERE/textdata-guard.py verify "$SRC/dyld_shared_cache_arm64e" "$M" --map "$TEXTDATA_SKIP" 2>&1 | tee "$OUT/textdata-verify.out" | tail -5
fi
rm -rf "$PRE"
echo "24A446" > "$OUT/DONE"
echo "CACHE-PIPELINE-DONE $(date -u +%T)"
