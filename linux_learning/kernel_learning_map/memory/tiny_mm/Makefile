CC ?= cc
CPPFLAGS ?= -Iinclude
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -Wpedantic -g -O0
SAN ?= -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS ?=
LDLIBS ?=

BUILD_DIR := build
TARGET := $(BUILD_DIR)/test_pmm
OBJS := $(BUILD_DIR)/pmm.o $(BUILD_DIR)/test_pmm.o

.PHONY: all test clean

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/pmm.o: src/pmm.c include/tmm.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SAN) -c $< -o $@

$(BUILD_DIR)/test_pmm.o: tests/test_pmm.c include/tmm.h tests/test_runner.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SAN) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(SAN) $(LDFLAGS) $^ -o $@ $(LDLIBS)

test: $(TARGET)
	./$(TARGET)

clean:
	rm -rf $(BUILD_DIR)
