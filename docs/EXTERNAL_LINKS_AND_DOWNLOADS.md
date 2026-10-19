# 📦 Arquivos Externos, Toolchains, ROM Stock e Imagens Pré-Compiladas (`boot.img`)

Para manter o repositório Git leve e rápido de clonar, arquivos binários gigantes genéricos (ROM Stock de 2.3 GB, Toolchains compiladores e imagens `.img`/`.zip` de 96 MB) não são comitados diretamente na árvore de código, sendo disponibilizados através dos links oficiais e das **GitHub Releases** deste repositório (`Fakezay/c40`).

---

## 1. Imagens `boot.img` Prontas para Flash (GitHub Releases + Mirrors)

Todas as imagens abaixo foram compiladas a partir deste repositório, reempacotadas com `magiskboot` (`KERNEL_FMT [gzip]`, `PATCHVBMETAFLAG=true`) e assinadas com `avbtool.py` (`--partition_size 100663296`).

### 🚀 Release v3.0 — Droidspaces-OSS + Docker + KernelSU (Mais Recente)
Inclui todas as opções da v2.0 + patches oficiais KABI 5.4 do Droidspaces-OSS (`sched.h`, `user.h`, `cgroup.c`) + `CONFIG_DEVTMPFS=y`, `CONFIG_USER_NS=y`, `CONFIG_MACVLAN=y`, `CONFIG_TMPFS_POSIX_ACL=y`, `CONFIG_TMPFS_XATTR=y`, `CONFIG_FW_LOADER_COMPRESS=y`, `CONFIG_IP_SET=y` e módulos Netfilter para UFW/Fail2ban.
- **GitHub Release (Link Permanente):**
  - Página da Release: https://github.com/Fakezay/c40/releases/tag/v3.0-kernelsu-droidspaces-docker
  - Download Direto `.img` (96 MB): `https://github.com/Fakezay/c40/releases/download/v3.0-kernelsu-droidspaces-docker/boot_poco_c40_droidspaces_v3.img`
  - Download Compactado `.zip` (~30 MB): `https://github.com/Fakezay/c40/releases/download/v3.0-kernelsu-droidspaces-docker/boot_poco_c40_droidspaces_v3.zip`
- **Mirror Catbox/Litterbox:**
  - `.img`: https://litter.catbox.moe/7peiv6.img (`SHA-256: d33d6707c62a0359e3a99ae973f73fc10b4d9d20194ebf24af66bfb722e57dbd`)
  - `.zip`: https://litter.catbox.moe/gwi5cu.zip (`SHA-256: 0bab00c462b9107c921f9d39e97a2094089f03f7508862ae30647234586ce102`)

---

### ✅ Release v2.0 — Docker + 4 Extras (`XT_MATCH_ADDRTYPE`, `CGROUP_DEVICE`, `CGROUP_PIDS`, `POSIX_MQUEUE`) + KernelSU
Confirmada 100% funcional no aparelho (Wi-Fi, Modem, GPU Mali, Display e KernelSU).
- **GitHub Release (Link Permanente):**
  - Página da Release: https://github.com/Fakezay/c40/releases/tag/v2.0-kernelsu-docker-4extra-working
  - Download Direto `.img` (96 MB): `https://github.com/Fakezay/c40/releases/download/v2.0-kernelsu-docker-4extra-working/boot_poco_c40_docker_v2_4extra.img`
  - Download Compactado `.zip` (~30 MB): `https://github.com/Fakezay/c40/releases/download/v2.0-kernelsu-docker-4extra-working/boot_poco_c40_docker_v2_4extra.zip`
- **Mirror Catbox Permanente:**
  - `.img`: https://files.catbox.moe/796flj.img (`SHA-256: 757acc2e5b2dc2d2817771795a7180fd0d9bc0779d866a56987c6ef87f7ce93e`)
  - `.zip`: https://files.catbox.moe/pv04q8.zip (`SHA-256: 282331ab7740f022ea3dcbe7377f3e936e3c966285d7bfb46f9f275a296e75b6`)

---

### ✅ Release v1.0 — KernelSU + Docker Base (Pure Stock ABI)
Primeira versão com Docker e KernelSU v0.9.5 (manual hooks) confirmada com boot limpo e todos os periféricos operando.
- **GitHub Release (Link Permanente):**
  - Página da Release: https://github.com/Fakezay/c40/releases/tag/v1.0-kernelsu-docker-working
  - Download Direto `.img` (96 MB): `https://github.com/Fakezay/c40/releases/download/v1.0-kernelsu-docker-working/boot_poco_c40_kernelsu_docker_pure_stock_abi.img`
  - Download Compactado `.zip` (~30 MB): `https://github.com/Fakezay/c40/releases/download/v1.0-kernelsu-docker-working/boot_poco_c40_kernelsu_docker_pure_stock_abi.zip`
- **Mirror Catbox Permanente:**
  - `.img`: https://files.catbox.moe/2z324i.img (`SHA-256: e0a3af2e64c7e192bef75864aed5bcb59b5fa9ce843cd73ac0f054e009b64b4a`)
  - `.zip`: https://files.catbox.moe/olu5cu.zip (`SHA-256: e519f8fd8c7b824fb41a217a1a8c3c24ff0c7557a83c34bb5be5df6eb10e9a2a`)

---

## 2. ROM Stock Oficial (Xiaomi POCO C40 - `frost` Global V13.0.25.0.RGFMIXM)

Usada para extrair o `boot.img` original de fábrica (ramdisk e cabeçalho Header v3) e os módulos `.ko` do `vendor.img` para auditoria de KABI:
- **Versão:** MIUI `V13.0.25.0.RGFMIXM` (Android 11.0 Global)
- **Nome do Arquivo:** `frost_global_images_V13.0.25.0.RGFMIXM_20231226.0000.00_11.0_global_cfa2ca6e21.tgz` (~2.32 GB)
- **Link Oficial Xiaomi (BigOTA / Bilibili Mirror):**
  - `https://bigota.d.miui.com/V13.0.25.0.RGFMIXM/frost_global_images_V13.0.25.0.RGFMIXM_20231226.0000.00_11.0_global_cfa2ca6e21.tgz`
  - Mirror alternativo: `https://bkt-sgp-miui-ota-update-alisgp.oss-ap-southeast-1.aliyuncs.com/V13.0.25.0.RGFMIXM/frost_global_images_V13.0.25.0.RGFMIXM_20231226.0000.00_11.0_global_cfa2ca6e21.tgz`

---

## 3. Toolchains de Compilação e Utilitários Externos

Para reproduzir a compilação em qualquer máquina Ubuntu/Debian (`x86_64`):

1. **LLVM / Clang + LLD + pahole (Pacotes nativos do Ubuntu ou AOSP Prebuilts):**
   ```bash
   sudo apt update
   sudo apt install -y clang lld llvm gcc-aarch64-linux-gnu dwarves bc bison flex libssl-dev libelf-dev zip python3
   ```
   *(Opcional — Toolchain Clang r383902b da Google/AOSP caso queira usar o compilador exato do Android 11):*
   - `https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86/+archive/refs/heads/android11-release/clang-r383902b.tar.gz`

2. **AVBTool (`avbtool.py`) Oficial do AOSP (para assinar a partição `boot` de 96 MB):**
   - `https://android.googlesource.com/platform/external/avb/+/refs/heads/android11-release/avbtool.py`

3. **Magiskboot (`x86_64`):**
   - Extraído do APK oficial do Magisk (`lib/x86_64/libmagiskboot.so`): https://github.com/topjohnwu/Magisk/releases

4. **Repositórios de Referência Integrados:**
   - **KernelSU (`v0.9.5` — último com suporte a kernel 5.4 non-GKI/JGKI via manual hooks):** https://github.com/tiann/KernelSU/tree/v0.9.5 (Aplicativo gerenciador: `KernelSU_v0.9.5_11872-release.apk`)
   - **Droidspaces-OSS (Patches KABI 5.4 e documentação):** https://github.com/ravindu644/Droidspaces-OSS
