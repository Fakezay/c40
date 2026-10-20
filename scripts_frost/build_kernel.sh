#!/bin/bash
set -e

KERNEL_DIR="/home/ubuntu/work_space/kernel_xiaomi_frost_kernelsu"
cd "${KERNEL_DIR}"

export PATH="/home/ubuntu/toolchains/clang-12-arm64-native/bin:/usr/bin:$PATH"
export ARCH=arm64
export SUBARCH=arm64
export CLANG_TRIPLE=aarch64-linux-gnu-
export CROSS_COMPILE=/usr/bin/aarch64-linux-gnu-
export CROSS_COMPILE_COMPAT=/home/ubuntu/toolchains/gcc32/bin/arm-linux-androideabi-

JOBS=$(nproc)
echo "=========================================================="
echo "Compilando POCO C40 (frost) Kernel com KernelSU (${JOBS} cores)..."
echo "=========================================================="

make O=out \
    ARCH=arm64 \
    SUBARCH=arm64 \
    CC=clang \
    LD=ld.lld \
    AR=llvm-ar \
    NM=llvm-nm \
    OBJCOPY=llvm-objcopy \
    OBJDUMP=llvm-objdump \
    READELF=llvm-readelf \
    STRIP=llvm-strip \
    CLANG_TRIPLE="${CLANG_TRIPLE}" \
    CROSS_COMPILE="${CROSS_COMPILE}" \
    CROSS_COMPILE_COMPAT="${CROSS_COMPILE_COMPAT}" \
    KCFLAGS="-w -D__ANDROID_COMMON_KERNEL__" \
    CONFIG_SECTION_MISMATCH_WARN_ONLY=y \
    -j"${JOBS}" \
    Image.gz Image dtbs

echo "=========================================================="
echo "Compilação KernelSU concluída com sucesso!"
ls -lh out/arch/arm64/boot/Image*
ls -lh out/arch/arm64/boot/dts/jlq/jlq_jr510.dtb 2>/dev/null || true
echo "=========================================================="
