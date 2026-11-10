# ghost_story — developer entry point. `make ci` is the M0 gate (see GARDENING.md).
.PHONY: ci build test clean ci-asan ci-tsan elk-up elk-setup elk-smoke elk-down

build:
	@cmake -S . -B build/main -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
	@cmake --build build/main -j$$(nproc)

test: build
	@ctest --test-dir build/main --output-on-failure

ci:
	@bash scripts/ci.sh

ci-asan:
	@cmake -S . -B build/asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DGHOST_STORY_SANITIZE=asan
	@cmake --build build/asan -j$$(nproc)
	@ctest --test-dir build/asan --output-on-failure

ci-tsan:
	@cmake -S . -B build/tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DGHOST_STORY_SANITIZE=tsan
	@cmake --build build/tsan -j$$(nproc)
	@ctest --test-dir build/tsan --output-on-failure

clean:
	@rm -rf build

# Dev observability stack (docs/MANUAL.md, "Log shipping & Kibana").
elk-up:
	@docker compose -f deploy/elk/docker-compose.yml up -d
elk-setup:
	@bash scripts/elk_setup.sh
elk-smoke:
	@bash scripts/elk_smoke.sh
elk-down:
	@docker compose -f deploy/elk/docker-compose.yml down
