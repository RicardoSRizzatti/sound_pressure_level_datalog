# Registro de desenvolvimento — firmware

Estado do projeto, decisões técnicas e resultados de validação.
Última atualização: 2026-07-05.

## Estado atual

| Fase | Descrição | Estado |
|---|---|---|
| 0 | Ambiente (CMake/Ninja/ARM GCC/slc-cli/SDK/Commander) | ✅ completo |
| 1 | Esqueleto compilável + flash + BLE advertising | ✅ validado na placa |
| 2 | Watchdog WDOG0 + causa de reset + `hang` | ✅ validado na placa |
| 3 | Aquisição PDM estéreo 32 kHz (LDMA + FIFO) | ✅ validado com mics reais |
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

- **fs = 32 kHz** (não 16 kHz): a 16 kHz o filtro de ponderação A sai da
  tolerância classe 2 da IEC 61672 em 6,3 kHz por *warping* da transformação
  bilinear; a 32 kHz fica dentro em toda a faixa verificada
  (`tools/design_a_weighting.py`).
- **CMSIS-DSP**: o Cortex-M33 tem extensão DSP/FPU; o pipeline usa
  `arm_q15_to_float` → `arm_biquad_cascade_df2T_f32` (3 seções) →
  `arm_power_f32`. Atenção à convenção do CMSIS: coeficientes do denominador
  entram **negados** (−a1, −a2).
- **PDM DSR = 64** → clock PDM = 2,048 MHz, centro da faixa standard dos mics
  (o default 32 dava 1,024 MHz, borda inferior da faixa).
- **Captura estéreo, SPL no canal 0**: os dois mics são adquiridos
  (diagnóstico por canal no `status`); o DSP usa o esquerdo.
- **FIFO de 4 blocos** entre ISR e loop principal: tolera ~40 ms de latência
  (prints de CLI, escrita NVM3) sem perder áudio. Não aumentar sem medir RAM:
  fila de 8 blocos estéreo estourou o heap da stack BLE
  (`sl_bt_advertiser_create_set` → 0x1007 → assert → loop de watchdog).
- **NVM3 de 128 KB** (16 páginas de 8 KB): anel de 2000 registros de 16 bytes
  com folga para wear-leveling. O default de 40 KB não comporta.
- **Watchdog com health-flags**: WDOG0 (~2 s) é alimentado no loop principal
  apenas se o áudio está fresco (<500 ms), a última escrita NVM3 teve sucesso
  e a stack BLE está saudável.
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
- **Watchdog**: `hang` → reset em ~2 s, `RSTCAUSE` identificado, contador
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
