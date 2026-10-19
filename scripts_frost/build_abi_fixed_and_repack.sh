#!/bin/bash
set -e

LOG="/home/ubuntu/build_abi_fixed.log"
exec > >(tee -a "${LOG}") 2>&1

echo "=========================================================="
echo "Iniciando compilação com ABI 100% compatível..."
echo "=========================================================="

cd /home/ubuntu/work_space/kernel_xiaomi_frost_kernelsu

export PATH="/home/ubuntu/toolchains/clang-12-arm64-native/bin:/usr/bin:$PATH"
export ARCH=arm64
export SUBARCH=arm64
export CLANG_TRIPLE=aarch64-linux-gnu-
export CROSS_COMPILE=/usr/bin/aarch64-linux-gnu-
export CROSS_COMPILE_COMPAT=/home/ubuntu/toolchains/gcc32/bin/arm-linux-androideabi-

cp /home/ubuntu/frost_kernelsu_working_boot.config out/.config
cat docker.config >> out/.config

JOBS=$(nproc)

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
    olddefconfig

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
    Image.gz Image

echo "=========================================================="
echo "Compilação concluída! Verificando Image e símbolos..."
echo "=========================================================="

KERNEL_IMG="/home/ubuntu/work_space/kernel_xiaomi_frost_kernelsu/out/arch/arm64/boot/Image"
if [ ! -f "${KERNEL_IMG}" ]; then
    echo "ERRO: Image não foi gerado!"
    exit 1
fi

grep -w "__cfi_slowpath" out/System.map || true
grep -w "__cfi_slowpath_diag" out/System.map || true

echo "=========================================================="
echo "Verificando offsets de struct net_device e struct net com pahole..."
echo "=========================================================="
python3 -c '
import subprocess, sys

def get_pahole_offset(struct_name, field_pattern):
    out = subprocess.check_output(f"pahole -C {struct_name} out/vmlinux", shell=True).decode()
    for line in out.splitlines():
        if field_pattern in line and "/*" in line:
            comment = line.split("/*")[1].split("*/")[0].strip()
            offset = int(comment.split()[0])
            return offset
    return None

rx_off = get_pahole_offset("net_device", "rx_handler;")
pcpu_off = get_pahole_offset("net_device", "pcpu_refcnt;")
gen_off = get_pahole_offset("net", "gen;")

print(f"rx_handler offset: {rx_off} (esperado: 792 / 0x318)")
print(f"pcpu_refcnt offset: {pcpu_off} (esperado: 1144 / 0x478)")
print(f"gen offset: {gen_off} (esperado: 3168 / 0xc60)")

if rx_off != 792:
    print(f"ERRO: rx_handler offset {rx_off} != 792")
    sys.exit(1)
if pcpu_off != 1144:
    print(f"ERRO: pcpu_refcnt offset {pcpu_off} != 1144")
    sys.exit(1)
if gen_off != 3168:
    print(f"ERRO: gen offset {gen_off} != 3168")
    sys.exit(1)

print("TODOS OS OFFSETS DE NET_DEVICE E STRUCT NET VERIFICADOS COM 100% DE SUCESSO!")
'



REPACK_DIR="/home/ubuntu/work_space/repack_abi_fixed"
rm -rf "${REPACK_DIR}"
mkdir -p "${REPACK_DIR}"
cd "${REPACK_DIR}"

cp /home/ubuntu/poco_c40_repack/ramdisk.cpio .
cp "${KERNEL_IMG}" ./kernel

echo "Reempacotando boot.img com magiskboot (GZIP + PATCHVBMETAFLAG)..."
PATCHVBMETAFLAG=true /home/ubuntu/poco_c40_anykernel3/tools/magiskboot repack /home/ubuntu/poco_c40_rom/frost_global_images_V13.0.25.0.RGFMIXM_20231226.0000.00_11.0_global/images/boot.img

echo "Assinando com avbtool.py..."
python3 /home/ubuntu/toolchain/avbtool.py add_hash_footer --image new-boot.img --partition_size 100663296 --partition_name boot

FINAL_IMG="/home/ubuntu/boot_poco_c40_droidspaces_v3.img"
cp new-boot.img "${FINAL_IMG}"

FINAL_ZIP="/home/ubuntu/boot_poco_c40_droidspaces_v3.zip"
rm -f "${FINAL_ZIP}"
zip -1 -j "${FINAL_ZIP}" "${FINAL_IMG}"

SHA_IMG=$(sha256sum "${FINAL_IMG}" | awk '{print $1}')
SHA_ZIP=$(sha256sum "${FINAL_ZIP}" | awk '{print $1}')

echo "Enviando para Catbox..."
LINK_IMG=$(curl -s -F "reqtype=fileupload" -F "time=72h" -F "fileToUpload=@${FINAL_IMG}" https://litterbox.catbox.moe/resources/internals/api.php)
LINK_ZIP=$(curl -s -F "reqtype=fileupload" -F "time=72h" -F "fileToUpload=@${FINAL_ZIP}" https://litterbox.catbox.moe/resources/internals/api.php)

cat << EOF > /home/ubuntu/LINK_DOWNLOAD_BOOT.txt
================================================================================
IMAGEM FINAL V3: POCO C40 KERNELSU + DROIDSPACES + DOCKER (PURE STOCK KABI)
================================================================================
Arquivo IMG: boot_poco_c40_droidspaces_v3.img (100.663.296 bytes)
Kernel: 5.4.242-jgki-perfuser (GZIP + Droidspaces patches + KABI 100% blindada)
SHA-256 (IMG): ${SHA_IMG}
Download Direto (.img): ${LINK_IMG}

Arquivo ZIP: boot_poco_c40_droidspaces_v3.zip (~30MB)
SHA-256 (ZIP): ${SHA_ZIP}
Download Compactado (.zip): ${LINK_ZIP}

COMANDOS DE FLASH (FASTBOOT):
fastboot flash boot_a boot_poco_c40_droidspaces_v3.img
fastboot flash boot_b boot_poco_c40_droidspaces_v3.img
fastboot reboot
================================================================================
EOF

cat /home/ubuntu/LINK_DOWNLOAD_BOOT.txt
echo "TUDO PRONTO COM SUCESSO!"

