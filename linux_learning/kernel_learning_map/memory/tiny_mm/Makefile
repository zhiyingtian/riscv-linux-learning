CC ?= cc
CPPFLAGS ?= -Iinclude
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -Wpedantic -g -O0
SAN ?= -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS ?=
LDLIBS ?=

BUILD_DIR := build
DEBUG_BUILD_DIR := build-debug
M2C_TARGET := $(BUILD_DIR)/test_pmm
M2_TARGET := $(BUILD_DIR)/test_buddy
M2B_TARGET := $(BUILD_DIR)/test_pcp
DEBUG_M2C_TARGET := $(DEBUG_BUILD_DIR)/test_pmm
DEBUG_M2_TARGET := $(DEBUG_BUILD_DIR)/test_buddy
DEBUG_M2B_TARGET := $(DEBUG_BUILD_DIR)/test_pcp
M2C_OBJS := $(BUILD_DIR)/pmm.o $(BUILD_DIR)/buddy.o $(BUILD_DIR)/pcp.o $(BUILD_DIR)/test_pmm.o
M2_OBJS := $(BUILD_DIR)/buddy.o $(BUILD_DIR)/test_buddy.o
M2B_OBJS := $(BUILD_DIR)/buddy.o $(BUILD_DIR)/pcp.o $(BUILD_DIR)/test_pcp.o

.PHONY: all test test-m2c test-m2 test-m2b test-all debug-m2c debug-m2 debug-m2b clean

all: $(M2C_TARGET) $(M2_TARGET) $(M2B_TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/pmm.o: src/pmm.c include/tmm.h include/buddy.h include/pcp.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SAN) -c $< -o $@

$(BUILD_DIR)/test_pmm.o: tests/test_pmm.c include/tmm.h include/buddy.h include/pcp.h tests/test_runner.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SAN) -c $< -o $@

$(M2C_TARGET): $(M2C_OBJS)
	$(CC) $(CFLAGS) $(SAN) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD_DIR)/buddy.o: src/buddy.c include/buddy.h include/tmm.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SAN) -c $< -o $@

$(BUILD_DIR)/test_buddy.o: tests/test_buddy.c include/buddy.h include/tmm.h tests/test_runner_buddy.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SAN) -c $< -o $@

$(M2_TARGET): $(M2_OBJS)
	$(CC) $(CFLAGS) $(SAN) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD_DIR)/pcp.o: src/pcp.c include/pcp.h include/buddy.h include/tmm.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SAN) -c $< -o $@

$(BUILD_DIR)/test_pcp.o: tests/test_pcp.c include/pcp.h include/buddy.h include/tmm.h tests/test_runner_pcp.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SAN) -c $< -o $@

$(M2B_TARGET): $(M2B_OBJS)
	$(CC) $(CFLAGS) $(SAN) $(LDFLAGS) $^ -o $@ $(LDLIBS)

test: test-m2c

test-m2c: $(M2C_TARGET)
	./$(M2C_TARGET)

test-m2: $(M2_TARGET)
	./$(M2_TARGET)

test-m2b: $(M2B_TARGET)
	./$(M2B_TARGET)

test-all: test-m2c test-m2 test-m2b

debug-m2c:
	$(MAKE) BUILD_DIR=$(DEBUG_BUILD_DIR) clean
	$(MAKE) BUILD_DIR=$(DEBUG_BUILD_DIR) SAN= $(DEBUG_M2C_TARGET)

debug-m2:
	$(MAKE) BUILD_DIR=$(DEBUG_BUILD_DIR) clean
	$(MAKE) BUILD_DIR=$(DEBUG_BUILD_DIR) SAN= $(DEBUG_M2_TARGET)

debug-m2b:
	$(MAKE) BUILD_DIR=$(DEBUG_BUILD_DIR) clean
	$(MAKE) BUILD_DIR=$(DEBUG_BUILD_DIR) SAN= $(DEBUG_M2B_TARGET)

clean:
	rm -rf $(BUILD_DIR)
