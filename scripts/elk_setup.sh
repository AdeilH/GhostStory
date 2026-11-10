#!/usr/bin/env bash
# Ghost Story — one-shot setup of the observability stack on top of a
# RUNNING Elasticsearch + Kibana (see deploy/elk/docker-compose.yml).
#
# Creates, idempotently:
#   1. ILM policy    ghost_story-logs-policy      (delete after 30 days)
#   2. Index template logs-ghost_story              (logs-ghost_story-*)
#   3. Kibana data view "logs-ghost_story-*"        (@timestamp)
#   4. Alert connector  webhook (WEBHOOK_URL) or server-log fallback
#   5. Alert rule    "Ghost Story: many errors"     (.es-query, > threshold)
#
# Env: ES_URL KIBANA_URL WEBHOOK_URL ERROR_THRESHOLD ERROR_WINDOW_MIN
# Reruns update the template/policy/rule in place (safe to re-configure).

set -euo pipefail

ES_URL="${ES_URL:-http://127.0.0.1:9200}"
KIBANA_URL="${KIBANA_URL:-http://127.0.0.1:5601}"
ERROR_THRESHOLD="${ERROR_THRESHOLD:-10}"
ERROR_WINDOW_MIN="${ERROR_WINDOW_MIN:-5}"
# Auth for secured clusters (the bundled dev stack runs with security off).
# KIBANA_USER/KIBANA_PASS fall back to the Elasticsearch credentials.
ES_USER="${ES_USER:-}"
ES_PASS="${ES_PASS:-}"
KIBANA_USER="${KIBANA_USER:-$ES_USER}"
KIBANA_PASS="${KIBANA_PASS:-$ES_PASS}"
RULE_NAME="Ghost Story: many errors"

log() { printf 'elk_setup: %s\n' "$*"; }
die() { printf 'elk_setup: FAIL: %s\n' "$*" >&2; exit 1; }

# JSON field lookup: json_get <json|-> <python-expr-on-d>. Used for the
# name→id lookups below; grep/sed parsing broke on field order and
# silently duplicated objects, so this goes through a real parser.
# "-" (or empty) reads the JSON from stdin.
json_get() {
    local json=$1
    if [[ -z $json || $json == "-" ]]; then json=$(cat); fi
    JSON_DATA=$json python3 -c "
import json, os
d = json.loads(os.environ['JSON_DATA'])
print($2)
" 2>/dev/null
}
command -v python3 >/dev/null 2>&1 ||
    die "python3 required for the Kibana API lookups (apt/pacman install python3)"

ES_CURL=(curl -sS)
KBN_CURL=(curl -sS)
ES_AUTH=""
KBN_AUTH=""
if [[ -n $ES_USER ]]; then
    ES_CURL+=(-u "$ES_USER:$ES_PASS")
    ES_AUTH="$ES_USER:$ES_PASS"
fi
if [[ -n $KIBANA_USER ]]; then
    KBN_CURL+=(-u "$KIBANA_USER:$KIBANA_PASS")
    KBN_AUTH="$KIBANA_USER:$KIBANA_PASS"
fi

wait_for() {
    local name=$1 url=$2 auth=${3:-}
    log "waiting for $name at $url ..."
    for _ in $(seq 1 60); do
        if curl -sS -f ${auth:+-u "$auth"} -o /dev/null "$url" 2>/dev/null; then
            log "$name is up"
            return 0
        fi
        sleep 2
    done
    die "$name not ready at $url (is the stack up? 'make elk-up'; set ES_USER/ES_PASS if secured)"
}

# curl wrapper that surfaces the response body on failure.
api() {
    local base=$1 method=$2 path=$3 payload=${4:-}
    local -a args
    if [[ $base == "$KIBANA_URL" ]]; then
        args=("${KBN_CURL[@]}" -X "$method" "$base$path"
            -H 'Content-Type: application/json' -H 'kbn-xsrf: true')
    else
        args=("${ES_CURL[@]}" -X "$method" "$base$path"
            -H 'Content-Type: application/json')
    fi
    if [[ -n $payload ]]; then args+=(-d "$payload"); fi
    local body status
    body=$("${args[@]}" -w $'\n%{http_code}')
    status=$(printf '%s' "$body" | tail -1)
    body=$(printf '%s' "$body" | sed '$d')
    if [[ ! $status =~ ^2 ]]; then
        printf 'elk_setup: %s %s -> HTTP %s\n%s\n' "$method" "$path" "$status" \
            "$body" >&2
        die "API call failed"
    fi
    printf '%s' "$body"
}

wait_for "Elasticsearch" "$ES_URL/" "$ES_AUTH"
wait_for "Kibana" "$KIBANA_URL/api/status" "$KBN_AUTH"

# Webhook connectors are a Gold+/trial feature. On a basic license the
# connector create would fail with an opaque 400 — check up front and fall
# back to the server-log connector with a clear explanation.
if [[ -n ${WEBHOOK_URL:-} ]]; then
    LICENSE_TYPE=$(curl -sS ${ES_AUTH:+-u "$ES_AUTH"} "$ES_URL/_license" 2>/dev/null |
        json_get - "d.get('license',{}).get('type','unknown')" || echo unknown)
    if [[ $LICENSE_TYPE == basic || $LICENSE_TYPE == unknown ]]; then
        log "WARNING: WEBHOOK_URL is set but .webhook connectors need a Gold+/trial license (cluster license: $LICENSE_TYPE)."
        log "         Falling back to the Kibana server-log connector (alerts appear in the Kibana container log)."
        log "         Enable webhooks on this cluster: curl -XPOST $ES_URL/_license/start_trial?acknowledge=true (free 30-day trial)."
        WEBHOOK_URL=""
    fi
fi

# --- 1. ILM policy: roll the data streams weekly (or at 25 GB) so the
#        delete phase's 30d clock can start; every rolled-over backing
#        index is deleted after 30d.
api "$ES_URL" PUT /_ilm/policy/ghost_story-logs-policy '{"policy":{"phases":{"hot":{"actions":{"rollover":{"max_age":"7d","max_size":"25gb"}}},"delete":{"min_age":"30d","actions":{"delete":{}}}}}}' >/dev/null
log "ILM policy: rollover 7d/25GB, delete backing indices after 30d"

# --- 2. Index template for all three streams (app log, incidents,
#        telemetry). Filebeat 9 ingests these as DATA STREAMS (the
#        logs-ghost_story-* names match its data-stream convention), so
#        the template must be data-stream-enabled and must outrank the
#        built-in logs-*-* template (priority 100) — otherwise our msg
#        text mapping is silently replaced by the built-in's keyword
#        mapping and phrase queries stop working.
api "$ES_URL" PUT /_index_template/logs-ghost_story '{
  "index_patterns": ["logs-ghost_story-*"],
  "priority": 500,
  "data_stream": {},
  "template": {
    "settings": {
      "index.lifecycle.name": "ghost_story-logs-policy",
      "number_of_shards": 1,
      "number_of_replicas": 0
    },
    "mappings": {
      "properties": {
        "@timestamp": {"type": "date"},
        "ts_ns":      {"type": "long"},
        "run":        {"type": "keyword"},
        "lvl":        {"type": "keyword"},
        "tid":        {"type": "keyword"},
        "th":         {"type": "keyword"},
        "sub":        {"type": "keyword"},
        "msg":        {"type": "text", "fields": {"keyword": {"type": "keyword"}}},
        "kind":       {"type": "keyword"},
        "subsystem":  {"type": "keyword"},
        "detail":     {"type": "text", "fields": {"keyword": {"type": "keyword"}}},
        "xruns":             {"type": "long"},
        "max_callback_us":   {"type": "double"},
        "midi_events":       {"type": "long"},
        "crashes_recovered": {"type": "long"},
        "duration_s":        {"type": "double"}
      }
    }
  }
}' >/dev/null
log "index template: logs-ghost_story-* (mappings + ILM attached)"

# --- 3. Kibana data view (Discover time filter = @timestamp).
DATA_VIEW_ID="logs-ghost_story"
if "${KBN_CURL[@]}" -f -o /dev/null \
    "$KIBANA_URL/api/data_views/data_view/$DATA_VIEW_ID" 2>/dev/null; then
    log "data view '$DATA_VIEW_ID' already exists"
else
    api "$KIBANA_URL" POST /api/data_views/data_view \
        '{"data_view":{"id":"'"$DATA_VIEW_ID"'","title":"logs-ghost_story-*","timeFieldName":"@timestamp"}}' \
        >/dev/null
    log "data view: logs-ghost_story-* (time field @timestamp)"
fi

# --- 4. Alert connector: webhook if configured, else Kibana server log.
# The name is type-suffixed so a rerun with a different delivery mode
# creates its own connector instead of silently reusing the old one.
if [[ -n ${WEBHOOK_URL:-} ]]; then
    CONNECTOR_TYPE=".webhook"
    CONNECTOR_NAME="ghost-story-error-webhook"
else
    CONNECTOR_TYPE=".server-log"
    CONNECTOR_NAME="ghost-story-error-server-log"
fi
CONNECTOR_ID=""
CONNECTORS=$("${KBN_CURL[@]}" "$KIBANA_URL/api/actions/connectors" 2>/dev/null || echo '[]')
CONNECTOR_ID=$(json_get "$CONNECTORS" \
    "next((c['id'] for c in d if c['name'] == '$CONNECTOR_NAME'), '')")
if [[ -n $CONNECTOR_ID ]]; then
    log "connector '$CONNECTOR_NAME' already exists ($CONNECTOR_ID)"
fi
if [[ -n ${WEBHOOK_URL:-} ]]; then
    if [[ -z $CONNECTOR_ID ]]; then
        # 9.x webhook config carries url/method/headers only — the payload
        # lives in the rule action's params (ACTION_PARAMS below).
        CONNECTOR_ID=$(api "$KIBANA_URL" POST /api/actions/connector '{
          "name": "'"$CONNECTOR_NAME"'",
          "connector_type_id": "'"$CONNECTOR_TYPE"'",
          "config": {
            "url": "'"$WEBHOOK_URL"'",
            "method": "post",
            "headers": {"Content-Type": "application/json"}
          }
        }' | json_get - "d['id']")
        log "webhook connector created ($CONNECTOR_ID)"
    else
        # A rerun with a changed WEBHOOK_URL must update the stored URL,
        # not silently keep the old one.
        CURRENT_URL=$(json_get "$CONNECTORS" \
            "next((c.get('config',{}).get('url','') for c in d if c['id'] == '$CONNECTOR_ID'), '')")
        if [[ $CURRENT_URL != "$WEBHOOK_URL" ]]; then
            api "$KIBANA_URL" PUT "/api/actions/connector/$CONNECTOR_ID" '{
              "name": "'"$CONNECTOR_NAME"'",
              "config": {
                "url": "'"$WEBHOOK_URL"'",
                "method": "post",
                "headers": {"Content-Type": "application/json"}
              }
            }' >/dev/null
            log "webhook connector URL updated -> $WEBHOOK_URL"
        fi
    fi
    ACTION_PARAMS='{"body":"{\"text\":\"Ghost Story alert: {{rule.name}} — {{context.value}} error/dropped-note log entries within the last '"$ERROR_WINDOW_MIN"' minute(s). {{context.link}}\"}"}'
else
    if [[ -z $CONNECTOR_ID ]]; then
        CONNECTOR_ID=$(api "$KIBANA_URL" POST /api/actions/connector '{"name":"'"$CONNECTOR_NAME"'","connector_type_id":"'"$CONNECTOR_TYPE"'","config":{}}' |
            json_get - "d['id']")
        log "no WEBHOOK_URL set — server-log connector created ($CONNECTOR_ID)"
        log "(alerts will appear in the Kibana container logs: docker compose logs kibana)"
    fi
    ACTION_PARAMS='{"level":"warn","message":"Ghost Story alert: {{rule.name}} — {{context.value}} error/dropped-note log entries within the last '"$ERROR_WINDOW_MIN"' minute(s). {{context.link}}"}'
fi
[[ -n $CONNECTOR_ID ]] || die "could not create/resolve alert connector"

# --- 5. Alert rule: count of error-level entries + note-drop storms over
# the window. The "dropped" phrase match catches WARN lines like
# "synth queue full — note dropped" / "N log entries dropped" — audible
# glitches ship as warns, and they deserve the same notification.
ERROR_QUERY="{\"query\":{\"bool\":{\"filter\":[{\"range\":{\"@timestamp\":{\"gte\":\"now-${ERROR_WINDOW_MIN}m\"}}}],\"should\":[{\"terms\":{\"lvl\":[\"ERROR\",\"CRIT\"]}},{\"match_phrase\":{\"msg\":\"dropped\"}}],\"minimum_should_match\":1}}}"
RULE_PARAMS='{
    "index": ["logs-ghost_story-*"],
    "timeField": "@timestamp",
    "esQuery": "'"${ERROR_QUERY//\"/\\\"}"'",
    "timeWindowSize": '"$ERROR_WINDOW_MIN"',
    "timeWindowUnit": "m",
    "thresholdComparator": ">",
    "threshold": ['"$ERROR_THRESHOLD"'],
    "size": 100
  }'
RULE_ACTIONS='[{"id": "'"$CONNECTOR_ID"'", "group": "query matched", "params": '"$ACTION_PARAMS"'}]'
# Create requires consumer + rule_type_id; the update endpoint (9.x, id in
# the path) rejects both — hence two bodies.
RULE_BODY='{
  "name": "'"$RULE_NAME"'",
  "consumer": "alerts",
  "schedule": {"interval": "1m"},
  "rule_type_id": ".es-query",
  "notify_when": "onActionGroupChange",
  "params": '"$RULE_PARAMS"',
  "actions": '"$RULE_ACTIONS"'
}'
RULE_UPDATE_BODY='{
  "name": "'"$RULE_NAME"'",
  "schedule": {"interval": "1m"},
  "notify_when": "onActionGroupChange",
  "params": '"$RULE_PARAMS"',
  "actions": '"$RULE_ACTIONS"'
}'
# The create API assigns the rule id itself; find an existing rule by name
# so reruns update in place instead of duplicating.
RULES=$("${KBN_CURL[@]}" "$KIBANA_URL/api/alerting/rules/_find?per_page=100" 2>/dev/null || echo '{"data":[]}')
RULE_ID=$(json_get "$RULES" \
    "next((r['id'] for r in d.get('data', []) if r['name'] == '$RULE_NAME'), '')")
if [[ -n $RULE_ID ]]; then
    api "$KIBANA_URL" PUT "/api/alerting/rule/$RULE_ID" "$RULE_UPDATE_BODY" >/dev/null
    log "alert rule updated in place ($RULE_ID)"
else
    api "$KIBANA_URL" POST /api/alerting/rule "$RULE_BODY" >/dev/null
    log "alert rule created"
fi

log "done. Discover: $KIBANA_URL/app/discover (data view: logs-ghost_story)"
log "alert: >$ERROR_THRESHOLD ERROR/CRIT entries within ${ERROR_WINDOW_MIN}m, checked every 1m"
log "smoke test: bash scripts/elk_smoke.sh"
