# Setup do ambiente de desenvolvimento

Ferramentas necessárias para compilar e gravar o firmware (Windows). O script
`tools/setup_env.ps1` verifica tudo isso automaticamente.

| Ferramenta | Versão | Instalação | Local esperado |
|---|---|---|---|
| CMake | ≥ 3.25 | `winget install Kitware.CMake` | PATH |
| Ninja | ≥ 1.11 | `winget install Ninja-build.Ninja` | PATH |
| Java (p/ slc-cli) | OpenJDK 17 | `winget install Microsoft.OpenJDK.17` | PATH |
| ARM GNU Toolchain | 12.2.Rel1 | zip de developer.arm.com | `%USERPROFILE%\silabs\arm-gnu-toolchain` |
| slc-cli | mais recente | zip de silabs.com | `%USERPROFILE%\silabs\slc_cli` |
| Simplicity SDK | v2025.6.3 | `git clone --depth 1 --branch v2025.6.3 https://github.com/SiliconLabs/simplicity_sdk` | `%USERPROFILE%\silabs\simplicity_sdk` |
| Simplicity Commander (CLI) | ≥ 1.24 | zip de silabs.com | `%USERPROFILE%\silabs\commander` |

Os locais podem ser sobrescritos pelas variáveis de ambiente `SISDK_ROOT`,
`SLC_PATH`, `ARM_GCC_DIR` e `COMMANDER` (ver `build.ps1`).

## Configuração única do slc-cli

Depois de instalar, aponte o slc para o SDK e confie na assinatura dele:

```powershell
slc configuration --sdk $env:USERPROFILE\silabs\simplicity_sdk
slc signature trust --sdk $env:USERPROFILE\silabs\simplicity_sdk
slc configuration --gcc-toolchain $env:USERPROFILE\silabs\arm-gnu-toolchain
```

## Fluxo de build

```powershell
./build.ps1 -Generate   # regenera o projeto SLC (necessário quando app.slcp muda)
./build.ps1             # build incremental (cmake + ninja)
./build.ps1 -Flash      # build + grava na placa via J-Link on-board
```

## Placa

Thunderboard EFR32BG22 (SLTB010A), **placa BRD4184A rev. A02** (serigrafia no
PCB). O projeto é gerado com `--with brd4184a` (default do `build.ps1`;
use `-Board brd4184b` para a revisão B).

⚠️ Os pinos dos microfones mudam entre revisões — gerar para a placa errada
faz o PDM ler nível constante (parece "sem microfone"):

| Sinal | BRD4184A (esta) | BRD4184B |
|---|---|---|
| PDM CLK | PC6 | PB0 |
| PDM DATA | PC7 | PB1 |
| Mic enable | PA0 | PC7 |

- MCU: EFR32BG22C224F512IM40 (Cortex-M33 c/ DSP+FPU, 512 KB flash, 32 KB RAM)
- 2× microfones PDM MEMS (esquerdo/direito) + Si7021 (RH/T) + IMU + luz
- Debugger J-Link on-board com VCOM (115200 8N1); driver SEGGER J-Link
  necessário no Windows (winget `NordicSemiconductor.JLink`)

## Testes de sanidade

Com a placa no USB e firmware gravado:

```powershell
./tools/sanity_test.ps1 -Port COM7
```

Valida boot, medição periódica, exatidão do DSP (tom sintético de 1 kHz a
−26 dBFS deve dar LAeq = 94,00 dB(A) — comando `testtone`), recuperação por
watchdog e persistência do NVM3.
