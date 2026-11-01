# ---------------------------------------------------------------------------
# Zero-dependency build: only needs clang++ and make (both ship with the
# Xcode Command Line Tools on macOS). CMakeLists.txt is provided as well.
#
#   make            release build of everything into build/release
#   make run        run the demo
#   make test       build + run unit tests
#   make bench      build + run all three benchmarks
#   make debug      -O0 -g build into build/debug (used by the VS Code debugger)
#   make asan       tests under AddressSanitizer + UBSan (memory bugs)
#   make tsan       tests under ThreadSanitizer (data races)
#   make clean
# ---------------------------------------------------------------------------
CXX      := clang++
STD      := -std=c++20
WARN     := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
INC      := -Iinclude
LDLIBS   := -pthread

MODE     ?= release
ifeq ($(MODE),release)
  OPT := -O3 -DNDEBUG
else ifeq ($(MODE),debug)
  OPT := -O0 -g
else ifeq ($(MODE),asan)
  OPT := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
else ifeq ($(MODE),tsan)
  OPT := -O1 -g -fsanitize=thread
endif

CXXFLAGS := $(STD) $(WARN) $(INC) $(OPT)
OUT      := build/$(MODE)

LIB_SRC   := src/order_book.cpp src/matching_engine.cpp
TEST_SRC  := tests/test_main.cpp tests/test_spsc_queue.cpp tests/test_order_book.cpp tests/test_matching_engine.cpp
HEADERS   := $(wildcard include/me/*.hpp) $(wildcard tests/*.hpp) $(wildcard bench/*.hpp)

BINS := $(OUT)/demo $(OUT)/unit_tests $(OUT)/bench_engine $(OUT)/bench_spsc $(OUT)/bench_pipeline

.PHONY: all run test bench debug asan tsan clean
all: $(BINS)

$(OUT)/demo: src/main.cpp $(LIB_SRC) $(HEADERS)
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) src/main.cpp $(LIB_SRC) -o $@ $(LDLIBS)

$(OUT)/unit_tests: $(TEST_SRC) $(LIB_SRC) $(HEADERS)
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -Itests $(TEST_SRC) $(LIB_SRC) -o $@ $(LDLIBS)

$(OUT)/bench_%: bench/bench_%.cpp $(LIB_SRC) $(HEADERS)
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -Ibench $< $(LIB_SRC) -o $@ $(LDLIBS)

run: $(OUT)/demo
	./$(OUT)/demo

test: $(OUT)/unit_tests
	./$(OUT)/unit_tests

bench: $(OUT)/bench_engine $(OUT)/bench_spsc $(OUT)/bench_pipeline
	./$(OUT)/bench_engine
	./$(OUT)/bench_spsc
	./$(OUT)/bench_pipeline

debug:
	$(MAKE) MODE=debug all

asan:
	$(MAKE) MODE=asan build/asan/unit_tests && ./build/asan/unit_tests

tsan:
	$(MAKE) MODE=tsan build/tsan/unit_tests && ./build/tsan/unit_tests

clean:
	rm -rf build
