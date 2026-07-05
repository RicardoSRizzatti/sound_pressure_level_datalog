# sound_pressure_level_datalog

Firmware de datalogger de nível de pressão sonora (NPS/SPL) para a
**Thunderboard EFR32BG22 (SLTB010A, placa BRD4184A rev. A02)**.

O dispositivo amostra os microfones PDM da placa, calcula métricas em dB(A)
com ponderação temporal **Fast** (τ = 125 ms) — **LAeq** e **LAFmax** por
intervalo (padrão 20 s, configurável) — grava os registros em flash interna
(NVM3) e os expõe por um serviço BLE customizado para o aplicativo
[spl_datalog_app](../spl_datalog_app). Um watchdog (WDOG0) reinicia o
firmware em caso de travamento, preservando os dados já gravados.

## Compilação

Toda a compilação é via **CMake + Ninja + ARM GCC**, com o projeto gerado
pelo **slc-cli** a partir do manifesto `app.slcp` (Simplicity SDK v2025.6.3,
sem IDE). Setup completo em [docs/setup.md](docs/setup.md).

```powershell
./tools/setup_env.ps1   # verifica o ambiente
./build.ps1 -Generate   # gera o projeto SLC + compila
./build.ps1 -Flash      # compila + grava na placa
```

## Estrutura

- `app.slcp` — manifesto SLC (componentes + placa)
- `src/` — código da aplicação (aquisição, DSP, datalog, BLE, watchdog)
- `config/` — configs SLC e GATT (`btconf/gatt_configuration.btconf`)
- `docs/` — setup, arquitetura e protocolo BLE
- `tools/` — scripts auxiliares

## Documentação

- [docs/setup.md](docs/setup.md) — montagem do ambiente, placa e testes de sanidade
- [docs/arquitetura.md](docs/arquitetura.md) — como o firmware funciona (explicação didática)
- [docs/protocolo_ble.md](docs/protocolo_ble.md) — contrato GATT com o app (fonte da verdade)
- [docs/desenvolvimento.md](docs/desenvolvimento.md) — estado do projeto, decisões técnicas e resultados de validação
