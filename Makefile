CC      ?= cc
CFLAGS  ?= -O3 -march=native -flto -Wall -Wextra -std=c11 -g -D_XOPEN_SOURCE=700
LDFLAGS ?= -flto

L       := libs
BUILD   := build

INCLUDES := -Isrc -Icli -I$(L)/blockmodel/include -I$(L)/canvas/include -I$(L)/chunk/include \
	-I$(L)/mca/include -I$(L)/nbt/include -I$(L)/png/include -I$(L)/mcjson/include \
	-I$(L)/zip/include -I$(L)/percpu_rwlock/include

BLOCKMODEL_LIB    := $(L)/blockmodel/build/libblockmodel.a
CHUNK_LIB         := $(L)/chunk/build/libchunk.a
MCA_LIB           := $(L)/mca/build/libmca.a
NBT_LIB           := $(L)/nbt/build/libnbt.a
PNG_LIB           := $(L)/png/build/libpng_min.a
MCJSON_LIB        := $(L)/mcjson/build/libmcjson.a
ZIP_LIB           := $(L)/zip/build/libzipreader.a
CANVAS_LIB        := $(L)/canvas/build/libcanvas.a
PERCPU_RWLOCK_LIB := $(L)/percpu_rwlock/build/libpercpu_rwlock.a
DEPLIBS := $(BLOCKMODEL_LIB) $(CHUNK_LIB) $(MCA_LIB) $(NBT_LIB) $(PNG_LIB) $(MCJSON_LIB) \
	$(ZIP_LIB) $(CANVAS_LIB) $(PERCPU_RWLOCK_LIB)

# src/: the modules, one directory each (world, light, mesh, tiles, iso);
# cli/: the commands. Objects go to build/ under the same paths.
SRCS := $(wildcard src/*/*.c) $(filter-out cli/skinlist.c, $(wildcard cli/*.c))
OBJS := $(patsubst %.c,$(BUILD)/%.o,$(SRCS))
# with the libraries' public headers: a struct changed there changes ours
HDRS := $(wildcard src/*.h src/*/*.h cli/*.h $(L)/*/include/*.h)

.PHONY: all clean FORCE
all: $(BUILD)/prism $(BUILD)/skinlist

$(BUILD)/prism: $(OBJS) $(DEPLIBS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -Wl,--start-group $(DEPLIBS) -Wl,--end-group -lz -lm -lpthread -o $@

$(BUILD)/%.o: %.c $(HDRS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Lists the skins a world's player heads name, for scripts/fetch_skins.sh.
$(BUILD)/skinlist: cli/skinlist.c src/world/skin.h $(MCA_LIB) $(NBT_LIB) | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) cli/skinlist.c $(MCA_LIB) $(NBT_LIB) -lz -o $@

$(BUILD):
	mkdir -p $(BUILD)

# Each archive is built by its own Makefile; the sub-make is a no-op when
# nothing changed.
FORCE:
$(DEPLIBS): FORCE
	@$(MAKE) --no-print-directory -C $(dir $(@D)) $(notdir $(@D))/$(notdir $@)

clean:
	rm -rf $(BUILD)
