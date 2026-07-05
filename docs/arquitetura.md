# Arquitetura do firmware — explicação didática

Este documento explica **como** e **por que** o firmware funciona, conceito a
conceito. Se você quiser só compilar, veja [setup.md](setup.md).

## Visão geral

```
[Mics PDM] → [Periférico PDM] → [LDMA] → [DSP: filtro A + Fast] → [LAeq/LAFmax 20 s]
                                                                        │
                          [App celular] ←─ BLE GATT ─┐                  ▼
                                                     ├──────────── [NVM3 (flash)]
                          [Serial VCOM p/ debug] ────┘
                     (Watchdog WDOG0 vigiando o loop principal o tempo todo)
```

## 1. Microfones PDM — o que é PDM?

Os dois microfones da placa (Knowles SPK0641HT4H-1) são digitais com saída
**PDM** (*Pulse Density Modulation*): em vez de entregarem amostras de 16 bits,
entregam um fluxo de bits em alta frequência (~1–3 MHz) onde a *densidade* de
1s representa a amplitude do som. O EFR32BG22 tem um **periférico PDM em
hardware** que faz a *decimação*: converte esse fluxo de bits em amostras PCM
de 16 bits na taxa que pedirmos (usamos **32 kHz**: verificamos que a 16 kHz o
filtro de ponderação A sai da tolerância da IEC 61672 em 6,3 kHz por causa do
*warping* da transformação bilinear; a 32 kHz fica dentro da classe 2 em toda a
faixa verificada — ver `tools/design_a_weighting.py`).

Usamos o driver pronto do SDK (`sl_mic.h`), que configura o periférico PDM e o
**LDMA** (DMA de baixa energia): as amostras vão direto para a RAM **sem a CPU
tocar em cada amostra**, em dois buffers alternados (*ping-pong*) — enquanto um
enche, o outro é processado. A CPU só acorda quando um bloco de 10 ms fica
pronto.

## 2. Ponderação A — por que filtrar?

O ouvido humano não percebe todas as frequências com a mesma sensibilidade:
graves e agudos extremos parecem mais "baixos" do que médios. A **curva de
ponderação A** (norma IEC 61672) simula isso, atenuando graves/agudos antes de
medir. Resultado em **dB(A)** — o padrão em normas de ruído ambiental e
ocupacional (NBR 10151, NR-15 etc.).

Implementação: uma cascata de **3 filtros biquad IIR** (6 polos no total),
com coeficientes projetados para fs = 32 kHz (gerados e validados pelo
`tools/design_a_weighting.py`, usando o venv do `signal_processing_toolbox`).
O processamento usa **CMSIS-DSP** aproveitando a extensão DSP/FPU do
Cortex-M33: `arm_q15_to_float` (conversão), `arm_biquad_cascade_df2T_f32`
(cascata de biquads) e `arm_power_f32` (energia do bloco) — custo de CPU
baixíssimo a 76,8 MHz.

## 3. Ponderação temporal Fast — o "medidor com inércia"

Um sonômetro não mostra o valor instantâneo: mostra uma média com "inércia"
definida por norma. **Fast** = média exponencial com constante de tempo
**τ = 125 ms** (Slow seria 1 s). Implementação por bloco:

```
energia_fast = α·(média de x² do bloco) + (1−α)·energia_fast
α = 1 − exp(−T_bloco / 0,125)      (T_bloco = 10 ms → α ≈ 0,077)
```

## 4. As métricas registradas a cada intervalo (20 s)

- **LAeq** — *nível sonoro contínuo equivalente*: o nível constante que teria a
  mesma energia acústica do som real no intervalo. É a "média energética":
  `LAeq = 10·log10( média de x²_ponderado_A )  + offset_calibração`
- **LAFmax** — o **maior** valor da leitura Fast dentro do intervalo (captura
  picos de curta duração que o LAeq dilui).

**Calibração**: o microfone entrega −26 dBFS quando exposto a 94 dB SPL @ 1 kHz
(dado de datasheet). Logo `SPL = dBFS + 120`. Um offset fino ajustável fica
guardado em NVM3 (comando `cal` / característica BLE).

## 5. NVM3 — armazenamento que sobrevive a reset

**NVM3** é o sistema de armazenamento chave-valor da Silicon Labs sobre a
flash interna, com *wear leveling* (espalha as escritas para não desgastar a
flash) e tolerância a queda de energia no meio da escrita. Cada registro tem
12 bytes: `{boot_id, seq, uptime_s, LAeq_cdB, LAFmax_cdB}` (níveis em
centésimos de dB, int16). Com ~64 KB reservados cabem >2000 registros em anel
(o mais novo sobrescreve o mais antigo).

Como a placa não tem relógio de tempo real com bateria, os registros usam
`boot_id + uptime`; o app faz *time sync* ao conectar e converte para hora
absoluta no download.

## 6. Watchdog — o "vigia" contra travamentos

O **WDOG0** é um contador em hardware que reinicia o chip se não for
"alimentado" a cada ~2 s. A alimentação acontece **no loop principal, e só se
o sistema estiver saudável**:

1. a ISR de áudio processou blocos recentemente (áudio vivo);
2. a última gravação NVM3 teve sucesso;
3. a stack BLE está respondendo.

Se qualquer parte travar, o watchdog estoura → reset → o boot lê a **causa do
reset** (registrador `RSTCAUSE`), incrementa um contador em NVM3 e o firmware
volta a operar — os registros já gravados permanecem intactos. O comando de
debug `hang` trava de propósito para testar esse caminho.

## 7. BLE — como o app conversa com a placa

A placa anuncia como **"SPL Logger"** e expõe um serviço GATT customizado
(detalhes em [protocolo_ble.md](protocolo_ble.md)):

- **Config** (read/write): bitmask de métricas + intervalo de coleta em s;
- **Time Sync** (write): epoch Unix do celular;
- **Data Transfer**: transferência dos registros por notificações, com ACK —
  o app confirma o que recebeu e a placa apaga só o que foi confirmado;
- **Status** (read/notify): uptime, nº de registros, resets por watchdog,
  tensão de bateria.

## 8. Energia — operação em bateria

Na CR2032, o consumo dominante é o conjunto mic + PDM + DSP contínuos e o
*advertising* BLE. O `power_manager` do SDK mantém o chip no modo de energia
mais fundo permitido (EM1 durante aquisição por DMA). A serial VCOM fica
desabilitada quando alimentado por bateria. Autonomia esperada: dias — ok para
os testes iniciais; *duty-cycle* do microfone é a otimização futura natural.

## 9. Build — de onde vem cada parte

- **`app.slcp`** declara *componentes* SLC (stack BLE, drivers, NVM3…);
- **`slc generate`** resolve as dependências e gera um projeto **CMake**
  apontando para o Simplicity SDK (fora do repo);
- **CMake + Ninja + ARM GCC** compilam; **Simplicity Commander** grava via
  J-Link on-board. Nada disso usa a IDE Simplicity Studio.
