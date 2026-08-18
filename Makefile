CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -Isrc -Itest
DEPFLAGS := -MMD -MP
LDFLAGS :=


BUILD_DIR    := build
OBJ_DIR      := $(BUILD_DIR)/obj
TEST_OBJ_DIR := $(BUILD_DIR)/obj-test
BIN_DIR      := $(BUILD_DIR)/bin

SRC_ALL  := $(wildcard src/*.c) $(wildcard src/util/*.c)
SRC_TEST := test/runner.c $(wildcard test/unit/*.c)

OBJ_SRC       := $(patsubst src/%.c,$(OBJ_DIR)/src/%.o,$(SRC_ALL))
OBJ_TEST_SRC  := $(patsubst src/%.c,$(TEST_OBJ_DIR)/src/%.o,$(filter-out src/driver.c,$(SRC_ALL)))
OBJ_TEST_TEST := $(patsubst test/%.c,$(TEST_OBJ_DIR)/test/%.o,$(SRC_TEST))

FICC_BIN := $(BIN_DIR)/ficc
TEST_BIN := $(BIN_DIR)/test_runner

DIRS := $(sort $(dir $(OBJ_SRC) $(OBJ_TEST_SRC) $(OBJ_TEST_TEST) $(FICC_BIN) $(TEST_BIN)))

.PHONY: all clean test dirs

all: $(FICC_BIN)

test: $(TEST_BIN)
	$(TEST_BIN)

$(FICC_BIN): $(OBJ_SRC) | dirs
	$(CC) $(LDFLAGS) $^ -o $@

$(TEST_BIN): $(OBJ_TEST_SRC) $(OBJ_TEST_TEST) | dirs
	$(CC) $(LDFLAGS) $^ -o $@

$(OBJ_DIR)/src/%.o: src/%.c | dirs
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(TEST_OBJ_DIR)/src/%.o: src/%.c | dirs
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(TEST_OBJ_DIR)/test/%.o: test/%.c | dirs
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

-include $(OBJ_DIR)/src/*.d $(TEST_OBJ_DIR)/src/*.d $(TEST_OBJ_DIR)/test/*.d

dirs:
	@mkdir -p $(DIRS)

clean:
	rm -rf $(BUILD_DIR)
