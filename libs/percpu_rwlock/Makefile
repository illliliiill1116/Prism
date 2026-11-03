CC       ?= cc
CFLAGS   ?= -O3 -march=native -flto -Wall -Wextra -std=c11 -D_XOPEN_SOURCE=700
LDFLAGS  ?= -flto

INCLUDES := -Iinclude

BUILD := build

.PHONY: all clean test

all: $(BUILD)/libpercpu_rwlock.a

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/percpu_rwlock.o: src/percpu_rwlock.c include/percpu_rwlock.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/libpercpu_rwlock.a: $(BUILD)/percpu_rwlock.o
	ar rcs $@ $^

test: $(BUILD)/test_percpu_rwlock
	$(BUILD)/test_percpu_rwlock

$(BUILD)/test_percpu_rwlock: test/test_percpu_rwlock.c $(BUILD)/libpercpu_rwlock.a
	$(CC) $(CFLAGS) $(INCLUDES) $< $(BUILD)/libpercpu_rwlock.a $(LDFLAGS) -lpthread -o $@

clean:
	rm -rf $(BUILD)
