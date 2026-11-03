CC       ?= cc
CFLAGS   ?= -O3 -march=native -flto -Wall -Wextra -std=c11
LDFLAGS  ?= -flto
LIBS     := -lz

NBT_DIR  ?= ../nbt
NBT_INC  := $(NBT_DIR)/include
NBT_LIB  := $(NBT_DIR)/build/libnbt.a

INCLUDES := -Iinclude -Itest -I$(NBT_INC)

BUILD := build

.PHONY: all clean test

all: $(BUILD)/libchunk.a

$(BUILD):
	mkdir -p $(BUILD)

$(NBT_LIB):
	$(MAKE) -C $(NBT_DIR)

$(BUILD)/chunk.o: src/chunk.c include/chunk.h $(NBT_INC)/nbt.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/libchunk.a: $(BUILD)/chunk.o
	ar rcs $@ $^

$(BUILD)/synth_section.o: test/synth_section.c test/synth_section.h include/chunk.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

test: $(BUILD)/test_chunk
	$(BUILD)/test_chunk

$(BUILD)/test_chunk: test/test_chunk.c $(BUILD)/synth_section.o $(BUILD)/libchunk.a $(NBT_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(BUILD)/synth_section.o $(BUILD)/libchunk.a $(NBT_LIB) $(LDFLAGS) $(LIBS) -o $@

clean:
	rm -rf $(BUILD)
