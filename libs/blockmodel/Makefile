CC       ?= cc
CFLAGS   ?= -O3 -march=native -flto -Wall -Wextra -std=c11 -D_XOPEN_SOURCE=700
LDFLAGS  ?= -flto
LIBS     := -lz -lm -lpthread

PNG_DIR     ?= ../png
MCJSON_DIR  ?= ../mcjson
ZIP_DIR     ?= ../zip
CANVAS_DIR  ?= ../canvas
PERCPU_RWLOCK_DIR ?= ../percpu_rwlock

PNG_INC     := $(PNG_DIR)/include
MCJSON_INC  := $(MCJSON_DIR)/include
ZIP_INC     := $(ZIP_DIR)/include
CANVAS_INC  := $(CANVAS_DIR)/include
PERCPU_RWLOCK_INC := $(PERCPU_RWLOCK_DIR)/include

PNG_LIB     := $(PNG_DIR)/build/libpng_min.a
MCJSON_LIB  := $(MCJSON_DIR)/build/libmcjson.a
ZIP_LIB     := $(ZIP_DIR)/build/libzipreader.a
CANVAS_LIB  := $(CANVAS_DIR)/build/libcanvas.a
PERCPU_RWLOCK_LIB := $(PERCPU_RWLOCK_DIR)/build/libpercpu_rwlock.a

INCLUDES := -Iinclude -I$(PNG_INC) -I$(MCJSON_INC) -I$(ZIP_INC) -I$(CANVAS_INC) -I$(PERCPU_RWLOCK_INC)
DEPLIBS  := $(PNG_LIB) $(MCJSON_LIB) $(ZIP_LIB) $(CANVAS_LIB) $(PERCPU_RWLOCK_LIB)

BUILD := build

.PHONY: all clean test

all: $(BUILD)/libblockmodel.a

$(BUILD):
	mkdir -p $(BUILD)

$(PNG_LIB):
	$(MAKE) -C $(PNG_DIR)
$(MCJSON_LIB):
	$(MAKE) -C $(MCJSON_DIR)
$(ZIP_LIB):
	$(MAKE) -C $(ZIP_DIR)
$(CANVAS_LIB):
	$(MAKE) -C $(CANVAS_DIR)
$(PERCPU_RWLOCK_LIB):
	$(MAKE) -C $(PERCPU_RWLOCK_DIR)

OBJS := $(BUILD)/registry.o $(BUILD)/modelchain.o $(BUILD)/shape.o $(BUILD)/faces.o $(BUILD)/statetint.o $(BUILD)/entity_shapes.o

$(BUILD)/%.o: src/%.c include/blockmodel.h src/internal.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/libblockmodel.a: $(OBJS)
	ar rcs $@ $^

test: $(BUILD)/test_blockmodel
	$(BUILD)/test_blockmodel "$(JAR)"

$(BUILD)/test_blockmodel: test/test_blockmodel.c $(BUILD)/libblockmodel.a $(DEPLIBS)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(BUILD)/libblockmodel.a $(DEPLIBS) $(LDFLAGS) $(LIBS) -o $@

clean:
	rm -rf $(BUILD)
