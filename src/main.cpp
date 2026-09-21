/*
 * Toque Cardia - ECG POC
 *
 * Prova de conceito minima: le o sinal bruto do modulo AD8232 e envia
 * pela serial USB em formato CSV para visualizacao / plot.
 *
 * NAO faz: calculo de BPM, deteccao de batimentos, filtros, fita de LEDs
 * externa, protocolo serial definitivo, Wi-Fi, Bluetooth ou interface web.
 *
 * Usa apenas o LED RGB onboard (WS2812) como indicador de diagnostico do
 * estado de lead-off: verde = contato ok, vermelho = eletrodo solto.
 *
 * Placa: Waveshare ESP32-S3-Zero (ESP32-S3).
 * Framework: Arduino.
 */

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Pinagem
//
// Modulo: SparkFun AD8232 Heart Monitor (ou clone). O bloco de parafusos traz
// a serigrafia: GND, 3.3v, OUTPUT, LO-, LO+, SDN.
//
// AD8232 (serigrafia)  ESP32-S3-Zero
// OUTPUT             -> GPIO1
// LO-  (serig. "D-") -> GPIO2   <- lead-off minus (confirmado: pad "O-")
// LO+  (serig. "D+") -> GPIO3   <- lead-off plus  (confirmado: pad "O+")
// 3.3v               -> 3V3
// GND                -> GND
// SDN                -> deixado com pull-up (~3V3) = chip LIGADO
//
// Observacao: os nomes fisicos D_MINUS / D_PLUS foram mantidos por
// compatibilidade com a montagem. Confirmado que correspondem aos sinais de
// lead-off LO- / LO+ do AD8232 (deteccao de eletrodo solto).
//
// Eletrodos (cabo snap): RA=vermelho, LA=verde, RL=amarelo.
// O AD8232 mede RA-LA usando RL como referencia (driven right leg): os tres
// precisam ter contato para o sinal fazer sentido.
// ---------------------------------------------------------------------------
const int ECG_PIN     = 1;  // GPIO1  - OUTPUT analogico do AD8232
const int D_MINUS_PIN = 2;  // GPIO2  - LO- (lead-off minus)
const int D_PLUS_PIN  = 3;  // GPIO3  - LO+ (lead-off plus)

// LED RGB onboard da Waveshare ESP32-S3-Zero: WS2812 no GPIO21.
// Usado como indicador de diagnostico do lead-off (verde=ok, vermelho=solto).
const int RGB_LED_PIN = 21;
const uint8_t LED_BRIGHTNESS = 24;  // 0..255; baixo para nao ofuscar na bancada

// Taxa de amostragem alvo: 250 Hz -> intervalo de 4000 us entre amostras.
const uint32_t SAMPLE_RATE_HZ   = 250;
const uint32_t SAMPLE_PERIOD_US = 1000000UL / SAMPLE_RATE_HZ;  // 4000 us

// Diagnostico: inclui (1) ou nao (0) o timestamp em us na saida CSV.
// O cabecalho se ajusta automaticamente para bater com as colunas emitidas.
// Util para plotar no Serial Plotter (tempo atrapalha) x registrar/analisar.
#define INCLUDE_TIME_US 1

// Marca o instante (micros) em que a proxima amostra deve ser coletada.
uint32_t nextSampleUs = 0;

// Ultimo estado de lead-off refletido no LED. Comeca em -1 (indefinido) para
// forcar a primeira atualizacao. Assim so escrevemos no WS2812 quando o estado
// muda, evitando reprogramar o LED a 250 Hz sem necessidade.
int lastLeadOffShown = -1;

// Atualiza o LED onboard conforme o estado de lead-off (0=ok, 1=solto).
void updateLeadOffLed(int leadOff) {
  if (leadOff == lastLeadOffShown) {
    return;  // nada mudou; nao reprograma o LED
  }
  lastLeadOffShown = leadOff;

  // Nesta placa o WS2812 usa ordem de cor GRB: os dois primeiros argumentos de
  // rgbLedWrite() saem trocados (R<->G) na pratica. Por isso passamos o valor
  // no primeiro argumento para o VERDE e no segundo para o VERMELHO, de modo
  // que a cor exibida corresponda a intencao.
  if (leadOff) {
    rgbLedWrite(RGB_LED_PIN, 0, LED_BRIGHTNESS, 0);  // vermelho = eletrodo solto
  } else {
    rgbLedWrite(RGB_LED_PIN, LED_BRIGHTNESS, 0, 0);  // verde = contato ok
  }
}

void setup() {
  Serial.begin(115200);

  // Nao bloquear esperando o terminal: damos apenas uma pequena folga para
  // o USB-CDC enumerar, mas o dispositivo comeca a funcionar de qualquer jeito.
  delay(200);

  analogReadResolution(12);          // ADC de 12 bits: valores 0..4095
  pinMode(ECG_PIN, INPUT);
  pinMode(D_MINUS_PIN, INPUT);
  pinMode(D_PLUS_PIN, INPUT);

  // Estado inicial do LED: assume lead-off (sem contato) ao ligar.
  updateLeadOffLed(1);

  // Identificacao + cabecalho CSV, impressos uma unica vez no boot.
  Serial.println("Toque Cardia ECG POC");
  Serial.print("sample_rate=");
  Serial.println(SAMPLE_RATE_HZ);
  // Legenda dos eletrodos e do lead-off, como comentario (linhas com '#'):
  Serial.println("# eletrodos: RA=vermelho LA=verde RL=amarelo");
  Serial.println("# lo_minus/lo_plus: 0=contato ok, 1=eletrodo solto");
  Serial.println("# lead_off: 0=todos em contato, 1=algum eletrodo solto");

  // Cabecalho CSV, montado conforme a flag INCLUDE_TIME_US.
#if INCLUDE_TIME_US
  Serial.println("time_us,adc,lo_minus,lo_plus,lead_off");
#else
  Serial.println("adc,lo_minus,lo_plus,lead_off");
#endif

  nextSampleUs = micros();
}

void loop() {
  // Temporizacao baseada em micros() para evitar deriva do delay(4).
  uint32_t now = micros();
  if ((int32_t)(now - nextSampleUs) < 0) {
    return;  // ainda nao chegou o momento da proxima amostra
  }
  nextSampleUs += SAMPLE_PERIOD_US;

  uint32_t timeUs = micros();
  int adc     = analogRead(ECG_PIN);
  int loMinus = digitalRead(D_MINUS_PIN);
  int loPlus  = digitalRead(D_PLUS_PIN);

  // Resumo de lead-off: 1 se qualquer um dos dois indicar eletrodo solto.
  // Facilita ver "contato ok x sem contato" numa unica coluna no grafico.
  int leadOff = (loMinus || loPlus) ? 1 : 0;

  // Indicador visual de diagnostico: verde=contato ok, vermelho=eletrodo solto.
  updateLeadOffLed(leadOff);

  // Linha CSV.
#if INCLUDE_TIME_US
  Serial.print(timeUs);
  Serial.print(',');
#endif
  Serial.print(adc);
  Serial.print(',');
  Serial.print(loMinus);
  Serial.print(',');
  Serial.print(loPlus);
  Serial.print(',');
  Serial.println(leadOff);
}
