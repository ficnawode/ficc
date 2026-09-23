CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -Isrc -Itest \
           -DFICC_BUILTIN_INCLUDE=\"$(abspath include)\"
DEPFLAGS := -MMD -MP
LDFLAGS :=

COMPILE_COMMANDS ?= 1
ifeq ($(COMPILE_COMMANDS),1)
  BEAR := $(shell command -v bear)
  ifeq ($(BEAR),)
    $(error COMPILE_COMMANDS=1 requires 'bear'; install it via your package manager or `pip install bear`)
  endif
  BEAR_RUN = $(BEAR) --append --output $(BUILD_DIR)/compile_commands.json --
else
  BEAR_RUN :=
endif


BUILD_DIR    := build
OBJ_DIR      := $(BUILD_DIR)/obj
TEST_OBJ_DIR := $(BUILD_DIR)/obj-test
BIN_DIR      := $(BUILD_DIR)/bin

SRC_ALL  := $(wildcard src/*.c) $(wildcard src/util/*.c) $(wildcard src/optpasses/*.c)
SRC_TEST := test/runner.c test/testdriver.c test/dwarfcheck.c \
            $(wildcard test/unit/*.c) $(wildcard test/compile/*.c)

OBJ_SRC       := $(patsubst src/%.c,$(OBJ_DIR)/src/%.o,$(SRC_ALL))
OBJ_TEST_SRC  := $(patsubst src/%.c,$(TEST_OBJ_DIR)/src/%.o,$(filter-out src/driver.c,$(SRC_ALL)))
OBJ_TEST_TEST := $(patsubst test/%.c,$(TEST_OBJ_DIR)/test/%.o,$(SRC_TEST))

FICC_BIN := $(BIN_DIR)/ficc
TEST_BIN := $(BIN_DIR)/test_runner

# test/compile/test_driver.c runs the real ficc binary through tc_run_shell.
TEST_CFLAGS := $(CFLAGS) -DFICC_BIN=\"$(abspath $(FICC_BIN))\"

DIRS := $(sort $(dir $(OBJ_SRC) $(OBJ_TEST_SRC) $(OBJ_TEST_TEST) $(FICC_BIN) $(TEST_BIN)))

FORMAT_FILES := $(shell find src include test \( -name '*.c' -o -name '*.h' \))

.PHONY: all clean test test-gdb selftest dirs compile-commands format format-check git-sweep git-smoke

GIT ?= ../git

all: $(FICC_BIN)

format:
	clang-format -i $(FORMAT_FILES)

format-check:
	clang-format --dry-run --Werror $(FORMAT_FILES)

test: $(TEST_BIN) $(FICC_BIN)
	$(TEST_BIN)

# Real-debugger certificate (opt-in; skips with a warning when gdb is absent).
test-gdb: $(FICC_BIN)
	./test/debug_gdb.sh $(abspath $(FICC_BIN))

# Phase 23 progress meter: compile every Git translation unit, bucket failures.
git-sweep: $(FICC_BIN)
	FICC=$(abspath $(FICC_BIN)) GIT=$(abspath $(GIT)) ./test/git-sweep.sh

# Functional certificate for a ficc-built git binary (see test/git-cc.sh).
git-smoke:
	GIT=$(abspath $(GIT)) ./test/git-smoke.sh

$(FICC_BIN): $(OBJ_SRC) | dirs
	$(CC) $(LDFLAGS) $^ -o $@

$(TEST_BIN): $(OBJ_TEST_SRC) $(OBJ_TEST_TEST) | dirs
	$(CC) $(LDFLAGS) $^ -o $@

$(OBJ_DIR)/src/%.o: src/%.c | dirs
	$(BEAR_RUN) $(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(TEST_OBJ_DIR)/src/%.o: src/%.c | dirs
	$(BEAR_RUN) $(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(TEST_OBJ_DIR)/test/%.o: test/%.c | dirs
	$(BEAR_RUN) $(CC) $(TEST_CFLAGS) $(DEPFLAGS) -c $< -o $@

TEST_DEPS := $(wildcard $(TEST_OBJ_DIR)/test/*.d $(TEST_OBJ_DIR)/test/*/*.d)
-include $(OBJ_DIR)/src/*.d $(TEST_OBJ_DIR)/src/*.d $(TEST_DEPS)

dirs:
	@mkdir -p $(DIRS)

clean:
	rm -rf $(BUILD_DIR)

compile-commands:
	rm -f $(BUILD_DIR)/compile_commands.json
	$(MAKE) clean
	$(MAKE) COMPILE_COMMANDS=1 all test

# --- Self-compilation / bootstrap check (phase 18: uses -o, per-stage object trees) ---
#
# stage 1: compile every source with the gcc-built ficc   -> build/selftest/obj/stage1
# stage 2: rebuild every source with ficc1 itself         -> build/selftest/obj/stage2
#
# Each stage writes objects to its own dir via `-c src -o obj/stageN/<src>.o`, so stage 2
# no longer overwrites stage 1. Bootstrap correctness = stage1 and stage2 OBJECT trees
# are byte-identical (stricter, and independent of the external linker).
#
# Both stages compile at -O1 so the bootstrap also exercises the optimizer; a
# self-host miscompile changes the generated code between stage 1 and stage 2.
#
# We still cd into the staging dir so __FILE__-derived strings match across stages, and
# still force -no-pie on the final links because ficc objects carry R_X86_64_32S data
# relocations; both vanish when an internal linker replaces the external ld step.
SELF_DIR  := $(BUILD_DIR)/selftest
SELF_SRC  := $(SELF_DIR)/src
SELF_OBJ1 := $(SELF_DIR)/obj/stage1
SELF_OBJ2 := $(SELF_DIR)/obj/stage2
FICC1_BIN := $(SELF_DIR)/ficc1
FICC2_BIN := $(SELF_DIR)/ficc2
SELF_FILES := $(wildcard src/*.c src/*.h src/util/*.c src/util/*.h \
                     src/optpasses/*.c src/optpasses/*.h)

$(SELF_DIR)/.staged: $(SELF_FILES) | dirs
	@rm -rf $(SELF_DIR)
	@mkdir -p $(SELF_SRC)/util $(SELF_SRC)/optpasses
	@cp src/*.c src/*.h $(SELF_SRC)/
	@cp -r src/util/. $(SELF_SRC)/util/
	@cp -r src/optpasses/. $(SELF_SRC)/optpasses/
	@touch $@

# $1 = compiler binary, $2 = target object dir. Compile each source from the staged
# dir (relative names, so __FILE__ matches) but write each .o into $(2). -I. covers
# the staged src root, so optpasses/ sources can reach sibling and util headers.
define self-compile-loop
	@rm -rf $(2) && mkdir -p $(2)/util $(2)/optpasses
	@cd $(SELF_SRC) && for c in *.c util/*.c $$(ls optpasses/*.c 2>/dev/null); do \
		SOURCE_DATE_EPOCH=0 $(1) -O1 -I. -c "-DFICC_BUILTIN_INCLUDE=\"$(abspath include)\"" \
			"$$c" -o "$(2)/$${c%.c}.o" \
		|| { echo "selftest: stage $(2) failed to compile $$c"; exit 1; }; \
	done
endef

selftest: $(FICC_BIN) $(SELF_DIR)/.staged
	@echo "== selftest stage 1: compile with $(notdir $(FICC_BIN)) =="
	$(call self-compile-loop,$(abspath $(FICC_BIN)),$(abspath $(SELF_OBJ1)))
	@$(CC) -no-pie $(LDFLAGS) $(SELF_OBJ1)/*.o $(SELF_OBJ1)/util/*.o $(SELF_OBJ1)/optpasses/*.o -o $(FICC1_BIN)
	@echo "== selftest stage 2: rebuild with ficc1 =="
	$(call self-compile-loop,$(abspath $(FICC1_BIN)),$(abspath $(SELF_OBJ2)))
	@$(CC) -no-pie $(LDFLAGS) $(SELF_OBJ2)/*.o $(SELF_OBJ2)/util/*.o $(SELF_OBJ2)/optpasses/*.o -o $(FICC2_BIN)
	@if diff -r --brief $(SELF_OBJ1) $(SELF_OBJ2); then \
		echo "selftest: OK — stage1 and stage2 object trees are byte-identical"; \
	else \
		echo "selftest: FAIL — bootstrap objects differ"; \
		diff -r $(SELF_OBJ1) $(SELF_OBJ2); \
		exit 1; \
	fi
