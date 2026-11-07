CC ?= gcc
CFLAGS ?= -O0 -g -Wall -Wextra
TARGET := mmap_file_lazy_demo
SRC := mmap_file_lazy.c

.PHONY: all run clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) file_data.txt
