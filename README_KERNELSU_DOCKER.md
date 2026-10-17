# POCO C40 (frost / JLQ JR510) - KernelSU + Docker Working Build

## Status: 100% Funcional (MIUI / Android 11)
- **KernelSU**: Suporte nativo completo (hooks manuais em `fs/exec.c`, `fs/open.c`, `fs/read_write.c`, `fs/stat.c`), `ksud`, `post-fs-data`, root funcional.
- **Docker**: Suporte total habilitado (Namespaces PID, NET, IPC, UTS; Cgroups v1 CPU, cpuset, memory + swap, io/blkio, freezer; VETH, Bridge, IPTables, NAT Masquerade, OverlayFS/overlay2).
- **Vendor KABI**: 100% preservada e alinhada aos módulos proprietários vendor (`mali_kbase.ko`, `rmnet_core.ko`, etc.).

---

## Detalhes Críticos de KABI Resolvidos
1. **CFI Slowpath & CRC**:
   - `__cfi_slowpath` e `__cfi_slowpath_diag` exportados para satisfazer `mali_kbase.ko`.
   - `check_version` contornado em `kernel/module.c` para aceitar módulos proprietários compilados sem CRC idêntico.
2. **Cgroups**:
   - Total de subsistemas mantido estritamente em **7** (`cpuset`, `cpu`, `cpuacct`, `io`, `memory`, `freezer`, `bpf`), idêntico à stock, evitando o deslocamento de ponteiros em `struct task_struct`.
3. **Rede e Offsets de `struct net_device`**:
   - O módulo vendor `rmnet_core.ko` acessa diretamente:
     - `real_dev->rx_handler` no offset **792 (0x318)** (`ldr x8, [x20, #792]`)
     - `real_dev->pcpu_refcnt` no offset **1144 (0x478)** (`ldr x9, [x20, #1144]`)
   - `CONFIG_IPVLAN` / `CONFIG_NET_L3_MASTER_DEV` e `CONFIG_VLAN_8021Q` foram desativados para evitar inserção de campos que quebravam estes offsets.
4. **Offsets de `struct net`**:
   - Campos antes de `gen` (offset **3168 / 0xc60**) 100% idênticos à stock defconfig (`CONFIG_IP_VS` desativado).

---

## Arquivos de Configuração Salvos
- Defconfig no repo: `arch/arm64/configs/frost_kernelsu_docker_working_final_defconfig`
- Config de backup: `/home/ubuntu/frost_kernelsu_docker_working_final.config`
- Diretório de backup: `/home/ubuntu/frost_working_configs/`

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

make O=out ARCH=arm64 frost_kernelsu_docker_working_final_defconfig
make O=out ARCH=arm64 -j$(nproc) Image.gz Image
```

Reempacotamento do `boot.img`:
```bash
cp out/arch/arm64/boot/Image kernel
PATCHVBMETAFLAG=true magiskboot repack <stock_boot.img>
python3 avbtool.py add_hash_footer --image new-boot.img --partition_size 100663296 --partition_name boot
```

---

## Comandos de Flash (Fastboot)
```bash
fastboot flash boot_a boot_poco_c40_kernelsu_docker_pure_stock_abi.img
fastboot flash boot_b boot_poco_c40_kernelsu_docker_pure_stock_abi.img
fastboot reboot
```
