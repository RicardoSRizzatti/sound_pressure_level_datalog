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
de 16 bits na taxa que pedirmos (usamos **48 kHz exatos**, DSR 50 → clock dos
mics em 2,4 MHz: essa combinação divide o clock de 38,4 MHz sem resto — o
prescaler do PDM é inteiro, e a combinação anterior "32 kHz" rodava na verdade
a 33,3 kHz. A 16 kHz o filtro de ponderação A sairia da tolerância da IEC
61672 por *warping* da bilinear — ver `tools/design_a_weighting.py`).

Usamos o driver pronto do SDK (`sl_mic.h`), que configura o periférico PDM e o
**LDMA** (DMA de baixa energia): as amostras vão direto para a RAM **sem a CPU
tocar em cada amostra**, em dois buffers alternados (*ping-pong*) — enquanto um
enche, o outro é processado. A CPU só acorda quando um bloco de 8 ms fica
pronto (a captura é mono — o mic esquerdo).

## 2. Ponderação A — por que filtrar?

O ouvido humano não percebe todas as frequências com a mesma sensibilidade:
graves e agudos extremos parecem mais "baixos" do que médios. A **curva de
ponderação A** (norma IEC 61672) simula isso, atenuando graves/agudos antes de
medir. Resultado em **dB(A)** — o padrão em normas de ruído ambiental e
ocupacional (NBR 10151, NR-15 etc.).

Implementação: uma cascata de **3 filtros biquad IIR** (6 polos no total),
com coeficientes projetados para fs = 48 kHz (gerados e validados pelo
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
α = 1 − exp(−T_bloco / 0,125)      (T_bloco = 8 ms → α ≈ 0,062)
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
centésimos de dB, int16), gravado em **lotes de 10 por objeto NVM3** — com o
anel cheio (2000 registros), objetos individuais tornavam a manutenção da
flash mais lenta que o watchdog. Na área de 128 KB cabem 2000 registros em
anel (o mais novo sobrescreve o mais antigo) mais um anel paralelo de ~600
espectros de 1/3 de oitava.

Como a placa não tem relógio de tempo real com bateria, os registros usam
`boot_id + uptime`; o app faz *time sync* ao conectar e converte para hora
absoluta no download.

## 6. Watchdog — o "vigia" contra travamentos

O **WDOG0** é um contador em hardware que reinicia o chip se não for
"alimentado" a tempo (~64 s durante o boot, quando a recuperação do NVM3
pode legitimamente demorar; ~8 s em operação, tolerando as pausas de
manutenção da flash). A alimentação acontece **no loop principal, e só se
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

## 8. Espectro de 1/3 de oitava — como um analisador de verdade

Além das métricas banda larga, o firmware calcula (métrica bit `0x04`) o
**LZeq por banda de terço de oitava** — 30 bandas de 20 Hz a 16 kHz, sem
ponderação de frequência, integradas ao longo do intervalo inteiro. A
implementação é a mesma de sonômetros/analisadores comerciais (IEC 61260):

- um **passa-banda Butterworth de ordem 6** (3 biquads CMSIS) por banda,
  rodando continuamente — nada de FFT nem buffers de áudio;
- **multitaxa**: as bandas graves rodam em taxas decimadas (÷4 por estágio,
  com anti-alias de ordem 10) — 5 estágios de 48 kHz a 187,5 Hz — o que
  reduz o custo de CPU e mantém os filtros numericamente saudáveis em
  float32;
- projeto e verificação automatizados em `tools/design_third_octave.py`
  (centro 0,00 dB, bordas −3,01 dB, rejeição ≥ 41 dB a uma oitava);
- o custo de CPU exigiu subir o núcleo para **76,8 MHz** (DPLL travado no
  HFXO — a configuração oficial de alta velocidade do xG22);
- validação na placa com tom sintético: banda de 1 kHz = 93,99 dB para um
  tom de 94,00 dB, vizinhas em −18 dB.

Os espectros vão para um anel NVM3 próprio, alinhado por `seq` aos
registros, e são sincronizados pela característica **Spectra** depois dos
Records (ver [protocolo_ble.md](protocolo_ble.md)).

## 8b. Métricas estendidas (conjunto B&K 2245)

Com a métrica `0x08` ligada, o firmware calcula por intervalo, além de
LAeq/LAFmax, o conjunto de um sonômetro classe 1 (referência: B&K 2245,
IEC 61672):

- **LAFmin / LASmax / LASmin**: mínimo Fast e extremos com o detector
  **Slow** (τ = 1 s), que roda em paralelo ao Fast;
- **LCpeak**: pico com **ponderação C** (2 biquads classe 2, projeto em
  `tools/design_c_weighting.py`) e detecção de pico verdadeiro (sem
  suavização) — captura impactos;
- **LAE** (nível de exposição sonora): `LAeq + 10·log10(T)`, normaliza a
  energia do intervalo para 1 s;
- **L10 / L50 / L90** (percentis estatísticos): nível excedido em 10%, 50%
  e 90% do tempo, calculados de um **histograma** do nível Fast (bins de
  0,5 dB) amostrado a cada bloco.

Vão para um anel NVM3 próprio (24 B/entrada), sincronizados pela
característica **Extended** depois dos Spectra.

## 8c. Feedback ao usuário

Toda vez que uma configuração é aceita (via BLE ou CLI), o LED0 da placa
**pisca 3 vezes** (não bloqueante, via sleeptimer) — confirmação visual
imediata de que a placa recebeu e aplicou os novos parâmetros. O app também
mostra um aviso na tela.

## 9. Energia — operação em bateria

Na CR2032, o consumo dominante é o conjunto mic + PDM + DSP contínuos e o
*advertising* BLE. O `power_manager` do SDK mantém o chip no modo de energia
mais fundo permitido (EM1 durante aquisição por DMA). A serial VCOM fica
desabilitada quando alimentado por bateria. Autonomia esperada: dias — ok para
os testes iniciais; *duty-cycle* do microfone é a otimização futura natural.

## 10. Build — de onde vem cada parte

- **`app.slcp`** declara *componentes* SLC (stack BLE, drivers, NVM3…);
- **`slc generate`** resolve as dependências e gera um projeto **CMake**
  apontando para o Simplicity SDK (fora do repo);
- **CMake + Ninja + ARM GCC** compilam; **Simplicity Commander** grava via
  J-Link on-board. Nada disso usa a IDE Simplicity Studio.
