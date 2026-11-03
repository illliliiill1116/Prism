CC       ?= cc
CFLAGS   ?= -O3 -march=native -flto -Wall -Wextra -std=c11
LDFLAGS  ?= -flto
LIBS     := -lz -lpthread

CANVAS_DIR ?= ../canvas
CANVAS_LIB := $(CANVAS_DIR)/build/libcanvas.a

INCLUDES := -Iinclude -I$(CANVAS_DIR)/include
FIXTURES_DIR := $(abspath test/fixtures)

BUILD := build

.PHONY: all clean test

all: $(BUILD)/libpng_min.a

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/png_decode.o: src/png_decode.c include/png_image.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/png_encode.o: src/png_encode.c include/png_image.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/libpng_min.a: $(BUILD)/png_decode.o $(BUILD)/png_encode.o
	ar rcs $@ $^

$(CANVAS_LIB):
	$(MAKE) -C $(CANVAS_DIR)

test: $(BUILD)/test_png
	$(BUILD)/test_png

$(BUILD)/test_png: test/test_png.c $(BUILD)/libpng_min.a $(CANVAS_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -DFIXTURES_DIR=\"$(FIXTURES_DIR)\" $< $(BUILD)/libpng_min.a $(CANVAS_LIB) $(LDFLAGS) $(LIBS) -o $@

clean:
	rm -rf $(BUILD)
