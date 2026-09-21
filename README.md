# Toque Cardia — ECG POC

Prova de conceito mínima para **ler o sinal bruto de um módulo AD8232** com um
**Waveshare ESP32-S3-Zero** e observar a forma de onda pela serial USB.

O objetivo desta etapa é apenas **validar a aquisição elétrica** e enxergar o
sinal bruto. Ainda **não** há cálculo de BPM, detecção de batimentos, filtros,
fita de LEDs, protocolo serial definitivo, Wi-Fi, Bluetooth ou interface web.

## Hardware

- Waveshare ESP32-S3-Zero (ESP32-S3), com LED RGB onboard (WS2812 no GPIO21)
- Módulo AD8232 (SparkFun Heart Monitor ou clone)
- Cabo de eletrodos com snaps: **RA=vermelho, LA=verde, RL=amarelo**
  (por ora, pontas de metal seguradas com os dedos; manoplas melhoram o contato)

## Pinagem

O bloco de parafusos do módulo tem a serigrafia (de cima para baixo):
`GND, 3.3v, OUTPUT, LO-, LO+, SDN`.

| AD8232 (serigrafia) | ESP32-S3-Zero | Papel no código | Função |
|---------------------|---------------|-----------------|--------|
| OUTPUT              | GPIO1         | `ECG_PIN`       | Saída analógica do ECG |
| LO-                 | GPIO2         | `D_MINUS_PIN`   | Lead-off minus |
| LO+                 | GPIO3         | `D_PLUS_PIN`    | Lead-off plus |
| 3.3v                | 3V3           | —               | Alimentação |
| GND                 | GND           | —               | Terra |
| SDN                 | —             | —               | Shutdown (pull-up ~3V3 = chip ligado) |

> O código mantém os nomes físicos `D_MINUS` / `D_PLUS` por compatibilidade com
> a montagem. Confirmado (pela serigrafia dos pads **O-** / **O+** e pelo
> comportamento) que correspondem aos sinais de lead-off **LO-** / **LO+** do
> AD8232, que sinalizam eletrodo solto.

### Sobre o AD8232 e os três eletrodos

O AD8232 mede a diferença **RA − LA** usando **RL** como referência (driven
right leg). Os três eletrodos precisam ter contato ao mesmo tempo para o sinal
fazer sentido: sem contato, a saída satura (ADC em 4095) e os pinos de lead-off
oscilam.

## Board definition

Usamos a board oficial **`waveshare_esp32_s3_zero`**, fornecida pelo platform
[pioarduino](https://github.com/pioarduino/platform-espressif32) (fork do
`espressif32` com suporte às placas ESP32-S3 mais recentes). A definição existe
no platform e não foi inventada.

A placa expõe a serial pela **porta USB-C nativa** (USB Serial/JTAG). Por isso o
`platformio.ini` habilita `-DARDUINO_USB_CDC_ON_BOOT=1`, necessário para que o
objeto `Serial` do Arduino escreva nessa porta.

## Como compilar

```
pio run
```

## Como fazer upload

Conecte a placa pela USB-C e rode:

```
pio run --target upload
```

Se o upload não iniciar, force o modo bootloader: segure **BOOT**, toque em
**RESET**, solte **BOOT** e rode o comando novamente.

## Como abrir o serial monitor

```
pio device monitor --baud 115200
```

(ou simplesmente `pio device monitor`, pois o baud já está fixado em
`monitor_speed = 115200` no `platformio.ini`.)

O dispositivo **não** espera o terminal conectar: ele começa a amostrar assim
que liga, mesmo sem monitor aberto.

## Formato da saída

No boot é impressa uma identificação, uma legenda (linhas começando com `#`) e
o cabeçalho CSV:

```
Toque Cardia ECG POC
sample_rate=250
# eletrodos: RA=vermelho LA=verde RL=amarelo
# lo_minus/lo_plus: 0=contato ok, 1=eletrodo solto
# lead_off: 0=todos em contato, 1=algum eletrodo solto
time_us,adc,lo_minus,lo_plus,lead_off
```

Em seguida, a 250 Hz (uma amostra a cada 4 ms), são enviadas linhas CSV:

```
time_us,adc,lo_minus,lo_plus,lead_off
1000000,1980,0,0,0
1004000,2012,0,0,0
1008000,2075,0,0,0
```

### Significado das colunas

| Coluna     | Significado |
|------------|-------------|
| `time_us`  | Timestamp em microssegundos (`micros()`) no momento da amostra |
| `adc`      | Leitura bruta do ADC de 12 bits em GPIO1 (0–4095) |
| `lo_minus` | Estado de LO- em GPIO2 (0=contato ok, 1=eletrodo solto) |
| `lo_plus`  | Estado de LO+ em GPIO3 (0=contato ok, 1=eletrodo solto) |
| `lead_off` | Resumo: 1 se `lo_minus` ou `lo_plus` indicarem eletrodo solto |

Ao tocar/remover os eletrodos, espera-se observar mudanças em
`lo_minus` / `lo_plus` / `lead_off`, além da variação de `adc` acompanhando o
sinal. Com os três eletrodos em bom contato, o esperado é `lead_off=0` e o
`adc` oscilando em torno de um nível médio.

## Indicador de lead-off no LED onboard

A Waveshare ESP32-S3-Zero tem um LED RGB (WS2812) no **GPIO21**. Ele é usado
como indicador de diagnóstico do estado de contato:

- **Verde** = contato ok (`lead_off=0`)
- **Vermelho** = eletrodo solto (`lead_off=1`)

O LED reflete o estado atual, sem piscar, e só é reprogramado quando o estado
muda (evita reescrever o WS2812 a 250 Hz). No boot ele começa em vermelho, já
que sem contato o lead-off é o esperado. O brilho é baixo por padrão
(`LED_BRIGHTNESS` em `src/main.cpp`) para não ofuscar na bancada.

Isso usa apenas o `rgbLedWrite()` do core Arduino-ESP32, sem biblioteca externa.
É o LED onboard como ferramenta de diagnóstico — não confundir com a "fita de
LEDs" externa, que continua fora do escopo desta etapa.

## Flag de diagnóstico

No topo de `src/main.cpp`:

```c
#define INCLUDE_TIME_US 1
```

- `1`: inclui a coluna `time_us` (bom para registrar e analisar depois).
- `0`: omite o `time_us` (bom para o **Serial Plotter**, onde o tempo atrapalha
  a visualização gráfica).

O cabeçalho CSV se ajusta automaticamente para bater com as colunas emitidas.

## Estado do diagnóstico

- Aquisição elétrica validada: com os três eletrodos, a saída sai da saturação,
  `lead_off` vai a `0` e o `adc` oscila em torno de um nível médio.
- Com pontas de metal e dedo seco, o sinal ainda capta bastante ruído de rede
  (60 Hz). Contato melhor (manoplas / gel) reduz isso. Filtragem fica para uma
  etapa futura, fora do escopo desta POC.
