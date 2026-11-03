CC       ?= cc
CFLAGS   ?= -O3 -march=native -flto -Wall -Wextra -std=c11
LDFLAGS  ?= -flto
INCLUDES := -Iinclude
LIBS     := -lz

BUILD := build

LIB_SRCS := src/nbt.c src/nbt_io.c
LIB_OBJS := $(LIB_SRCS:src/%.c=$(BUILD)/%.o)
LIB      := $(BUILD)/libnbt.a

.PHONY: all clean test

all: $(LIB)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c include/nbt.h include/nbt_io.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(LIB): $(LIB_OBJS)
	ar rcs $@ $^

$(BUILD)/synth.o: test/synth.c test/synth.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

test: $(BUILD)/test_nbt
	$(BUILD)/test_nbt

$(BUILD)/test_nbt: test/test_nbt.c $(BUILD)/synth.o $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -Itest $< $(BUILD)/synth.o $(LIB) $(LDFLAGS) $(LIBS) -o $@

clean:
	rm -rf $(BUILD)
