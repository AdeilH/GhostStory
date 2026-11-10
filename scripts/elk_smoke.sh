#!/usr/bin/env bash
# Ghost Story — end-to-end gate for the log pipeline (no audio hardware
# needed):
#
#   app.jsonl (schema v3)    → Filebeat → Elasticsearch → Kibana data view
#   telemetry/sessions.jsonl → Filebeat → Elasticsearch    + alert rule
#   incidents.jsonl          → Filebeat → Elasticsearch
#
# Requires the stack up and set up first:  make elk-up && make elk-setup
# Uses LOG_FILE / XDG_DATA_HOME env if set; otherwise the default app path.

set -euo pipefail

ES_URL="${ES_URL:-http://127.0.0.1:9200}"
KIBANA_URL="${KIBANA_URL:-http://127.0.0.1:5601}"
# Auth for secured clusters (the bundled dev stack runs with security off).
ES_USER="${ES_USER:-}"
ES_PASS="${ES_PASS:-}"
KIBANA_USER="${KIBANA_USER:-$ES_USER}"
KIBANA_PASS="${KIBANA_PASS:-$ES_PASS}"
DATA_BASE="${XDG_DATA_HOME:-$HOME/.local/share}"
LOG_FILE="${LOG_FILE:-$DATA_BASE/ghost_story/logs/app.jsonl}"
TELEMETRY_FILE="${TELEMETRY_FILE:-$DATA_BASE/ghost_story/telemetry/sessions.jsonl}"
SMOKE_TOTAL=30
SMOKE_ERRORS=5
POLL_TIMEOUT_S=90

ok() { printf 'elk_smoke: %s\n' "$*"; }
pass() { printf 'elk_smoke: PASS — %s\n' "$*"; exit 0; }
fail() { printf 'elk_smoke: FAIL — %s\n' "$*" >&2; exit 1; }

ES_CURL=(curl -sS)
KBN_CURL=(curl -sS)
if [[ -n $ES_USER ]]; then ES_CURL+=(-u "$ES_USER:$ES_PASS"); fi
if [[ -n $KIBANA_USER ]]; then KBN_CURL+=(-u "$KIBANA_USER:$KIBANA_PASS"); fi

for cmd in curl date; do
    command -v "$cmd" >/dev/null 2>&1 || fail "$cmd not available"
done

# --- 1. Append synthetic schema-v3 lines exactly as the app writes them
#       (run id + thread label), plus one WARN "dropped" line — the same
#       phrase the widened alert query matches.
mkdir -p "$(dirname "$LOG_FILE")" "$(dirname "$TELEMETRY_FILE")"
for i in $(seq 1 "$SMOKE_TOTAL"); do
    lvl=INFO
    [ "$i" -gt $((SMOKE_TOTAL - SMOKE_ERRORS)) ] && lvl=ERROR
    printf '{"ts_ns":%s,"@timestamp":"%s","run":"deadbeef","lvl":"%s","tid":7,"th":"smoke","sub":"smoke","msg":"smoke entry %d of %d"}\n' \
        "$(date -u +%s%N)" "$(date -u +%Y-%m-%dT%H:%M:%S.%3NZ)" "$lvl" \
        "$i" "$SMOKE_TOTAL" >>"$LOG_FILE"
done
printf '{"ts_ns":%s,"@timestamp":"%s","run":"deadbeef","lvl":"WARN","tid":7,"th":"audio","sub":"smoke","msg":"synth queue full — note dropped"}\n' \
    "$(date -u +%s%N)" "$(date -u +%Y-%m-%dT%H:%M:%S.%3NZ)" >>"$LOG_FILE"
# One synthetic session_end as telemetry.cpp writes it (with @timestamp).
printf '{"ts_ns":%s,"@timestamp":"%s","kind":"session_end","xruns":0,"max_callback_us":512.0,"midi_events":42,"crashes_recovered":0,"duration_s":30.0}\n' \
    "$(date -u +%s%N)" "$(date -u +%Y-%m-%dT%H:%M:%S.%3NZ)" >>"$TELEMETRY_FILE"
printf 'elk_smoke: appended %d lines (%d ERROR, 1 dropped-WARN) to %s + 1 session_end\n' \
    "$SMOKE_TOTAL" "$SMOKE_ERRORS" "$LOG_FILE"

# --- 2. Poll Elasticsearch until Filebeat has indexed them.
# Always prints a number, even when the cluster is unreachable.
es_count() {
    local index=$1 q=$2 n
    n=$("${ES_CURL[@]}" "$ES_URL/$index/_search" \
        -H 'Content-Type: application/json' -d "$q" 2>/dev/null |
        sed -n 's/.*"value":\([0-9]*\).*/\1/p' | head -1)
    printf '%s' "${n:-0}"
}
ALL_Q='{"query":{"term":{"sub":"smoke"}},"size":0,"track_total_hits":true}'
ERR_Q='{"query":{"bool":{"filter":[{"term":{"sub":"smoke"}},{"terms":{"lvl":["ERROR","CRIT"]}}]}},"size":0,"track_total_hits":true}'
ALERT_Q='{"query":{"bool":{"filter":[{"term":{"sub":"smoke"}},{"range":{"@timestamp":{"gte":"now-10m"}}}],"should":[{"terms":{"lvl":["ERROR","CRIT"]}},{"match_phrase":{"msg":"dropped"}}],"minimum_should_match":1}},"size":0,"track_total_hits":true}'
TEL_Q='{"query":{"term":{"kind":"session_end"}},"size":0,"track_total_hits":true}'
count=$(es_count logs-ghost_story-app "$ALL_Q")
deadline=$((SECONDS + POLL_TIMEOUT_S))
while [ "$count" -lt "$SMOKE_TOTAL" ] && [ "$SECONDS" -lt "$deadline" ]; do
    sleep 3
    count=$(es_count logs-ghost_story-app "$ALL_Q")
done
[ "$count" -ge "$SMOKE_TOTAL" ] ||
    fail "only $count/$SMOKE_TOTAL smoke docs indexed after ${POLL_TIMEOUT_S}s (filebeat output? 'docker compose -f deploy/elk/docker-compose.yml logs filebeat')"
ok "$count/$SMOKE_TOTAL smoke docs indexed (sub:smoke)"

# --- 3. Error sub-path: the ERROR half must be queryable via lvl.
err=$(es_count logs-ghost_story-app "$ERR_Q")
[ "$err" -ge "$SMOKE_ERRORS" ] ||
    fail "only $err/$SMOKE_ERRORS ERROR-level docs indexed (lvl mapping?)"
ok "$err ERROR docs visible to the alert rule query (lvl filter works)"

# --- 4. Alert-query sub-path: the dropped-WARN must match the widened
#       should-query (this is what makes note-drop storms notify).
alert=$(es_count logs-ghost_story-app "$ALERT_Q")
[ "$alert" -ge $((SMOKE_ERRORS + 1)) ] ||
    fail "only $alert docs match the widened alert query (match_phrase on msg broken?)"
ok "$alert docs match the widened alert query (dropped-WARN included)"

# --- 5. Telemetry stream: numeric session stats land in their own index.
tel=$(es_count logs-ghost_story-telemetry "$TEL_Q")
[ "$tel" -ge 1 ] ||
    fail "no session_end doc in logs-ghost_story-telemetry (filebeat telemetry input?)"
ok "telemetry stream indexed ($tel session_end docs — xruns/callback_us chartable)"

# --- 6. Kibana wiring: data view + alert rule exist.
"${KBN_CURL[@]}" -f -o /dev/null \
    "$KIBANA_URL/api/data_views/data_view/logs-ghost_story" ||
    fail "data view 'logs-ghost_story' missing (run scripts/elk_setup.sh)"
"${KBN_CURL[@]}" -f "$KIBANA_URL/api/alerting/rules/_find?per_page=100" |
    grep -q '"name":"Ghost Story: many errors"' ||
    fail "alert rule 'Ghost Story: many errors' missing (run scripts/elk_setup.sh)"
ok "data view + alert rule present"
printf 'elk_smoke: Discover → %s/app/discover (data view: logs-ghost_story)\n' "$KIBANA_URL"
pass "pipeline end-to-end"
