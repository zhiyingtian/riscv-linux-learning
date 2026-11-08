CC ?= cc
CPPFLAGS ?= -Iinclude
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -Wpedantic -g -O0
SAN ?= -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS ?=
LDLIBS ?=

BUILD_DIR := build
M1_TARGET := $(BUILD_DIR)/test_pmm
M2_TARGET := $(BUILD_DIR)/test_buddy
M1_OBJS := $(BUILD_DIR)/pmm.o $(BUILD_DIR)/test_pmm.o
M2_OBJS := $(BUILD_DIR)/buddy.o $(BUILD_DIR)/test_buddy.o

.PHONY: all test test-m1 test-m2 test-all clean

all: $(M1_TARGET) $(M2_TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/pmm.o: src/pmm.c include/tmm.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SAN) -c $< -o $@

$(BUILD_DIR)/test_pmm.o: tests/test_pmm.c include/tmm.h tests/test_runner.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SAN) -c $< -o $@

$(M1_TARGET): $(M1_OBJS)
	$(CC) $(CFLAGS) $(SAN) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD_DIR)/buddy.o: src/buddy.c include/buddy.h include/tmm.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SAN) -c $< -o $@

$(BUILD_DIR)/test_buddy.o: tests/test_buddy.c include/buddy.h include/tmm.h tests/test_runner_buddy.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SAN) -c $< -o $@

$(M2_TARGET): $(M2_OBJS)
	$(CC) $(CFLAGS) $(SAN) $(LDFLAGS) $^ -o $@ $(LDLIBS)

test: test-m1

test-m1: $(M1_TARGET)
	./$(M1_TARGET)

test-m2: $(M2_TARGET)
	./$(M2_TARGET)

test-all: test-m1 test-m2

clean:
	rm -rf $(BUILD_DIR)
