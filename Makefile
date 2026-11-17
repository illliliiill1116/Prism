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
OBJS := $(patsubst %.c,$(BUILD)/%.o,$(SRCS)) $(BUILD)/embed.o
# the app's page and the viewer, built into prism for prism serve
EMBED := $(wildcard app/*.html app/*.js app/*.css viewer/*.html viewer/*.css viewer/js/*.js)
# with the libraries' public headers: a struct changed there changes ours
HDRS := $(wildcard src/*.h src/*/*.h cli/*.h $(L)/*/include/*.h)

.PHONY: all clean win wasm web FORCE
all: $(BUILD)/prism $(BUILD)/skinlist

$(BUILD)/prism: $(OBJS) $(DEPLIBS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -Wl,--start-group $(DEPLIBS) -Wl,--end-group -lz -lm -lpthread -o $@

$(BUILD)/embed.c: $(EMBED) scripts/embed.sh
	@mkdir -p $(@D)
	scripts/embed.sh $@ $(EMBED)

$(BUILD)/embed.o: $(BUILD)/embed.c
	$(CC) $(CFLAGS) -c $< -o $@

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

# Windows: prism.exe, cross-built with MinGW-w64 (gcc-mingw-w64-x86-64 and
# libz-mingw-w64-dev), everything linked in so the one file runs on its own;
# for any x86-64 CPU of the last decade, not just this one.
WIN_CC      ?= x86_64-w64-mingw32-gcc
WIN_WINDRES ?= x86_64-w64-mingw32-windres
WIN_BUILD   := build-win
WIN_CFLAGS  := -O3 -march=x86-64-v2 -mtune=generic -flto -Wall -Wextra -std=c11 -D_WIN32_WINNT=0x0A00 \
	-D__USE_MINGW_ANSI_STDIO=1 -D_USE_MATH_DEFINES -Wno-format
WIN_SRCS    := $(SRCS) $(wildcard $(L)/*/src/*.c)
WIN_OBJS    := $(patsubst %.c,$(WIN_BUILD)/%.o,$(WIN_SRCS)) $(WIN_BUILD)/embed.o $(WIN_BUILD)/prism.res.o
WIN_HDRS    := $(HDRS) $(wildcard $(L)/*/src/*.h)

win: $(WIN_BUILD)/prism.exe

$(WIN_BUILD)/prism.exe: $(WIN_OBJS)
	$(WIN_CC) $(WIN_CFLAGS) -static $(WIN_OBJS) -lz -lws2_32 -lshell32 -lpthread -o $@

$(WIN_BUILD)/%.o: %.c $(WIN_HDRS)
	@mkdir -p $(@D)
	$(WIN_CC) $(WIN_CFLAGS) $(INCLUDES) -c $< -o $@

$(WIN_BUILD)/embed.o: $(BUILD)/embed.c
	@mkdir -p $(@D)
	$(WIN_CC) $(WIN_CFLAGS) -c $< -o $@

# the manifest: UTF-8 paths (see src/plat.h), and an icon-less version info
$(WIN_BUILD)/prism.res.o: cli/win/prism.rc cli/win/prism.manifest
	@mkdir -p $(@D)
	$(WIN_WINDRES) -I cli/win $< -o $@

# The browser: the same sources built with Emscripten (emsdk on the PATH)
# into build-wasm/prism.js + prism.wasm. Its own threads are Web Workers;
# under Node (with NODERAWFS, so it reads and writes the disk) it runs as
# the command does: node build-wasm/prism.js bake ... scripts/wasm_check.sh
# checks it bakes what the native build does.
EMCC        ?= emcc
WASM_BUILD  := build-wasm
WASM_CFLAGS := -O3 -msimd128 -pthread -std=gnu11 -Wall -Wno-format -Wno-pointer-type-mismatch
WASM_FLAGS  := -sUSE_ZLIB=1 -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB -sINITIAL_MEMORY=256MB \
	-sSTACK_SIZE=8MB -sDEFAULT_PTHREAD_STACK_SIZE=8MB -sPTHREAD_POOL_SIZE=16 -sEXIT_RUNTIME=1
WASM_SRCS   := $(filter-out cli/serve.c, $(SRCS)) $(wildcard $(L)/*/src/*.c)
WASM_OBJS   := $(patsubst %.c,$(WASM_BUILD)/%.o,$(WASM_SRCS))

wasm: $(WASM_BUILD)/prism.js

# when it was linked, for prism map to tell bakes older than it (plat.c)
WASM_BUILT = @echo 'const long long prism_built = '$$(date +%s)';' > $(WASM_BUILD)/built.c

$(WASM_BUILD)/prism.js: $(WASM_OBJS)
	$(WASM_BUILT)
	$(EMCC) $(WASM_CFLAGS) $(WASM_FLAGS) $(WASM_OBJS) $(WASM_BUILD)/built.c -o $@

# The browser app as a static site in build-web/: the app's page, the
# viewer, prism built for the page (an ES module run in workers, see
# web/prism-worker.js) and the service worker (web/sw.js). Any static host
# does; GitHub Pages too.
WEB_BUILD := build-web
WEB_FLAGS := -sUSE_ZLIB=1 -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=web,worker -sINVOKE_RUN=0 -sEXIT_RUNTIME=0 \
	-sFORCE_FILESYSTEM=1 -lworkerfs.js -sEXPORTED_RUNTIME_METHODS=FS,WORKERFS,callMain \
	-sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB -sINITIAL_MEMORY=256MB -sSTACK_SIZE=8MB \
	-sDEFAULT_PTHREAD_STACK_SIZE=8MB '-sPTHREAD_POOL_SIZE=Module["pool"]||0' -sPTHREAD_POOL_SIZE_STRICT=2

web: $(WEB_BUILD)/prism.js $(EMBED) web/sw.js web/prism-worker.js
	mkdir -p $(WEB_BUILD)/app $(WEB_BUILD)/viewer/js
	cp app/index.html $(WEB_BUILD)/index.html
	cp app/*.js app/*.css $(WEB_BUILD)/app/
	cp viewer/*.html viewer/*.css $(WEB_BUILD)/viewer/
	cp viewer/js/*.js $(WEB_BUILD)/viewer/js/
	# the viewer's scripts, for the page's Export (app/export.js)
	cd viewer && ls js/*.js | sed 's/.*/"&"/' | paste -sd, | sed 's/.*/[&]/' > ../$(WEB_BUILD)/viewer/files.json
	cp web/sw.js web/prism-worker.js $(WEB_BUILD)/

$(WEB_BUILD)/prism.js: $(WASM_OBJS)
	@mkdir -p $(@D)
	$(WASM_BUILT)
	$(EMCC) $(WASM_CFLAGS) $(WEB_FLAGS) $(WASM_OBJS) $(WASM_BUILD)/built.c -o $@

$(WASM_BUILD)/%.o: %.c $(WIN_HDRS)
	@mkdir -p $(@D)
	$(EMCC) $(WASM_CFLAGS) -sUSE_ZLIB=1 $(INCLUDES) -c $< -o $@

clean:
	rm -rf $(BUILD) $(WIN_BUILD) $(WASM_BUILD) $(WEB_BUILD)
