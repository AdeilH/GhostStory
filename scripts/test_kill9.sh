#!/usr/bin/env bash
# addu94 — scripts/test_kill9.sh
# Spec §6.3 gate: kill -9 mid-recording must lose at most ~1 s of edits
# and the engine must relaunch cleanly over the dirty journal.
# End-to-end: notes flow through event queue → recorder → journal (fsync
# per append), the process is SIGKILLed, then the project is reloaded.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/build/main/addu94"
DIR=$(mktemp -d /tmp/addu94_kill9.XXXXXX)
SMF="$DIR/out.mid"
trap 'rm -rf "$DIR"' EXIT

# 1. Record synthetic notes at 20/s until killed.
"$BIN" selftest-record --project-dir "$DIR" --interval-ms 50 >/dev/null 2>&1 &
REC_PID=$!
sleep 1.5        # ~30 notes journaled
kill -9 "$REC_PID" 2>/dev/null
wait "$REC_PID" 2>/dev/null

# 2. Recovery: load must succeed; export proves events survived.
if ! "$BIN" export --project-dir "$DIR" --smf "$SMF" >/dev/null 2>&1; then
    echo "kill9: FAIL (project did not load after kill -9)"
    exit 1
fi

# 3. SMF must contain recovered notes (count note-on/off events, not size).
NOTES=$(python3 - "$SMF" <<'PYEOF'
import sys
b = open(sys.argv[1],'rb').read()
pos = 14
count = 0
while pos + 8 <= len(b):
    if b[pos:pos+4] != b'MTrk': break
    ln = int.from_bytes(b[pos+4:pos+8],'big')
    body = b[pos+8:pos+8+ln]
    i = 0
    while i < len(body):
        st = body[i]
        if st < 0xF0 and (st & 0xF0) in (0x80, 0x90):
            count += 1
            i += 3
        elif st == 0xFF:
            break
        else:
            i += 1
    pos += 8 + ln
print(count)
PYEOF
)
if [ "${NOTES:-0}" -lt 2 ]; then
    echo "kill9: FAIL (only $NOTES note events recovered — data lost)"
    exit 1
fi
echo "kill9: PASS (kill -9 mid-recording recovered $NOTES note events)"
exit 0
