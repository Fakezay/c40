# POCO C40 (frost / JLQ JR510) - KernelSU + Docker Working Build

## Status: 100% Funcional (MIUI / Android 11) - Testado e Validado no Aparelho!
- **KernelSU**: Suporte nativo completo (hooks manuais em `fs/exec.c`, `fs/open.c`, `fs/read_write.c`, `fs/stat.c`), `ksud`, `post-fs-data`, root funcional.
- **Docker Completo**: Suporte total habilitado e validado via `check-config.sh`:
  - Namespaces: PID, NET, IPC, UTS, SYSVIPC.
  - Cgroups: Memory (com swap accounting), CPU, CPUAcct, Block I/O (com throttling), Freezer, BPF, CPUSets, **Device (`CONFIG_CGROUP_DEVICE`)**, **PIDs (`CONFIG_CGROUP_PIDS`)**.
  - Rede & Pontes: Virtual Ethernet (`CONFIG_VETH`), Linux Bridge (`CONFIG_BRIDGE`), TUN/TAP, Dummy.
  - Firewall & NAT: IPTables, NAT Masquerade, Redirect, Conntrack, Mark, **`CONFIG_NETFILTER_XT_MATCH_ADDRTYPE`**.
  - IPC Avançado: **`CONFIG_POSIX_MQUEUE`** e **`CONFIG_POSIX_MQUEUE_SYSCTL`**.
  - Armazenamento: OverlayFS (`overlay2`), Binfmt_misc, Ext4 POSIX ACL.
- **Vendor KABI**: 100% preservada e alinhada aos módulos proprietários vendor (`mali_kbase.ko`, `rmnet_core.ko`, Wi-Fi `qca_cld3_wlan.ko`, etc.).

---

## Detalhes Críticos de KABI Resolvidos
1. **CFI Slowpath & CRC**:
   - `__cfi_slowpath` e `__cfi_slowpath_diag` exportados em `kernel/cfi.c` para satisfazer `mali_kbase.ko`.
   - `check_version` contornado em `kernel/module.c` para aceitar módulos proprietários compilados sem CRC idêntico.
2. **Rede e Offsets de `struct net_device`**:
   - O módulo vendor proprietário `rmnet_core.ko` acessa diretamente com assembly hardcoded:
     - `real_dev->rx_handler` no offset **792 (0x318)** (`ldr x8, [x20, #792]`)
     - `real_dev->pcpu_refcnt` no offset **1144 (0x478)** (`ldr x9, [x20, #1144]`)
   - `CONFIG_IPVLAN` / `CONFIG_NET_L3_MASTER_DEV` e `CONFIG_VLAN_8021Q` foram desativados para evitar inserção de campos que quebravam estes offsets.
3. **Offsets de `struct net`**:
   - Campos antes de `gen` (offset **3168 / 0xc60**) 100% idênticos à stock defconfig (`CONFIG_IP_VS` desativado).
4. **O Que Deve Permanecer Rigorosamente DESATIVADO**:
   - `# CONFIG_BRIDGE_NETFILTER is not set`: Evita deslocamento do enum `skb_ext_id` em `include/linux/skbuff.h`, prevenindo pânico de rede no Wi-Fi/Modem.
   - `# CONFIG_USER_NS is not set`: Evita violações de SELinux nos kworkers do Android 11.

---

## Arquivos de Configuração Salvos
- **Defconfig v2.0 (Final)**: `arch/arm64/configs/frost_kernelsu_docker_v2_working_final_defconfig`
- **Defconfig v1.0 (Base)**: `arch/arm64/configs/frost_kernelsu_docker_working_final_defconfig`
- **Config de backup**: `/home/ubuntu/frost_working_configs/frost_kernelsu_docker_v2_working_final.config`
- **Diretório de backup completo**: `/home/ubuntu/frost_working_configs/`

---

## Como Recompilar e Empacotar
Execute o script automatizado:
```bash
bash /home/ubuntu/build_abi_fixed_and_repack.sh
```

Ou manualmente:
```bash
export PATH="/home/ubuntu/toolchains/clang-12-arm64-native/bin:/usr/bin:$PATH"
export ARCH=arm64
export SUBARCH=arm64
export CLANG_TRIPLE=aarch64-linux-gnu-
export CROSS_COMPILE=/usr/bin/aarch64-linux-gnu-
export CROSS_COMPILE_COMPAT=/home/ubuntu/toolchains/gcc32/bin/arm-linux-androideabi-

make O=out ARCH=arm64 frost_kernelsu_docker_v2_working_final_defconfig
make O=out ARCH=arm64 -j$(nproc) Image.gz Image
```

Reempacotamento do `boot.img`:
```bash
cp out/arch/arm64/boot/Image kernel
PATCHVBMETAFLAG=true magiskboot repack <stock_boot.img>
python3 avbtool.py add_hash_footer --image new-boot.img --partition_size 100663296 --partition_name boot
```

---

## Imagens Prontas e Comandos de Flash (Fastboot)

### Imagem v2.0 (Recomendada - Com as 4 Opções do `oxi`):
- Arquivo: `boot_poco_c40_docker_v2_4extra.img` (100.663.296 bytes)
- SHA-256: `757acc2e5b2dc2d2817771795a7180fd0d9bc0779d866a56987c6ef87f7ce93e`
- Link direto: https://files.catbox.moe/796flj.img
- Link ZIP: https://files.catbox.moe/pv04q8.zip

```bash
fastboot flash boot_a boot_poco_c40_docker_v2_4extra.img
fastboot flash boot_b boot_poco_c40_docker_v2_4extra.img
fastboot reboot
```
