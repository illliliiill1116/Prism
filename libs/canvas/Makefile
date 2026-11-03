CC       ?= cc
CFLAGS   ?= -O3 -march=native -flto -Wall -Wextra -std=c11
LDFLAGS  ?= -flto

INCLUDES := -Iinclude

BUILD := build

.PHONY: all clean test

all: $(BUILD)/libcanvas.a

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/canvas.o: src/canvas.c include/canvas.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/libcanvas.a: $(BUILD)/canvas.o
	ar rcs $@ $^

test: $(BUILD)/test_canvas
	$(BUILD)/test_canvas

$(BUILD)/test_canvas: test/test_canvas.c $(BUILD)/libcanvas.a
	$(CC) $(CFLAGS) $(INCLUDES) $< $(BUILD)/libcanvas.a $(LDFLAGS) -o $@

clean:
	rm -rf $(BUILD)
