LIBJPEG_TURBO_I=../ext/libjpeg-turbo
LIBJPEG_TURBO_L=../ext/libjpeg-turbo-build

$(LIBJPEG_TURBO_L)/Makefile: $(LIBJPEG_TURBO_I)/CMakeLists.txt
	mkdir -p $(LIBJPEG_TURBO_L)
	cd $(LIBJPEG_TURBO_L); \
	cmake ../libjpeg-turbo \
	-G"Unix Makefiles" \
	-DANDROID_ABI=arm64-v8a \
	-DANDROID_ARM_MODE=arm \
	-DANDROID_PLATFORM=android-$(ANDROID_VERSION) \
	-DCMAKE_ASM_FLAGS="--target=aarch64-linux-android${ANDROID_VERSION}" \
	-DCMAKE_TOOLCHAIN_FILE=${NDK}/build/cmake/android.toolchain.cmake \
	-DENABLE_SHARED=OFF

$(LIBJPEG_TURBO_L)/libjpeg.a: $(LIBJPEG_TURBO_L)/Makefile
	$(MAKE) -C $(LIBJPEG_TURBO_L)
