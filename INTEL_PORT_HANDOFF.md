# Handoff — port Intel do nxnative / NX Play

## Objetivo

Permitir que o NX Play/nxnative tenha um build para Macs Intel (`x86_64`) capaz de executar o código AArch64 dos jogos de Switch 1 e Switch 2. O caminho Intel usa tradução dinâmica AArch64 → x86_64; não é apenas uma recompilação nativa do código do jogo.

## Checkouts

- Core: `/workspace/nxnative`
- Wrapper: `/workspace/nx-native-wrapper`
- O trabalho está deliberadamente sem commit. Preservar as alterações existentes.

## O que já foi implementado

### Core (`nxnative`)

- Backend Dynarmic vendorizado em `dm/third_party/dynarmic`.
- Backend Intel em:
  - `dm/host/guest_cpu_dynarmic.cpp`
  - `dm/host/guest_cpu_abi_x86_64.S`
  - `dm/host/guest_cpu_guest_abi_x86_64.S`
  - `dm/host/guest_cpu.h`
  - `dm/host/intel/CMakeLists.txt`
- Tradução de AArch64 para x86_64 com:
  - CPU, memória, TLS/TPIDR, gates e SVCs sintéticos;
  - chamadas host → guest e guest → host;
  - HLE e imports não implementados;
  - ponte de ABI para registradores, argumentos de stack e retornos indiretos (`sret`) de estruturas grandes;
  - entrada de threads traduzidas.
- Makefile seleciona o backend com `DM_GUEST_CPU_DYNARMIC=1` e remove assembly ARM incompatível do caminho Intel.
- libvpx e dav1d usam caminho C portátil no build Intel.
- GL, Vulkan, NVN, codecs, JIT, callbacks e serviços HLE receberam adaptações para não chamar endereços AArch64 como se fossem funções x86_64.
- `scripts/build_ipa_from_nsp.py` escolhe automaticamente `native` em Apple Silicon e `intel` em Mac Intel.

### Wrapper (`nx-native-wrapper`)

- `project.yml` configura o app como universal: `arm64 x86_64`.
- `bin/build` e `bin/release` verificam as duas arquiteturas.
- `bin/setup`, README e CHANGELOG explicam que o caminho Intel ainda é experimental.

## Validações já realizadas no ambiente Linux

O smoke test passou:

```sh
cd /workspace/nxnative
PYTHONPATH=/workspace/.codex-cmake \
PATH=/workspace/.codex-cmake/bin:$PATH \
cmake --build dm/host/intel/build --target dm_guest_cpu_smoke --parallel
dm/host/intel/build/dm_guest_cpu_smoke
```

Resultado esperado:

```text
translated CPU, TLS, ABI and HLE/gate smoke passed
```

Também passaram:

- 18 testes de `dm/tools/tests/test_nsp_env_checks.py`;
- smoke de callbacks host→guest com 8 argumentos em registradores e 10 argumentos, incluindo argumentos na stack traduzida;
- `python3 -m py_compile scripts/build_ipa_from_nsp.py`;
- `bash -n` dos scripts do wrapper;
- `git diff --check` nos dois repositórios;
- `make -C dm/host -n GAME=0100000000010000 DM_GUEST_CPU_DYNARMIC=1 dmhost`.

## O que ainda não foi comprovado

O ambiente anterior era Linux, sem Xcode ou SDK macOS. Ainda é necessário testar em um Mac Intel real:

1. Configuração e compilação completa do `dmhost` com Xcode/SDK macOS.
2. Link final do app e do backend Dynarmic em `x86_64`.
3. Boot de pelo menos um jogo de Switch 1.
4. Boot de pelo menos um jogo de Switch 2.
5. Correções de runtime descobertas no primeiro boot.
6. JIT gerado em runtime, callbacks complexos, funções variádicas e assinaturas ABI mistas.
7. Pausa, retomada, crash dump e contexto de threads traduzidas.

Não declarar que jogos já funcionam antes desses testes.

## Próximos comandos no Mac Intel

Primeiro verificar o ambiente:

```sh
uname -m
xcodebuild -version
xcrun --sdk macosx --show-sdk-path
```

Instalar as dependências do backend, se necessário:

```sh
brew bundle --file /caminho/para/nx-native-wrapper/Brewfile
```

Executar o smoke test do tradutor:

```sh
cd /caminho/para/nxnative
make -C dm/host/intel check
```

Depois testar o host completo com um title ID real:

```sh
make -C dm/host \
  GAME=0100000000010000 \
  DM_GUEST_CPU_DYNARMIC=1 \
  dmhost
```

O novo agente deve registrar o primeiro erro completo do compilador/linker/runtime, corrigir uma camada por vez e repetir os testes. Não usar `git reset --hard`, `git checkout --` ou apagar alterações existentes.

## Instrução para o novo chat

“Leia `/caminho/para/nxnative/INTEL_PORT_HANDOFF.md`, inspecione os diffs existentes nos dois repositórios e continue o port Intel. Estou executando em um Mac Intel com Xcode. Primeiro valide o ambiente e rode o smoke test; depois tente o build completo do `dmhost`. Preserve todas as alterações existentes, não faça commit sem pedir e não declare compatibilidade de jogos antes de um boot real.”
