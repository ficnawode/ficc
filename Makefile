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

SRC_ALL  := $(wildcard src/*.c) $(wildcard src/util/*.c)
SRC_TEST := test/runner.c test/testdriver.c $(wildcard test/unit/*.c) $(wildcard test/compile/*.c)

OBJ_SRC       := $(patsubst src/%.c,$(OBJ_DIR)/src/%.o,$(SRC_ALL))
OBJ_TEST_SRC  := $(patsubst src/%.c,$(TEST_OBJ_DIR)/src/%.o,$(filter-out src/driver.c,$(SRC_ALL)))
OBJ_TEST_TEST := $(patsubst test/%.c,$(TEST_OBJ_DIR)/test/%.o,$(SRC_TEST))

FICC_BIN := $(BIN_DIR)/ficc
TEST_BIN := $(BIN_DIR)/test_runner

DIRS := $(sort $(dir $(OBJ_SRC) $(OBJ_TEST_SRC) $(OBJ_TEST_TEST) $(FICC_BIN) $(TEST_BIN)))

.PHONY: all clean test selftest dirs compile-commands

all: $(FICC_BIN)

test: $(TEST_BIN)
	$(TEST_BIN)

$(FICC_BIN): $(OBJ_SRC) | dirs
	$(CC) $(LDFLAGS) $^ -o $@

$(TEST_BIN): $(OBJ_TEST_SRC) $(OBJ_TEST_TEST) | dirs
	$(CC) $(LDFLAGS) $^ -o $@

$(OBJ_DIR)/src/%.o: src/%.c | dirs
	$(BEAR_RUN) $(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(TEST_OBJ_DIR)/src/%.o: src/%.c | dirs
	$(BEAR_RUN) $(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(TEST_OBJ_DIR)/test/%.o: test/%.c | dirs
	$(BEAR_RUN) $(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

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

# --- Self-compilation / bootstrap check ---
#
# Stage 1: compile every ficc source with the gcc-built ficc, link with gcc ->
# ficc1. Stage 2: rebuild every source *with ficc1 itself*, link with gcc ->
# ficc2. A correct, deterministic self-hosting compiler must produce a ficc2
# byte-identical to ficc1; any mismatch (or crash) is a bootstrap failure.
#
# Both stages compile the same staged copy of src/ using relative names, so
# __FILE__-derived constants match; SOURCE_DATE_EPOCH pins __DATE__/__TIME__
# so the comparison does not depend on the wall clock. ficc emits each object
# next to its source, so stage 2 overwrites the stage-1 objects in place.
#
# ficc-produced objects contain R_X86_64_32S data-facing relocations, so the
# link is forced -no-pie (plain `make` builds gcc objects and is unaffected).
SELF_DIR  := $(BUILD_DIR)/selftest
SELF_SRC  := $(SELF_DIR)/src
FICC1_BIN := $(SELF_DIR)/ficc1
FICC2_BIN := $(SELF_DIR)/ficc2
SELF_FILES := $(wildcard src/*.c src/*.h src/util/*.c src/util/*.h)

$(SELF_DIR)/.staged: $(SELF_FILES) | dirs
	@rm -rf $(SELF_DIR)
	@mkdir -p $(SELF_SRC)/util
	@cp src/*.c src/*.h $(SELF_SRC)/
	@cp src/util/*.c src/util/*.h $(SELF_SRC)/util/
	@touch $@

# ficc writes object files next to the source; the loop runs from the staging
# dir so every object lands there and relative __FILE__ strings match.
define self-compile-loop
	@cd $(SELF_SRC) && for c in *.c util/*.c; do \
		SOURCE_DATE_EPOCH=0 $(1) -c "-DFICC_BUILTIN_INCLUDE=\"$(abspath include)\"" "$$c" \
		|| { echo "selftest: stage $(2) failed to compile $$c"; exit 1; }; \
	done
endef

selftest: $(FICC_BIN) $(SELF_DIR)/.staged
	@echo "== selftest stage 1: compile with $(notdir $(FICC_BIN)) =="
	$(call self-compile-loop,$(abspath $(FICC_BIN)),1)
	@$(CC) -no-pie $(LDFLAGS) $(SELF_SRC)/*.o $(SELF_SRC)/util/*.o -o $(FICC1_BIN)
	@echo "== selftest stage 2: rebuild with ficc1 =="
	$(call self-compile-loop,$(abspath $(FICC1_BIN)),2)
	@$(CC) -no-pie $(LDFLAGS) $(SELF_SRC)/*.o $(SELF_SRC)/util/*.o -o $(FICC2_BIN)
	@if cmp -s $(FICC1_BIN) $(FICC2_BIN); then \
		echo "selftest: OK — ficc1 and ficc2 are byte-identical"; \
	else \
		echo "selftest: FAIL — ficc1 and ficc2 differ (bootstrap mismatch)"; \
		cmp $(FICC1_BIN) $(FICC2_BIN); \
		exit 1; \
	fi
