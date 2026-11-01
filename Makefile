# itchbook — common entry points. J defaults to half the logical CPUs.
J ?= $(shell n=$$( (sysctl -n hw.ncpu 2>/dev/null || nproc) ); echo $$(( n/2 > 2 ? n/2 : 2 )))
BUILD ?= build
DATA_DIR ?= $(HOME)/quantfactory/_data/itch
ITCH ?= $(DATA_DIR)/01302019.NASDAQ_ITCH50.gz
SYMBOL ?= AAPL

.PHONY: build test bench micro lint fuzz demo data verify sanitize tsan clean

build:
	cmake -S . -B $(BUILD) -G Ninja -DCMAKE_BUILD_TYPE=Release -DITCHBOOK_NATIVE=ON
	cmake --build $(BUILD) -j $(J)

test: build
	ctest --test-dir $(BUILD) -j $(J) --output-on-failure

# ASan + UBSan build and test run.
sanitize:
	cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DITCHBOOK_SANITIZE=address,undefined -DITCHBOOK_BENCH=OFF
	cmake --build build-asan -j $(J)
	ctest --test-dir build-asan -j $(J) --output-on-failure

tsan:
	cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DITCHBOOK_SANITIZE=thread -DITCHBOOK_BENCH=OFF
	cmake --build build-tsan -j $(J)
	ctest --test-dir build-tsan -j $(J) --output-on-failure

micro: build
	$(BUILD)/itchbook-bench --benchmark_repetitions=5 --benchmark_report_aggregates_only=true

# Full-day replay benchmark suite on real data (run on an idle machine).
bench: build
	ITCH=$(ITCH) BUILD=$(BUILD) bash bench/run_replay_bench.sh

verify: build
	$(BUILD)/itchbook verify $(ITCH) --depth 10 --full-every 1000

lint:
	@command -v clang-format >/dev/null && clang-format --dry-run --Werror include/itchbook/*.hpp src/*.cpp tools/*.cpp tests/*.cpp tests/*.hpp bench/*.cpp fuzz/*.cpp || echo "clang-format not installed; skipping"
	@command -v clang-tidy >/dev/null && clang-tidy -p $(BUILD) src/*.cpp tools/itchbook.cpp || echo "clang-tidy not installed; skipping"

# libFuzzer needs a clang with the fuzzer runtime (Homebrew/upstream LLVM, or Linux clang).
FUZZ_CXX ?= clang++
fuzz:
	cmake -S . -B build-fuzz -G Ninja -DCMAKE_CXX_COMPILER=$(FUZZ_CXX) -DCMAKE_BUILD_TYPE=RelWithDebInfo -DITCHBOOK_FUZZ=ON -DITCHBOOK_BENCH=OFF
	cmake --build build-fuzz --target fuzz-parser -j $(J)
	mkdir -p fuzz/corpus && build-fuzz/fuzz-parser fuzz/corpus -max_total_time=$${FUZZ_SECONDS:-300} -rss_limit_mb=2048

data:
	ITCH_DATA_DIR=$(DATA_DIR) bash scripts/fetch_itch.sh 01302019

# Live top-of-book replay; falls back to synthetic data if the NASDAQ file is absent.
demo: build
	@if [ -f "$(ITCH)" ]; then \
	  $(BUILD)/itchbook top $(ITCH) --symbol $(SYMBOL) --from 09:30 --every-ms 250 --frames 40 --sleep-ms 120; \
	else \
	  echo "NASDAQ sample not found ($(ITCH)); using synthetic flow. Run 'make data' for real data."; \
	  $(BUILD)/itchbook gen $(BUILD)/synthetic.itch.gz --messages 2000000 && \
	  $(BUILD)/itchbook top $(BUILD)/synthetic.itch.gz --symbol SYM0 --every-ms 1 --frames 40 --sleep-ms 120; \
	fi

clean:
	rm -rf build build-asan build-tsan build-fuzz
