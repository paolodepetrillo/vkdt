.PHONY:tools

ifneq ($(TARGET),aarch64-linux-android)
  tools:../bin/data/spectra.lut ../bin/vkdt-mkssf ../bin/vkdt-mkclut ../bin/vkdt-lutinfo ../bin/vkdt-eval-profile
else
  # cannot run mkabney or macadam when cross compiling
  tools:../bin/vkdt-mkssf ../bin/vkdt-mkclut ../bin/vkdt-lutinfo ../bin/vkdt-eval-profile
endif

ADD_CFLAGS=-Itools -Itools/shared
ADD_LDFLAGS=-lm -ldl

MKSSF_DEPS=tools/clut/src/dngproc.h\
        tools/shared/cie1931.h\
        tools/shared/matrices.h\
        tools/clut/src/cfa.h\
        tools/clut/src/cfa_data.h\
        tools/clut/src/cfa-plain.h\
        tools/clut/src/cfa-sigmoid.h\
        tools/clut/src/cfa-legendre.h\
        tools/clut/src/cfa-gauss.h

MKCLUT_DEPS=core/inpaint.h \
     core/solve.h \
     core/clip.h

../bin/vkdt-mkssf: tools/clut/src/mkssf.c ${MKSSF_DEPS} Makefile
	$(CC) $(CFLAGS) $(EXE_CFLAGS) $(OPT_CFLAGS) $(ADD_CFLAGS) $< -o $@ $(LDFLAGS) $(ADD_LDFLAGS)

../bin/vkdt-mkclut: tools/clut/src/mkclut.c ${MKCLUT_DEPS} Makefile
	$(CC) $(CFLAGS) $(EXE_CFLAGS) $(OPT_CFLAGS) $(ADD_CFLAGS) $< -o $@ $(LDFLAGS) $(ADD_LDFLAGS)

../bin/vkdt-lutinfo: tools/clut/src/lutinfo.c core/lut.h Makefile
	$(CC) $(CFLAGS) $(EXE_CFLAGS) $(OPT_CFLAGS) $(ADD_CFLAGS) $< -o $@ $(LDFLAGS) $(ADD_LDFLAGS)

../bin/vkdt-eval-profile: tools/clut/src/eval.c ${MKSSF_DEPS} Makefile
	$(CC) $(CFLAGS) $(EXE_CFLAGS) $(OPT_CFLAGS) $(ADD_CFLAGS) $< -o $@ $(LDFLAGS) $(ADD_LDFLAGS)

../bin/data/spectra.lut: mkabney macadam.lut Makefile
	@echo "[tools] precomputing rgb to spectrum upsampling table.."
	./mkabney
	mv spectra.lut abney.lut ../bin/data/

macadam.lut: macadam
	./macadam

macadam: tools/spec/macadam.c core/threads.c Makefile
	@echo "[tools] precomputing max theoretical reflectance brightness.."
	$(CC) $(CFLAGS) $(OPT_CFLAGS) $(EXE_CFLAGS) $(ADD_CFLAGS) $< core/threads.c -o $@ $(LDFLAGS) $(ADD_LDFLAGS) -pthread

mkabney: tools/spec/createlut.c core/threads.c Makefile
	$(CC) $(CFLAGS) $(OPT_CFLAGS) $(EXE_CFLAGS) $(ADD_CFLAGS) $< core/threads.c -o $@ $(LDFLAGS) $(ADD_LDFLAGS) -pthread
