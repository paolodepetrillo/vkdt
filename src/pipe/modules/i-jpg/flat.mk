ifeq ($(VKDT_JPEG_STATIC), 1)
  MOD_CFLAGS=-I$(LIBJPEG_TURBO_I) -I$(LIBJPEG_TURBO_L)
  MOD_LDFLAGS=-L$(LIBJPEG_TURBO_L) -ljpeg
  MOD_DEPS=$(LIBJPEG_TURBO_L)/libjpeg.a
else
  MOD_CFLAGS=$(VKDT_JPEG_CFLAGS)
  MOD_LDFLAGS=$(VKDT_JPEG_LDFLAGS)
endif
pipe/modules/i-jpg/libi-jpg.so:pipe/modules/i-jpg/jpegexiforient.h
