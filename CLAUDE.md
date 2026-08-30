# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Estilo de resposta

Conforme `instrucoes_projeto.md`: seja muito objetivo, direto, sem textos longos ou redundantes. Responda apenas o necessário para executar a tarefa.

## O que é

Firmware Arduino (C++) para ESP8266, que roda um display de status na moto: relógio (RTC DS1302), clima (OpenWeatherMap), trânsito até o destino (TomTom Routing API) e recebe GPS de um celular via HTTP. Sem toolchain no repo (sem `platformio.ini`, sem config de `arduino-cli`) — é só o `.ino`.

## Estrutura

- `MotoSync/MotoSync.ino` — firmware principal (único arquivo com toda a lógica).
- `i2c_scanner/i2c_scanner.ino` — sketch avulso de diagnóstico para descobrir endereços I2C dos periféricos (LCD, RTC) no barramento.

## Build / compilar

Não há projeto PlatformIO configurado. Para compilar via `arduino-cli`:

```bash
arduino-cli core install esp8266:esp8266 --additional-urls https://arduino.esp8266.com/stable/package_esp8266com_index.json
arduino-cli lib install "ArduinoJson" "LiquidCrystal I2C" "Rtc by Makuna"
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 MotoSync/MotoSync.ino
```

Upload: `arduino-cli upload -p <porta> --fqbn esp8266:esp8266:nodemcuv2 MotoSync/MotoSync.ino`

Não há testes automatizados (é firmware embarcado, validação é em hardware real).

**Gotcha de libs conflitantes:** se `~/Arduino/libraries` tiver mais de uma lib "LiquidCrystal_I2C" instalada (comum se outros projetos Arduino já rodaram nessa máquina), o arduino-cli pode escolher a errada e falhar em `lcd.init()` com "has no member named 'init'". A lib correta para este sketch é `LiquidCrystal I2C` (Marco Schwartz/Frank de Brabander, a que expõe `.init()`), não a fork `Arduino-LiquidCrystal-I2C-library-master`. Compilado e testado com sucesso (RAM 38%, IRAM 94%, flash 30%) usando `esp8266:esp8266:nodemcuv2` com apenas essa lib no path.

## Arquitetura (`clovk_moto.ino`)

Loop principal não-bloqueante (`server.handleClient()` + `MDNS.update()` a cada iteração, sem `delay()` no `loop()`), com temporização por `millis()`:

- **Display LCD 16x2** (I2C 0x27): alterna a cada ~11s entre Tela 0 (relógio + data + trânsito, 8s) e Tela 1 (clima, 3s).
- **RTC DS1302** via `ThreeWire` (pinos GPIO 12/14/13): sincronizado uma vez via NTP no `setup()`, depois mantém a hora sozinho.
- **Servidor HTTP** (porta 80, `/gps`): recebe `lat`/`lon` (e opcionalmente `destLat`/`destLon`/`nome`) de um app no celular. É o único endpoint de entrada de dados externos.
- **Roteamento automático Casa↔Trabalho**: `handleGPSUpdate()` compara a posição recebida com `HOME_LAT/LON` e `WORK_LAT/LON` (raio de proximidade `PROXIMITY_RADIUS`) para decidir o destino automaticamente, a menos que `destLat`/`destLon` seja enviado explicitamente.
- **Clima** (`atualizarClima()`): consulta OpenWeatherMap a cada 10 min quando há Wi-Fi.
- **Trânsito** (`atualizarTransito()`): consulta TomTom Routing API a cada 3 min quando há GPS + Wi-Fi, e também é forçada a cada novo `/gps` recebido. Classifica `statusTransito` em Livre/Moderado/Intenso pelo `trafficDelayInSeconds`.
- **Reconexão Wi-Fi**: se cair, tenta reconectar a cada 5s sem travar o `loop()`.

### Pontos de atenção

- Credenciais e API keys (Wi-Fi, OpenWeatherMap, TomTom) estão hardcoded no topo do arquivo como placeholders (`SUA_API_KEY_...`, `SUA_SENHA_WIFI`) — preencher localmente antes de compilar, nunca commitar valores reais.
- `HOME_LAT`/`HOME_LON` estão zerados (placeholder) — precisam ser configurados com as coordenadas reais de casa.
- Strings do LCD são formatadas com `snprintf` em buffers fixos (16 colunas) — qualquer alteração de layout precisa respeitar esse limite de caracteres.
