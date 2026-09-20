CC      := gcc
CFLAGS  := -std=c99 -Wall -Wextra -O2 -g
TARGET  := gba

SRCS := src/main.c \
        src/cpu/cpu.c \
        src/memory/memory.c

OBJS := $(SRCS:.c=.o)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: clean