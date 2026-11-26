# Development build for asnw. The per-directory makefiles still work on
# their own; this one adds out-of-tree builds, sanitizer variants and tests.
#
#   make                  library + wrapper + examples + unit tests (debug)
#   make check            run unit tests (debug build)
#   make check-asan       same under AddressSanitizer + UBSan, plus the
#                         reproducers for open bugs (unittest/kb_*.c), if any
#   make check-tsan       unit tests under ThreadSanitizer
#   make check-valgrind   unit tests under valgrind memcheck
#   make coverage         run the tests in a coverage build, print line
#                         coverage per source file
#   make lint             gcc -Wextra and cppcheck over the library
#   make VARIANT=release  optimized build in build/release
#   make clean
#
# One test only:  make check-asan T=t_echo

VARIANT ?= debug
CC      ?= gcc
B       := build/$(VARIANT)

CFLAGS_COMMON  := -std=gnu11 -g -Wall -Wextra -Wno-unused-parameter -Wno-pointer-arith -Wno-strict-aliasing -MMD -MP -Iasnw -Iwrapper
CFLAGS_debug   := -O0
CFLAGS_release := -O2
CFLAGS_asan    := -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined
CFLAGS_tsan    := -O1 -fno-omit-frame-pointer -fsanitize=thread
CFLAGS_cov     := -O0 --coverage
LDFLAGS_asan   := -fsanitize=address,undefined
LDFLAGS_tsan   := -fsanitize=thread
LDFLAGS_cov    := --coverage

CFLAGS  := $(CFLAGS_COMMON) $(CFLAGS_$(VARIANT))
LDFLAGS := $(LDFLAGS_$(VARIANT))
LDLIBS  := -lev -lpthread

LIB_SRC := $(wildcard asnw/*.c)
LIB_OBJ := $(patsubst %.c,$(B)/%.o,$(LIB_SRC))
LIB     := $(B)/libasnw.a

RPC_SRC := wrapper/ut_rpc.c wrapper/ut_crc32.c wrapper/ut_rpc_clt.c wrapper/ut_rpc_svr.c
RPC_OBJ := $(patsubst %.c,$(B)/%.o,$(RPC_SRC))
RPC_LIB := $(B)/libutrpc.a

EXAMPLE_SRC := $(wildcard example/*.c) $(wildcard c1000k/*.c) wrapper/test_rpc_svr.c wrapper/test_rpc_clt.c
EXAMPLES    := $(patsubst %.c,$(B)/%,$(EXAMPLE_SRC))

UT_SRC := $(wildcard unittest/*.c)
UT_BIN := $(patsubst %.c,$(B)/%,$(UT_SRC))

.PHONY: all lib examples tests check check-asan check-tsan check-valgrind coverage lint clean
.SECONDARY:

all: lib examples tests
lib: $(LIB) $(RPC_LIB)
examples: $(EXAMPLES)
tests: $(UT_BIN)

$(B)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(LIB): $(LIB_OBJ)
	ar rcs $@ $^

$(RPC_LIB): $(RPC_OBJ)
	ar rcs $@ $^

$(B)/%: $(B)/%.o $(RPC_LIB) $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

check: tests
	@unittest/run.sh $(B) $(VARIANT) $(T)

check-asan:
	@$(MAKE) --no-print-directory VARIANT=asan check

check-tsan:
	@$(MAKE) --no-print-directory VARIANT=tsan check

check-valgrind: tests
	@unittest/run.sh $(B) valgrind $(T)

coverage:
	@find build/cov -name '*.gcda' -delete 2>/dev/null || true
	@$(MAKE) --no-print-directory VARIANT=cov check
	@unittest/coverage.sh build/cov

lint:
	$(CC) -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-pointer-arith -fsyntax-only $(LIB_SRC)
	cppcheck --quiet --enable=warning,performance --std=c11 --inline-suppr asnw wrapper

clean:
	rm -rf build

-include $(shell find build -name '*.d' 2>/dev/null)
