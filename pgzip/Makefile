# args
# build with -m32?
M32      := 0
# build for out min size
MINS     := 0
STATIC_C := 0

LIB_PATH := ../lib
LIB_OBJ := \
    $(LIB_PATH)/crc32.o \
    $(LIB_PATH)/deflate_compress.o \
    $(LIB_PATH)/deflate_decompress.o \
    $(LIB_PATH)/gzip_compress.o \
    $(LIB_PATH)/gzip_decompress.o \
    $(LIB_PATH)/utils.o \
    $(LIB_PATH)/x86/cpu_features.o \
    $(LIB_PATH)/arm/cpu_features.o

APP_PATH := ../programs
APP_OBJ := \
    $(APP_PATH)/gzip_compress_by_stream_mt.o \
    $(APP_PATH)/gzip_decompress_by_stream_mt.o \
    $(APP_PATH)/prog_util.o \
    $(APP_PATH)/tgetopt.o

DEF_FLAGS := \
    -O3 -DNDEBUG -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64

# DEF_FLAGS += -I$(LDEF_PATH)

ifeq ($(M32),0)
else
  DEF_FLAGS += -m32
endif
ifeq ($(MINS),0)
else
  DEF_FLAGS += \
    -s \
    -Wno-error=format-security \
    -fvisibility=hidden  \
    -ffunction-sections -fdata-sections \
    -ffat-lto-objects -flto
  CXXFLAGS += -fvisibility-inlines-hidden
endif
CFLAGS   += $(DEF_FLAGS) 
CXXFLAGS += $(DEF_FLAGS) -std=c++11 -D_GLIBCXX_USE_NANOSLEEP

DEF_LINK := -lpthread -lstdc++
ifeq ($(M32),0)
else
  DEF_LINK += -m32
endif
ifeq ($(MINS),0)
else
  DEF_LINK += -s -Wl,--gc-sections,--as-needed
endif
ifeq ($(STATIC_C),0)
else
  DEF_LINK += -static
endif

.PHONY: all clean

all: libpgzip.a pgzip

libpgzip.a: $(LIB_OBJ) $(APP_OBJ)
	$(AR) rcs $@ $^

pgzip: libpgzip.a
	$(CC)	$(APP_PATH)/pgzip.c libpgzip.a $(CFLAGS) $(DEF_LINK) -o pgzip

RM := rm -f
clean:
	$(RM)	libpgzip.a pgzip $(LIB_OBJ) $(APP_OBJ)
