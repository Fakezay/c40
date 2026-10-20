# 🐧 Xiaomi POCO C40 (`frost` / JLQ JR510) — Kernel `5.4.242-jgki-perfuser` com KernelSU v0.9.5 + Docker + Droidspaces-OSS

Este repositório contém o código-fonte completo, patches de preservação de KABI, scripts de compilação/reempacotamento e documentação técnica da engenharia reversa realizada para portar **KernelSU (`v0.9.5` via Manual Hooks)**, **Docker Engine (`moby`)** e **Droidspaces-OSS** para o **Xiaomi POCO C40 (`frost`, SoC JLQ JR510)** mantendo **100% de compatibilidade binária (KABI)** com os módulos proprietários pré-compilados da partição `vendor` (`rmnet_core.ko`, `mali_kbase.ko`, `qca_cld3_wlan.ko`, `slg_main.ko`).

---

## 📥 Downloads Diretos (`boot.img` Prontos para Flash)

Você pode baixar as imagens prontas na aba [**Releases**](https://github.com/Fakezay/c40/releases) deste repositório ou pelos links diretos abaixo:

| Versão | Recursos Ativados | Link GitHub Release (`.img` 96 MB) | Link GitHub Release (`.zip` 30 MB) | Status no Aparelho |
| :--- | :--- | :--- | :--- | :--- |
| **v3.0** (`v3.0-kernelsu-droidspaces-docker`) | **Droidspaces-OSS + Docker + KernelSU** (`DEVTMPFS`, `USER_NS`, `MACVLAN`, `POSIX_ACL`, `IP_SET`, UFW/Fail2ban, Patches KABI 5.4) | [**Baixar v3.0 `.img`**](https://github.com/Fakezay/c40/releases/download/v3.0-kernelsu-droidspaces-docker/boot_poco_c40_droidspaces_v3.img) | [**Baixar v3.0 `.zip`**](https://github.com/Fakezay/c40/releases/download/v3.0-kernelsu-droidspaces-docker/boot_poco_c40_droidspaces_v3.zip) | Compilado + Validado via `pahole` |
| **v2.0** (`v2.0-kernelsu-docker-4extra-working`) | **Docker + 4 Extras** (`XT_MATCH_ADDRTYPE`, `CGROUP_DEVICE`, `CGROUP_PIDS`, `POSIX_MQUEUE`) + **KernelSU v0.9.5** | [**Baixar v2.0 `.img`**](https://github.com/Fakezay/c40/releases/download/v2.0-kernelsu-docker-4extra-working/boot_poco_c40_docker_v2_4extra.img) | [**Baixar v2.0 `.zip`**](https://github.com/Fakezay/c40/releases/download/v2.0-kernelsu-docker-4extra-working/boot_poco_c40_docker_v2_4extra.zip) | ✅ **Testado e 100% Funcional** |
| **v1.0** (`v1.0-kernelsu-docker-working`) | **KernelSU v0.9.5 + Docker Base** (Namespaces, Cgroups, OverlayFS, VETH, Bridge, Netfilter/NAT) | [**Baixar v1.0 `.img`**](https://github.com/Fakezay/c40/releases/download/v1.0-kernelsu-docker-working/boot_poco_c40_kernelsu_docker_pure_stock_abi.img) | [**Baixar v1.0 `.zip`**](https://github.com/Fakezay/c40/releases/download/v1.0-kernelsu-docker-working/boot_poco_c40_kernelsu_docker_pure_stock_abi.zip) | ✅ **Testado e 100% Funcional** |

> 📄 Para links de download da **ROM Stock oficial (2.3 GB)**, **Toolchains** e **Mirrors Catbox**, consulte [**`docs/EXTERNAL_LINKS_AND_DOWNLOADS.md`**](docs/EXTERNAL_LINKS_AND_DOWNLOADS.md).

---

## ⚡ Como Flashear via Fastboot

```bash
fastboot flash boot_a boot_poco_c40_droidspaces_v3.img
fastboot flash boot_b boot_poco_c40_droidspaces_v3.img
fastboot reboot
```

---

## 🔬 Resumo da Engenharia Reversa e Blindagem de KABI (JLQ JR510)

O kernel `5.4.242-jgki-perfuser` do POCO C40 carrega dezenas de módulos `.ko` proprietários durante o boot (incluindo `rmnet_core.ko` para dados móveis, `mali_kbase.ko` para a GPU Mali-G52 e `qca_cld3_wlan.ko` para o Wi-Fi). Qualquer deslocamento de offset nas estruturas centrais do kernel causa *Kernel Panic* ou *bootloop*.

### Offsets Críticos Verificados com `pahole` no `vmlinux`:
| Estrutura | Campo | Offset Obrigatório (Stock) | Quem depende |
| :--- | :--- | :--- | :--- |
| `struct net_device` | `rx_handler` | **`792` (`0x318`)** | `rmnet_core.ko` (`rmnet_rx_handler`) |
| `struct net_device` | `pcpu_refcnt` | **`1144` (`0x478`)** | `rmnet_core.ko` / `qca_cld3_wlan.ko` |
| `struct net` | `gen` | **`3168` (`0xc60`)** | `rmnet_core.ko` (`net_generic()`) |
| `struct sk_buff` | `enum skb_ext_id` | Sem `BRIDGE_NETFILTER` | `rmnet_core.ko` / `qca_cld3_wlan.ko` |

### Patches Aplicados no Código-Fonte:
1. **KernelSU v0.9.5 Manual Hooks (`fs/exec.c`, `fs/open.c`, `fs/read_write.c`, `fs/stat.c`, `drivers/input/input.c`, `drivers/tty/pty.c`, `security/selinux/hooks.c`):**
   - Integração nativa sem depender de `CONFIG_KPROBES`.
   - Correção na assinatura de `ksu_handle_devpts(struct inode *)` para evitar corrupção da pilha ao abrir pseudoterminais (`pty`).
2. **Desacoplamento de `NET_INGRESS` e `NETFILTER_INGRESS` em `include/linux/netdevice.h`:**
   - Permite ativar `CONFIG_NETFILTER=y` para o Docker mantendo `nf_hooks_ingress` oculto quando `CONFIG_NETFILTER_INGRESS` não está definido, preservando os offsets `792` e `1144` em `struct net_device`.
3. **Patches Oficiais do Droidspaces-OSS para Kernel 5.4 GKI:**
   - `include/linux/sched.h`: Armazena `sysvsem` e `sysvshm` dentro dos paddings `ANDROID_KABI_RESERVE(6, 7, 8)` da `struct task_struct`.
   - `include/linux/sched/user.h`: Armazena `mq_bytes` (`CONFIG_POSIX_MQUEUE=y`) dentro de `ANDROID_KABI_USE(1, ...)` da `struct user_struct`.
   - `kernel/cgroup/cgroup.c`: Restaura symlinks com prefixo `cgroup.` quando `CGRP_ROOT_NOPREFIX` está ativo para compatibilidade com `systemd` dentro de contêineres.

---

## 📂 Estrutura Extra Adicionada ao Repositório

- [`docs/entendimentoAoDevolvKernelEKernelSu.txt`](docs/entendimentoAoDevolvKernelEKernelSu.txt): Diário técnico completo (28 seções) documentando todas as descobertas, causas de falha (LZ4 vs GZIP, AVB, CFI, KABI offsets) e validações.
- [`docs/EXTERNAL_LINKS_AND_DOWNLOADS.md`](docs/EXTERNAL_LINKS_AND_DOWNLOADS.md): Links para download da ROM Stock, toolchains, repositórios externos e imagens `.img`/`.zip`.
- [`scripts_frost/build_abi_fixed_and_repack.sh`](scripts_frost/build_abi_fixed_and_repack.sh): Script automatizado que compila o kernel, valida os offsets `792`, `1144` e `3168` via `pahole`, reempacota com `magiskboot` e assina com `avbtool.py`.
- [`configs_frost/`](configs_frost/): Backup de todos os `.config` funcionais (`v1.0`, `v2.0`, `v3.0` e `config_stock_5.4.242-jgki-perfuser`).
- [`arch/arm64/configs/`](arch/arm64/configs/):
  - `frost_kernelsu_docker_defconfig` (v1.0)
  - `frost_kernelsu_docker_v2_4extra_defconfig` (v2.0)
  - `frost_kernelsu_droidspaces_v3_defconfig` (v3.0)
- [`docker.config`](docker.config): Fragmento de configuração com todas as flags KABI-safe para Docker e Droidspaces-OSS.
