CC       ?= cc
CFLAGS   ?= -O3 -march=native -flto -Wall -Wextra -std=c11
LDFLAGS  ?= -flto
LIBS     := -lz

INCLUDES := -Iinclude
FIXTURES_DIR := $(abspath test/fixtures)

BUILD := build

.PHONY: all clean test

all: $(BUILD)/libzipreader.a

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/zip_reader.o: src/zip_reader.c include/zip_reader.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/libzipreader.a: $(BUILD)/zip_reader.o
	ar rcs $@ $^

test: $(BUILD)/test_zip
	$(BUILD)/test_zip

$(BUILD)/test_zip: test/test_zip.c $(BUILD)/libzipreader.a
	$(CC) $(CFLAGS) $(INCLUDES) -DFIXTURES_DIR=\"$(FIXTURES_DIR)\" $< $(BUILD)/libzipreader.a $(LDFLAGS) $(LIBS) -o $@

clean:
	rm -rf $(BUILD)
