ifeq ($(TARGET),aarch64-linux-android)
  MOD_CFLAGS=-I$(LIBJPEG_TURBO_I) -I$(LIBJPEG_TURBO_L)
  MOD_LDFLAGS=-L$(LIBJPEG_TURBO_L) -ljpeg
  EXTRA_DEPS=$(LIBJPEG_TURBO_L)/libjpeg.a
else
  MOD_CFLAGS=$(shell pkg-config --cflags libjpeg)
  MOD_LDFLAGS=$(shell pkg-config --libs libjpeg)
  EXTRA_DEPS=
endif
pipe/modules/i-jpg/libi-jpg.so:pipe/modules/i-jpg/jpegexiforient.h $(EXTRA_DEPS)
