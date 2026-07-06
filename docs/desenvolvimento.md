# Registro de desenvolvimento — firmware

Estado do projeto, decisões técnicas e resultados de validação.
Última atualização: 2026-07-05.

## Estado atual

| Fase | Descrição | Estado |
|---|---|---|
| 0 | Ambiente (CMake/Ninja/ARM GCC/slc-cli/SDK/Commander) | ✅ completo |
| 1 | Esqueleto compilável + flash + BLE advertising | ✅ validado na placa |
| 2 | Watchdog WDOG0 + causa de reset + `hang` | ✅ validado na placa |
| 3 | Aquisição PDM mono 48 kHz (LDMA + FIFO) | ✅ validado com mics reais |
| 4 | DSP: ponderação A + Fast + LAeq/LAFmax (CMSIS-DSP) | ✅ validado (±0,02 dB) |
| 5 | Datalog NVM3 + config persistida + CLI | ✅ validado na placa |
| 6 | Serviço BLE GATT customizado | ✅ validado ponta a ponta com o app (sync 198+31 registros, ACK, config, time sync) |
| 7 | Power manager + operação em bateria | ⏳ pendente |
| 8 | Espectro 1/3 de oitava (30 bandas, 20 Hz–16 kHz) | ✅ validado (1 kHz → 93,99 dB, 0 overruns) |

**Espectro (fase 8):** fs subiu para **48 kHz exato** (o prescaler inteiro do
PDM sobre 38,4 MHz fazia os "32 kHz" rodarem a 33,3 kHz reais); banco
multitaxa IEC 61260 (Butterworth ordem 6 por banda, 5 estágios ÷4,
anti-alias ordem 10, projeto/validação em `tools/design_third_octave.py`);
SYSCLK a 76,8 MHz **via DPLL travado no HFXO** (38,4 MHz não comporta o
banco; HFRCO livre impede o boot da stack BLE); LZeq por banda gravado num
anel NVM3 paralelo e sincronizado pela característica Spectra
(protocolo_ble.md). Lição de build: `slc generate` preserva config files já
existentes — use `./build.ps1 -Clean -Generate` para aplicar mudanças de
`configuration:` do slcp.

Bugs achados na validação com hardware (corrigidos): loop de watchdog com
anel NVM3 cheio (→ watchdog em 2 estágios + **lotes de 10 registros por
objeto NVM3**), DONE do sync perdido com fila TX cheia (→ retry), START
fora da janela pós mass-erase (→ clamp). Detalhes nos commits.

## Decisões técnicas (e porquês)

- **fs = 48 kHz EXATO** (DSR 50, clock dos mics 2,4 MHz): o prescaler do PDM
  é inteiro sobre o clock de 38,4 MHz — a combinação antiga "32 kHz + DSR 64"
  rodava na verdade a 33,3 kHz (4,2% de erro, inaceitável para espectro).
  48 kHz × 50 divide 38,4 MHz sem resto. A 16 kHz o filtro A sairia da
  classe 2 da IEC 61672 por *warping* (`tools/design_a_weighting.py`).
- **CMSIS-DSP**: o Cortex-M33 tem extensão DSP/FPU; o pipeline usa
  `arm_q15_to_float` → `arm_biquad_cascade_df2T_f32` (3 seções) →
  `arm_power_f32`. Atenção à convenção do CMSIS: coeficientes do denominador
  entram **negados** (−a1, −a2).
- **PDM DSR = 64** → clock PDM = 2,048 MHz, centro da faixa standard dos mics
  (o default 32 dava 1,024 MHz, borda inferior da faixa).
- **Captura mono (mic esquerdo)**: foi estéreo durante o diagnóstico da
  pinagem; voltou a mono para devolver ~4,5 KB de RAM ao heap da stack BLE.
  RAM é o recurso crítico: estouros do heap causam 0x1007 no advertiser ou
  a stack nem inicializar (sem log!).
- **FIFO de 4 blocos com drenagem LIMITADA** entre ISR e loop principal:
  tolera ~32 ms de latência; a drenagem tem teto por passada para que
  sobrecarga de CPU vire contador de overruns, e não loop de watchdog.
- **NVM3 de 128 KB** (16 páginas de 8 KB): registros em **lotes de 10 por
  objeto** (o anel cheio com objetos individuais tornava boot scan/repack
  mais lentos que o watchdog) + anel paralelo de ~600 espectros (lotes de 3);
  repack amortizado via `nvm3_repack()` no loop principal.
- **Watchdog em dois estágios com health-flags**: ~64 s no boot (recuperação
  NVM3 pode bloquear segundos numa única chamada), ~8 s em operação;
  alimentado apenas se o áudio está fresco (<500 ms), a última escrita NVM3
  teve sucesso e a stack BLE está saudável.
- **SYSCLK 76,8 MHz via DPLL travado no HFXO**: 38,4 MHz não comporta o
  banco de filtros; HFRCO livre como SYSCLK impede o boot da stack BLE.
- **Sem RTC de bateria**: registros carimbam `(boot_id, uptime_s)`; o app faz
  *time sync* e o firmware persiste `epoch_boot` por boot (anel de 8 em NVM3).

## ⚠️ Armadilha: revisão da placa

A placa deste projeto é **BRD4184A rev. A02** (serigrafia no PCB) e **tem**
2 microfones. Os pinos mudam entre revisões (tabela em [setup.md](setup.md)).
Gerar o projeto para a placa errada (`--with brd4184b`) faz o PDM ler nível
constante (+FS DC) — sintoma de "microfone morto". Além disso, o EEPROM do
kit reporta "TB010A Rev. A01" no `commander-cli adapter probe`, o que **não**
reflete a revisão real do PCB — confie na serigrafia.

## Resultados de validação (na placa)

- **Suíte de sanidade** (`tools/sanity_test.ps1 -Port COM7`): 18/18 PASS —
  boot, medição periódica, exatidão do DSP, watchdog, persistência, dump.
- **Exatidão do DSP** (tom sintético via `testtone`, calibração −26 dBFS ↔ 94 dB SPL):

  | Tom @ −26 dBFS | Esperado | Medido |
  |---|---|---|
  | 1 kHz | 94,00 dB(A) | 93,99 |
  | 125 Hz | 77,81 dB(A) | 77,80 |
  | 4 kHz | 94,88 dB(A) | 94,86 |

- **Som ambiente** (sala silenciosa): LAeq ≈ 43–45 dB(A), LAFmax ≈ 45–48 dB(A),
  0 overruns de áudio, ~92 blocos/s.
- **Watchdog**: `hang` → reset em ~8 s, `RSTCAUSE` identificado, contador
  persistido, registros e configuração intactos.
- **BLE**: advertising como "SPL Logger"; conexão e leitura de características
  validadas com nRF Connect (protocolo em [protocolo_ble.md](protocolo_ble.md)).

## Comandos CLI (VCOM 115200)

`status` · `dump` (CSV) · `clear` · `cal <centi-dB>` · `interval <s>` ·
`testtone <Hz> <centi-dBFS>` (0 0 desliga) · `hang` (teste do watchdog)

## Pendências

1. **Fase 7**: `power_manager` (EM1/EM2), VCOM off em bateria, medição de
   consumo na CR2032 e estimativa de autonomia.
2. **Bonding BLE** (Just Works) nas características de escrita.
3. Validar o fluxo completo de sync (START/ACK/DONE) com o app Android.
4. Calibração acústica fina com fonte de referência (comando `cal`).
