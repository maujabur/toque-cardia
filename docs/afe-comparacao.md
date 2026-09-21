# Escolha do front-end de ECG (AFE): AD8232 vs ADS1293

Documento de decisão para o projeto Toque Cardia. Registra a comparação entre o
**AD8232** (usado na POC atual) e o **ADS1293**, e a recomendação para as
próximas etapas.

## Contexto: o que a POC já provou

Nos testes de bancada com o AD8232 (Waveshare ESP32-S3-Zero + módulo SparkFun
AD8232, eletrodos RA/LA/RL), com contato razoável e o notebook fora da tomada,
o sinal apareceu como uma **onda lenta e limpa**, sem o serrilhado de 60 Hz que
dominava os primeiros testes.

Conclusão importante: o problema inicial **não era o chip**, e sim o contato
(pontas de metal + dedo seco) e o acoplamento da rede elétrica. As manoplas de
academia encomendadas tendem a resolver a maior parte disso.

Isso reposiciona a pergunta: não é "o AD8232 é ruim?", mas "o que ganhamos ao
pagar a complexidade extra do ADS1293?".

## Diferença de categoria

Os dois chips resolvem problemas diferentes:

- **AD8232** — front-end analógico de **1 derivação**, com **saída analógica
  única**. A leitura é feita pelo ADC do próprio ESP32 (foi o que a POC fez).
  Barato, simples, 3 fios. Voltado a monitoração de frequência cardíaca /
  fitness de uma derivação.

- **ADS1293** — front-end **digital** de **3 canais / 24 bits**, com
  comunicação por **SPI**.

### Recursos do ADS1293

Segundo a Texas Instruments e a documentação da ProtoCentral (conteúdo
parafraseado por conformidade de licença):

- 3 canais de ECG simultâneos, cada um com ADC sigma-delta de 24 bits
- Taxa de amostragem programável (de ~10 Hz até dezenas de kHz)
- Right Leg Drive (RLD) integrado e Wilson Central Terminal (WCT), permitindo
  derivações precordiais / múltiplas derivações reais
- Detecção de lead-off AC e DC
- Ruído de entrada em torno de 7 µVpp em 40 Hz; consumo ~0,3 mW/canal
- Entradas endurecidas contra EMI e pino de alarme por interrupção

Fontes:
- [TI — ADS1293](https://www.ti.com/product/ADS1293)
- [ProtoCentral — ADS1293 Getting Started](https://protocentral.com/product/protocentral-ads1293-breakout-board/docs/getting-started/)

## Comparação lado a lado

| Critério | AD8232 (atual) | ADS1293 |
|---|---|---|
| Derivações | 1 | Até 3 simultâneas + precordiais |
| Conversão | ADC do ESP32 (12 bit) | 24 bit interno, dedicado por canal |
| Interface | Analógica (1 fio) | SPI (vários fios) |
| Qualidade do sinal | Boa para 1 derivação | Superior, menos ruído, EMI-hardened |
| Complexidade de firmware | Trivial (já implementada) | Alta: registradores, SPI, biblioteca |
| Custo / disponibilidade | Barato, onipresente | Mais caro, geralmente em breakout |
| Escopo desta POC | Já atende | Overkill agora |

## Recomendação

**Para a etapa atual: manter o AD8232.** Migrar agora adiciona complexidade
(SPI, mapa de registradores, biblioteca, 24 bits) sem ganho para o objetivo já
definido — validar a aquisição elétrica e visualizar a forma de onda. Os dados
de bancada mostram que essa validação já está feita.

**O ADS1293 passa a valer a pena quando o projeto exigir:**

- Múltiplas derivações reais (o ganho mais decisivo dele)
- Qualidade de sinal de grau clínico / menos ruído sem depender tanto do contato
- Amostragem determinística feita pelo próprio AFE (deixando de depender do ADC
  do ESP32 e do timing por `micros()`)
- Detecção de lead-off mais rica e diagnóstico por interrupção

Como as manoplas de academia apontam para contato por mãos (tipicamente 1
derivação, tipo Lead I), é provável que o AD8232 permaneça adequado por um bom
tempo. O ADS1293 só se justifica se o roadmap incluir ECG multi-derivação.

## Caminho sugerido

1. Concluir a POC com o AD8232 e testar com as manoplas quando chegarem.
2. Decidir com base no resultado:
   - Se uma derivação bem captada entrega o que se quer (visualizar/registrar o
     batimento e, mais tarde, BPM), permanecer no AD8232.
   - Se for necessário mais derivações ou qualidade que o contato por manopla
     não oferece, planejar a migração para o ADS1293 como uma etapa de peso
     próprio (hardware SPI + driver dedicado).

## Status

- Decisão atual: **permanecer no AD8232** para a POC e as próximas etapas de
  aquisição de uma derivação.
- Revisar esta decisão após os testes com manoplas ou se o escopo passar a
  exigir múltiplas derivações.
