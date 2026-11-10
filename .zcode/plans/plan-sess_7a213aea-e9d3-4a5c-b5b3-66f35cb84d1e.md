# Kibana log shipping + error-burst alerting for Ghost Story

## Context

Ghost Story's async logger (`src/log/logger.hpp/.cpp`) already emits machine-readable JSONL, but **every production call site passes `""`** — the app never writes a log file, so there is nothing to ship anywhere. The user wants a development observability loop: app logs visible in Kibana, and a notification when many ERROR/CRIT entries occur. Chosen approach: a local Docker Compose stack (Elasticsearch + Kibana + Filebeat), with Kibana alerting wired to a generic webhook connector.

Two app-side gaps must be fixed first:
1. **Timestamps**: `Logger::now_ns()` uses `steady_clock` (ns since boot — meaningless in Kibana). Telemetry/incidents already use `system_clock`.
2. **No default log path / dir creation**: `Logger::init` doesn't create parent dirs and there's no path resolver.

No networking enters the C++ app itself (protects the RT-audio design rule); Filebeat does all shipping. All artifacts are local-first: nothing leaves the machine unless the developer runs the stack and sets a webhook URL.

## 1. App-side changes (C++)

**`src/log/logger.hpp` / `.cpp`**
- `now_ns()`: switch `steady_clock` → `system_clock` (matches `telemetry.cpp:35` pattern; still a vDSO call, RT-safe).
- JSONL schema v2 in `write_entry()` (logger.cpp:110): `{"ts_ns":<wall ns>,"@timestamp":"2026-11-10T12:34:56.789Z","lvl":"…","tid":…,"sub":"…","msg":"…"}` — `@timestamp` built with `std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::milliseconds>(sys_time))` (GCC supports chrono `%` formatting; emitted directly so any shipper indexes with zero conversion).
- Size-capped rotation: `kRotateBytes = 32 MiB`. At `init()`: if existing file ≥ cap, rename `app.jsonl` → `app.jsonl.1` (replace). Writer thread (which owns `file_`) counts appended bytes and rotates mid-session the same way. Include `<filesystem>`, `<system_error>`.
- `init()`: `create_directories` on the file's parent (currently missing).
- New free function `std::string default_log_path()` in namespace `ghost_story::log`, mirroring `telemetry_path()` in `src/ops/telemetry.cpp:16-26` (XDG_DATA_HOME / `~/.local/share`) → `<base>/ghost_story/logs/app.jsonl`.

**`src/app/main.cpp`**
- New small helper in the anonymous namespace: resolve log file = `--log-file PATH` flag > `GHOST_STORY_LOG_FILE` env (`0` = off, other value = path) > `default_log_path()`.
- `cmd_play` (main.cpp:60): `Logger::init(resolved, Level::Info)` instead of `init("", …)`.
- `selftest-record` (main.cpp:263): same resolution — gives the kill9/soak gates persistent post-crash diagnostics.
- On shutdown in `cmd_play`: `log::info("log", "shutdown: {} entries dropped", Logger::dropped_entries())` — makes queue pressure visible in Kibana.
- `print_usage()`: document `--log-file`.

**`tests/test_logger.cpp`**
- Update assertions: `{"ts_ns":` prefix, `\"@timestamp\":\"20` present, ISO format sanity.
- New test `rotates_oversized_file`: `std::filesystem::resize_file(path, kRotateBytes + 1)` → `init` → old content lives in `.path + ".1"`, new file starts empty.

## 2. Observability stack — new `deploy/elk/`

- **`docker-compose.yml`** (ELASTIC_VERSION via env, default `8.17.4`):
  - `elasticsearch`: single node, `xpack.security.enabled=false` (dev-only, ports bound to `127.0.0.1:9200`), `ES_JAVA_OPTS=-Xms1g -Xmx1g`.
  - `kibana`: `127.0.0.1:5601`, points at the ES service.
  - `filebeat`: mounts `./filebeat.yml:ro` and `${GHOST_STORY_DATA_DIR:-$HOME/.local/share/ghost_story}:/data:ro`.
- **`filebeat.yml`**: two `filestream` inputs with `ndjson` parser — `/data/logs/app.jsonl` and `/data/incidents.jsonl` → ES indices `logs-ghost_story-app` / `logs-ghost_story-incidents` (via `output.elasticsearch.indices` + `when.contains log.file.name`). `setup.template.enabled: false`, `setup.ilm.enabled: false` (template managed by setup script).
- **`.env.example`**: `ELASTIC_VERSION`, `ERROR_THRESHOLD`, `ERROR_WINDOW_MIN`, `WEBHOOK_URL`.

## 3. New `scripts/elk_setup.sh` (env-driven, curl, honest failure output)

Env: `ES_URL` (default `http://127.0.0.1:9200`), `KIBANA_URL` (default `http://127.0.0.1:5601`), `WEBHOOK_URL` (optional), `ERROR_THRESHOLD` (default 10), `ERROR_WINDOW_MIN` (default 5). Waits for ES/Kibana readiness, then idempotently:
1. `PUT _ilm/policy/ghost_story-logs-policy` — delete phase, `min_age: 30d`.
2. `PUT _index_template/logs-ghost_story` — pattern `logs-ghost_story-*`; mappings: `@timestamp` date, `ts_ns` long, `lvl` keyword, `sub` keyword, `msg` text + `.keyword` subfield, `tid` long; plus incidents fields `kind`/`subsystem`/`detail` keywords.
3. Kibana data view `logs-ghost_story-*` with time field `@timestamp` (`POST /api/data_views/data_view`; skip if GET says it exists).
4. If `WEBHOOK_URL` set: webhook connector (`.webhook`, POST, mustache body referencing `{{rule.name}}`/`{{context.value}}`); otherwise fall back to a `.server-log` connector so the rule works zero-config.
5. Alert rule via `POST /api/alerting/rule`: `rule_type_id: es-query`, index `logs-ghost_story-*`, query `lvl:(ERROR OR CRIT)`, `aggregationType: count`, comparator `> ERROR_THRESHOLD`, window `ERROR_WINDOW_MIN` minutes, schedule every 1m, `notifyWhen: onActionGroupChange` (no spam), action → connector from step 4.
Prints the Kibana Discover URL and any non-2xx response bodies.

## 4. New `scripts/elk_smoke.sh` (end-to-end gate, no audio hardware needed)

1. Computes the same XDG log path as the app; appends 30 schema-v2 synthetic lines (25 INFO + 5 ERROR, `sub:"smoke"`, real wall-clock `@timestamp`).
2. Polls ES `_search` for `sub:smoke` docs until all 30 indexed (60 s timeout, exit 1 otherwise) — proves compose → Filebeat → template → index pipeline.
3. Verifies data view and alert rule exist via Kibana API.
4. Prints counts + `PASS`/`FAIL`; suitable as a manual gate (`make elk-smoke`), **not** added to `scripts/ci.sh` (Docker too heavy for the 8-stage CI gate).

## 5. Integration glue

- **`Makefile`**: targets `elk-up`, `elk-down`, `elk-setup`, `elk-smoke` (thin wrappers, matching the existing `ci`/`test` style).
- **`docs/MANUAL.md`**: new "Log shipping & Kibana (development observability)" section — quickstart (`make elk-up && make elk-setup`, open Kibana), KQL examples (`lvl: ERROR`, `sub: audio and xrun`), env-var table (thresholds, webhook), rotation behavior, privacy note (local-first, `GHOST_STORY_LOG_FILE=0` opt-out parity with `GHOST_STORY_TELEMETRY`), and the alert rule semantics.
- **`README.md`** ops line, **`CHANGELOG.md`** entry (match existing format), **`docs/ghost_story.1`** FILES entry for `logs/app.jsonl` + `--log-file`.

## Files

| Action | Path |
|---|---|
| Modify | `src/log/logger.hpp`, `src/log/logger.cpp`, `src/app/main.cpp`, `tests/test_logger.cpp` |
| Create | `deploy/elk/docker-compose.yml`, `deploy/elk/filebeat.yml`, `deploy/elk/.env.example` |
| Create | `scripts/elk_setup.sh`, `scripts/elk_smoke.sh` |
| Modify | `Makefile`, `docs/MANUAL.md`, `README.md`, `CHANGELOG.md`, `docs/ghost_story.1` |

No CMake changes needed (no new C++ files/deps; `logger.cpp` is already in `ghost_story_core`).

## Verification

1. `cmake --build build/main && ctest` from `build/main` — updated + new logger tests pass (also in ASan/TSan configs if handy).
2. `docker compose -f deploy/elk/docker-compose.yml up -d` → `bash scripts/elk_setup.sh` → `bash scripts/elk_smoke.sh` — expect PASS and 30 docs in `logs-ghost_story-app` (only if Docker available; scripts fail loudly otherwise).
3. Real-data check: `ghost_story play --headless --seconds 5` → `cat ~/.local/share/ghost_story/logs/app.jsonl` shows v2 lines → smoke script ingests them → visible in Kibana Discover.
4. Alert path: set `ERROR_THRESHOLD=1`, run play, manually append ERROR lines past threshold → webhook receives a POST (or `.server-log` fallback logs the alert).