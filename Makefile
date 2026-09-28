CC     := gcc
CFLAGS := -std=c99 -Wall -Wextra -O2 -g -MMD -MP
LDLIBS := -lm
TARGET := gba
TEST   := tests

CORE_SRCS := src/emulator/emulator.c \
             src/cpu/cpu.c \
             src/cpu/thumb.c \
             src/cpu/bios.c \
             src/memory/memory.c \
             src/hw/hw.c \
             src/ppu/ppu.c

SRCS := src/runner.c $(CORE_SRCS)

OBJS := $(SRCS:.c=.o)

TEST_SRCS := src/main.c src/testrom.c $(CORE_SRCS)

TEST_OBJS := $(TEST_SRCS:.c=.o)

DEPS := $(OBJS:.o=.d) $(TEST_OBJS:.o=.d)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(TEST): $(TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $(TEST_OBJS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

test: $(TEST)
	./$(TEST)

clean:
	rm -f $(OBJS) $(TEST_OBJS) $(DEPS) $(TARGET) $(TEST)

-include $(DEPS)

.PHONY: all test clean
