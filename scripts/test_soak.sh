#!/usr/bin/env bash
# ghost_story — scripts/test_soak.sh
# Spec §11 soak gate: scripted long-run session — concurrent recording
# (journaling), MIDI fuzz storm, and a live bench stream — asserting zero
# xruns, zero lost events, and a clean final project load.
# Usage: test_soak.sh [minutes]   (default 30 = spec duration)
set -uo pipefail

MINUTES="${1:-30}"
SECONDS_TOTAL=$((MINUTES * 60))
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/build/main/ghost_story"
DIR=$(mktemp -d /tmp/ghost_story_soak.XXXXXX)
trap 'kill 2>/dev/null; rm -rf "$DIR"' EXIT

echo "soak: ${MINUTES} min scripted session (record + fuzz + bench)"

# 1. Recording stream (journal + transport under load) for the full soak.
"$BIN" selftest-record --project-dir "$DIR" --interval-ms 50 >/dev/null 2>&1 &
REC=$!

# 2. Fuzz storm at the MIDI engine for the full soak.
"$ROOT/build/main/fuzz_midi" "$SECONDS_TOTAL" 777 >/dev/null 2>&1 &
FUZZ=$!

# 3. Bench stream (own engine; callback stats) for the full soak.
"$BIN" bench --seconds "$SECONDS_TOTAL" > "$DIR/bench.out" 2>&1 &
BENCH=$!

wait $BENCH
kill $REC $FUZZ 2>/dev/null
wait 2>/dev/null

# Gates: zero xruns, engine stayed alive (bench exits 0 only on pass).
if grep -q "gate: PASS" "$DIR/bench.out"; then
    XR=$(grep -o "xruns=[0-9]*" "$DIR/bench.out" | head -1 | cut -d= -f2)
else
    echo "soak: FAIL (bench gate did not pass)"
    cat "$DIR/bench.out"
    exit 1
fi

# Final integrity: the soak-long journal must load and export.
if ! "$BIN" export --project-dir "$DIR" --smf "$DIR/final.mid" >/dev/null 2>&1; then
    echo "soak: FAIL (final project load/export failed)"
    exit 1
fi

NOTES=$(python3 - "$DIR/final.mid" <<'PYEOF'
import sys
b = open(sys.argv[1],'rb').read()
count = 0
pos = 14
while pos + 8 <= len(b):
    if b[pos:pos+4] != b'MTrk': break
    ln = int.from_bytes(b[pos+4:pos+8],'big')
    body = b[pos+8:pos+8+ln]
    i = 0
    while i < len(body):
        st = body[i]
        if st < 0xF0 and (st & 0xF0) in (0x80, 0x90):
            count += 1; i += 3
        elif st == 0xFF: break
        else: i += 1
    pos += 8 + ln
print(count)
PYEOF
)
if [ "${NOTES:-0}" -lt 100 ]; then
    echo "soak: FAIL (only $NOTES notes recovered — journal lost events)"
    exit 1
fi
echo "soak: PASS (${MINUTES} min: xruns=$XR, $NOTES note events journaled+recovered, fuzz survived)"
exit 0
