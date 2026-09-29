# The PSP engine benchmark: PXJS and QuickJS in one EBOOT (see main.c).
# Built by scripts/bench-psp.sh in the pinned PSPDEV container.

OUT    := build
OBJDIR := $(OUT)/obj
$(shell mkdir -p $(OBJDIR))

TARGET          = $(OUT)/pxbench
PSP_EBOOT       = $(OUT)/EBOOT.PBP
PSP_EBOOT_SFO   = $(OUT)/PARAM.SFO
PSP_EBOOT_TITLE = PXJS bench
EXTRA_TARGETS   = $(PSP_EBOOT)

PXJS := ../..
QJS  := ../third_party/quickjs

PXJS_SRCS := $(wildcard $(PXJS)/src/*.c)
QJS_SRCS  := $(QJS)/quickjs.c $(QJS)/libregexp.c $(QJS)/libunicode.c $(QJS)/cutils.c $(QJS)/dtoa.c

OBJS := $(OBJDIR)/main.o \
        $(patsubst $(PXJS)/src/%.c,$(OBJDIR)/pxjs/%.o,$(PXJS_SRCS)) \
        $(patsubst $(QJS)/%.c,$(OBJDIR)/qjs/%.o,$(QJS_SRCS))

# The same flags the runtime uses for each engine.
CFLAGS = -O2 -G0 -std=gnu11 -fno-strict-aliasing -Wall -Wno-unused-parameter -I$(PXJS)/include -I$(QJS) \
         -D_GNU_SOURCE -DCONFIG_VERSION=\"$(shell cat $(QJS)/VERSION)\"

BUILD_PRX        = 1
PSP_FW_VERSION   = 660
PSP_LARGE_MEMORY = 0
LIBS =

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

# After the include: see pspx-runtime/Makefile.
LIBS := $(LIBS) -lpsppower -lm

$(OBJDIR)/pxjs/%.o: $(PXJS)/src/%.c $(wildcard $(PXJS)/src/*.h $(PXJS)/include/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(PXJS)/src -c $< -o $@

# QuickJS: its own warning profile; and GCC 14+ rejects its int32_t/int
# pointer mixing (newlib on the PSP defines int32_t as long).
$(OBJDIR)/qjs/%.o: $(QJS)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fwrapv -w -Wno-incompatible-pointer-types -c $< -o $@

$(OBJDIR)/%.o: %.c $(wildcard $(PXJS)/include/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
