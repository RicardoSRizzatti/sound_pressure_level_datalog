# Protocolo BLE — SPL Datalogger Service

Contrato GATT entre o firmware e o app. **Este documento é a fonte da
verdade**: qualquer mudança aqui exige versão nova (característica Config,
campo `version`). Todos os inteiros são **little-endian**. Níveis em
**centi-dB(A)** (`6234` = 62,34 dB). `INT16_MIN` (`0x8000`) = métrica
desabilitada.

O dispositivo anuncia como **"SPL Logger"** com o UUID do serviço no
advertising. Conexão sem pairing para leitura; escrita de Config/Sync exige
bonding Just Works (definido na Fase 6).

## Serviço

| Item | UUID |
|---|---|
| SPL Datalogger Service | `b8f00001-9df1-4c39-9f2c-3e0d94a83f2b` |

## Características

### Config — `b8f00002-...` (read / write)

4 bytes:

| Offset | Tipo | Campo |
|---|---|---|
| 0 | u8 | `metrics` bitmask: bit0 = LAeq, bit1 = LAFmax |
| 1 | u16 | `interval_s` (5–3600) |
| 3 | i8 | reservado (0) |

Escrita inválida → erro ATT `Out of Range (0xFF)`. A configuração persiste em
NVM3 e vale imediatamente (o intervalo em andamento é reiniciado).

### Calibration — `b8f00003-...` (read / write)

2 bytes: `cal_offset_cdb` i16 — offset dBFS→dB SPL em centi-dB
(padrão `12300` = +123,00 dB).

### Time Sync — `b8f00004-...` (write)

8 bytes: `epoch_ms` u64 — hora Unix do celular em milissegundos. O firmware
calcula `epoch_boot = epoch_ms/1000 - uptime_s` e persiste
`(boot_id, epoch_boot)` num anel de 8 entradas em NVM3 — assim registros de
boots anteriores que receberam sync continuam datáveis.

### Boot Epochs — `b8f00005-...` (read)

Até 8 × 8 bytes: `{ boot_id u32, epoch_boot u32 }`. Usado pelo app para
converter `(boot_id, uptime_s)` de cada registro em hora absoluta:
`timestamp = epoch_boot + uptime_s`.

### Status — `b8f00006-...` (read / notify)

16 bytes:

| Offset | Tipo | Campo |
|---|---|---|
| 0 | u32 | `uptime_s` |
| 4 | u32 | `boot_id` |
| 8 | u32 | `record_count` |
| 12 | u16 | `wdog_resets` |
| 14 | u16 | `mic_overruns` (saturado em 65535) |

Notificado a cada registro novo quando inscrito.

### Sync Control — `b8f00007-...` (write / indicate)

Comandos do app (write):

| Byte 0 | Comando | Payload |
|---|---|---|
| `0x01` | START | `from_seq` u32 — inicia envio de registros ≥ from_seq |
| `0x02` | ACK | `through_seq` u32 — confirma recebimento; firmware **apaga** ≤ through_seq |
| `0x03` | STOP | — |

Respostas do firmware (indicate):

| Byte 0 | Evento | Payload |
|---|---|---|
| `0x81` | RANGE | `oldest_seq` u32, `next_seq` u32 (resposta ao START) |
| `0x82` | DONE | `last_sent_seq` u32 (fim do backlog) |
| `0x8F` | ERR | código u8 |

### Records — `b8f00008-...` (notify)

Fluxo de registros durante a sincronização. Cada notificação carrega
`N = len/16` registros consecutivos (N ≥ 1, limitado pelo MTU — negociar
MTU 247 permite 14 por notificação):

| Offset | Tipo | Campo |
|---|---|---|
| 0 | u32 | `boot_id` |
| 4 | u32 | `seq` |
| 8 | u32 | `uptime_s` |
| 12 | i16 | `laeq_cdb` |
| 14 | i16 | `lafmax_cdb` |

## Fluxo de sincronização do app

1. Conecta, negocia MTU, lê **Status** e **Boot Epochs**;
2. Escreve **Time Sync** (garante que o boot atual fique datável);
3. Escreve **Config** se o usuário mudou métricas/intervalo;
4. **Sync Control** ← START(`último_seq_conhecido + 1`);
5. Recebe **Records** por notificação, persiste em banco local
   (dedup por `boot_id+seq`), até **DONE**;
6. **Sync Control** ← ACK(`último_seq_recebido`) — o firmware libera a flash;
7. STOP / desconecta.

Se a conexão cair no meio, o app repete de (4) — nada se perde, pois o
firmware só apaga o que foi confirmado por ACK.
