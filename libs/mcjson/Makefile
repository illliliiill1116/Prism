CC       ?= cc
CFLAGS   ?= -O3 -march=native -flto -Wall -Wextra -std=c11
LDFLAGS  ?= -flto

INCLUDES := -Iinclude

BUILD := build

.PHONY: all clean test

all: $(BUILD)/libmcjson.a

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/json.o: src/json.c include/json.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/libmcjson.a: $(BUILD)/json.o
	ar rcs $@ $^

test: $(BUILD)/test_json
	$(BUILD)/test_json

$(BUILD)/test_json: test/test_json.c $(BUILD)/libmcjson.a
	$(CC) $(CFLAGS) $(INCLUDES) $< $(BUILD)/libmcjson.a $(LDFLAGS) -o $@

clean:
	rm -rf $(BUILD)
