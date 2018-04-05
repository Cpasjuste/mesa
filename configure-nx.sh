#!/bin/bash

cp configure configure.nx

# clock_gettime...
sed -i 's/if test "x$ac_cv_func_clock_gettime" = xyes; then :/if test "x$ac_cv_func_clock_gettime" = xno; then :/g' configure.nx

sed -i 's/as_fn_error $? "Building mesa on this platform requires pthreads" "$LINENO" 5/echo "ok"/g' configure.nx
#sed -i 's/-Werror=implicit-function-declaration//g' configure.nx
sed -i 's/-Werror=missing-prototypes//g' configure.nx
sed -i 's/-pthreads//g' configure.nx
sed -i 's/-pthread//g' configure.nx
sed -i 's/noinst_PROGRAMS += spirv2nir//g' src/compiler/Makefile.nir.am
sed -i 's/-DHAVE_LIBDRM/-DNOTHAVE_LIBDRM/g' configure.nx
sed -i 's/-DENABLE_SHADER_CACHE/-DDISABLE_SHADER_CACHE/g' configure.nx
sed -i 's/std=c99/std=gnu99/g' configure.nx

PORTLIBS_PATH="${DEVKITPRO}/portlibs/switch" \
PATH="${DEVKITA64}/bin:${PORTLIBS_PATH}/bin:${PATH}" \
ACLOCAL_FLAGS="-I${PORTLIBS_PATH}/share/aclocal" \
CFLAGS="-DPIPE_OS_UNIX -std=gnu99 -O2 -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIC -ftls-model=local-exec -ffast-math -ffunction-sections -fdata-sections -D__NX__ -DINT_MAX=2147483647 -DLLONG_MAX=9223372036854775807LL -DM_PI_2=1.57079632679489661923 -DM_PI_4=0.78539816339744830962" \
CPPFLAGS="-Wno-error=missing-prototypes -O2 -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIC -ftls-model=local-exec -isystem ${DEVKITPRO}/libnx/include -I${PORTLIBS_PATH}/include -D__NX__ -DLLONG_MAX=9223372036854775807LL" \
LDFLAGS="-L${DEVKITPRO}/libnx/lib -L${PORTLIBS_PATH}/lib" \
LIBS="-lnx" \
PTHREADSTUBS_CFLAGS="-I${DEVKITPRO}/libnx/include" \
PTHREADSTUBS_LIBS="-lnx" \
PTHREAD_LIBS="-lnx" \
./configure.nx --prefix=${PORTLIBS_PATH} --host=aarch64-none-elf \
	--disable-shared --enable-static \
	--disable-llvm-shared-libs \
	--disable-gles1 \
	--disable-gles2 \
	--disable-dri \
	--disable-dri3 \
	--disable-glx \
	--enable-osmesa  \
	--disable-gallium-osmesa \
	--disable-egl \
	--disable-shared-glapi \
	--disable-llvm \
	--with-gallium-drivers="" \
	--with-dri-drivers="" \
	--with-platforms="" \
	--disable-gbm
