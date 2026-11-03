CC       ?= cc
CFLAGS   ?= -O3 -march=native -flto -Wall -Wextra -std=c11
LDFLAGS  ?= -flto
LIBS     := -lz

NBT_DIR  ?= ../nbt
NBT_INC  := $(NBT_DIR)/include
NBT_LIB  := $(NBT_DIR)/build/libnbt.a
NBT_TEST_DIR := $(NBT_DIR)/test

INCLUDES := -Iinclude -Itest -I$(NBT_INC) -I$(NBT_TEST_DIR)

BUILD := build

.PHONY: all clean test

all: $(BUILD)/libmca.a

$(BUILD):
	mkdir -p $(BUILD)

# the nbt static lib is built by its own project Makefile; build it if
# it's missing (or stale) rather than duplicating its rules here.
$(NBT_LIB):
	$(MAKE) -C $(NBT_DIR)

$(BUILD)/mca.o: src/mca.c include/mca.h $(NBT_INC)/nbt.h $(NBT_INC)/nbt_io.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/libmca.a: $(BUILD)/mca.o
	ar rcs $@ $^

$(BUILD)/synth.o: $(NBT_TEST_DIR)/synth.c $(NBT_TEST_DIR)/synth.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/synth_region.o: test/synth_region.c test/synth_region.h $(NBT_TEST_DIR)/synth.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

test: $(BUILD)/test_mca
	$(BUILD)/test_mca

$(BUILD)/test_mca: test/test_mca.c $(BUILD)/synth_region.o $(BUILD)/synth.o $(BUILD)/libmca.a $(NBT_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(BUILD)/synth_region.o $(BUILD)/synth.o $(BUILD)/libmca.a $(NBT_LIB) $(LDFLAGS) $(LIBS) -o $@

clean:
	rm -rf $(BUILD)
