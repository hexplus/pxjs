# -fno-strict-aliasing: heap cells are read through several struct types
# (PxCell, PxObject, PxArray...) that share a prefix; that is the engine's
# design, and the optimizer must not assume otherwise (-O2 miscompiled it).
#
# PXJS host build: the pxjs runner, 32-bit (the value encoding keeps a
# pointer in 32 bits, exactly as on the PSP), with ASan and UBSan. Doubles
# in SSE2 registers: the x87 unit computes in 80 bits and rounds twice,
# where the PSP's (software) doubles round once, as IEEE 754 says.
#
#   make                 build/host/pxjs
#   make test            every tests/js/*.js, normally and under GC stress
#   make SAN=            without sanitizers (for timing)

OUT  := build/host
CC   ?= gcc
SAN  ?= -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined
OPT  ?= -O1 -g
CFLAGS := -m32 -msse2 -mfpmath=sse -std=gnu11 $(OPT) -fno-strict-aliasing -Wall -Wextra -Wno-unused-parameter -Iinclude -Isrc $(SAN)

SRCS := src/px_heap.c src/px_string.c src/px_object.c src/px_lexer.c src/px_compiler.c src/px_vm.c \
        src/px_api.c src/px_builtins.c src/px_json.c src/px_iter.c src/px_promise.c \
        src/px_collections.c src/px_date.c src/px_regexp.c src/px_typed.c src/px_web.c \
        src/px_asyncgen.c src/px_proxy.c src/px_unicode.c src/px_dtoa.c
OBJS := $(addprefix $(OUT)/,$(SRCS:.c=.o))

$(OUT)/pxjs: $(OBJS) $(OUT)/tools/pxjs.o $(OUT)/tools/px_disasm.o
	$(CC) -m32 $(SAN) $^ -lm -o $@

$(OUT)/%.o: %.c $(wildcard src/*.h include/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# The Test262 host (tools/test262_host.c), built optimised and without
# sanitizers by scripts/test262.sh: make OUT=build/t262 SAN= OPT=-O2 build/t262/test262_host
$(OUT)/test262_host: $(OBJS) $(OUT)/tools/test262_host.o
	$(CC) -m32 $(SAN) $^ -lm -o $@

.PHONY: test clean
test: $(OUT)/pxjs
	@sh tests/run.sh $(OUT)/pxjs

clean:
	rm -rf $(OUT)
