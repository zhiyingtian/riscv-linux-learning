CC ?= gcc
CFLAGS ?= -O0 -g -Wall -Wextra
TARGET := trigger_alloc
SRC := trigger_alloc.c

.PHONY: all test clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^

# 最小自检:能编译 + 能跑通 MARK 时间线
test: $(TARGET)
	@./$(TARGET) 4 | grep -q "MARK done" && echo "trigger_alloc: OK"
	@bash -n trace_events.sh && bash -n trace_graph.sh && echo "scripts: OK"

clean:
	rm -f $(TARGET)
