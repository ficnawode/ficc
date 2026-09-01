CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -Isrc -Itest
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

.PHONY: all clean test dirs compile-commands

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
