# addu94 — developer entry point. `make ci` is the M0 gate (see GARDENING.md).
.PHONY: ci build test clean ci-asan ci-tsan

build:
	@cmake -S . -B build/main -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
	@cmake --build build/main -j$$(nproc)

test: build
	@ctest --test-dir build/main --output-on-failure

ci:
	@bash scripts/ci.sh

ci-asan:
	@cmake -S . -B build/asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DADDU94_SANITIZE=asan
	@cmake --build build/asan -j$$(nproc)
	@ctest --test-dir build/asan --output-on-failure

ci-tsan:
	@cmake -S . -B build/tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DADDU94_SANITIZE=tsan
	@cmake --build build/tsan -j$$(nproc)
	@ctest --test-dir build/tsan --output-on-failure

clean:
	@rm -rf build
