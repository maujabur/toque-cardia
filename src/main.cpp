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
#include <Adafruit_NeoPixel.h>

// ===========================================================================
// CONFIGURACAO DOS EFEITOS VISUAIS DA FITA (ajuste aqui)
//
// Todos os parametros de aparencia/animacao da fita ficam centralizados neste
// bloco para facilitar o ajuste sem cacar pelo codigo.
// ===========================================================================

// --- Geometria da fita -----------------------------------------------------
#define STRIP_LED_PIN       13   // GPIO de dados da fita WS2812
#define STRIP_TOTAL_LEDS    250  // comprimento fisico total da fita (todos os LEDs)
#define STRIP_OFFSET        10    // LEDs iniciais NAO utilizaveis (pulados no desenho)
#define STRIP_USABLE_LEDS   45   // LEDs utilizaveis, contados a partir do offset
#define STRIP_BRIGHTNESS    40   // 0..255; brilho global da fita

// --- Heartbeat (estado REPOUSO, lead-off) -----------------------------------
#define HEARTBEAT_PERIODO_MS 2000  // duracao de um ciclo do pulso (lento)
#define HEARTBEAT_MAX_PCT    50    // pico da rampa: % do valor de pixel (0..100)

// --- Aquisicao (barra de progresso, lead-on inicial) ------------------------
#define AQUISICAO_MS         6000 // duracao do periodo de aquisicao
#define BARRA_G_INICIAL      60    // verde inicial da barra (laranja); vai a 0 (vermelho)

// --- Beat (scan proporcional ao BPM) ----------------------------------------
#define BPM_SIMULADO         72    // BPM fake para animar o beat (sem medicao real)
#define BEAT_FADE            3     // escurecimento do trail por ms; menor = rastro mais longo

// --- Parametros de simulação de BPM
#define BPM_MIN_VALID        60
#define BPM_MAX_VALID        100

#define BPM_DELTA_1          5
#define BPM_DELTA_2          20
#define TIME_LEAD_OFF_DELTA  10000

// ===========================================================================


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

// ---------------------------------------------------------------------------
// Fita de LED WS2812 externa (parametros no bloco de config no topo do arquivo)
//
// Ligada ao STRIP_LED_PIN (dado). Alimentar por 5V + GND comuns com o ESP32.
//
// Mapa de enderecamento da fita:
//   [0 .. STRIP_OFFSET-1]                          -> pulados (nunca desenhados)
//   [STRIP_OFFSET .. STRIP_OFFSET+USABLE-1]        -> area utilizavel (o desenho)
//   [resto ate STRIP_TOTAL_LEDS-1]                 -> sobra fisica (apagada)
//
// O STRIP_OFFSET permite pular alguns LEDs no inicio da fita que nao fazem
// parte do desenho principal (ex.: sobra antes do formato de coracao). Ajuste
// em tempo de compilacao conforme a instalacao fisica.
//
// Declaramos o buffer com o total fisico para conseguir apagar TODOS os LEDs;
// caso contrario, os LEDs fora da area util acendem com lixo ao ligar.
//
// As funcoes de desenho usam indices LOGICOS (0..STRIP_USABLE_LEDS-1) e o
// helper pix() converte para o indice FISICO somando o offset.
// ---------------------------------------------------------------------------

// Controlador da fita, dimensionado pelo total fisico. Ordem de cor BRG
// (confirmada no teste de boot). WS2812/WS2812B rodam a 800 kHz.
Adafruit_NeoPixel strip(STRIP_TOTAL_LEDS, STRIP_LED_PIN, NEO_BRG + NEO_KHZ800);

// Converte um indice logico da area util (0..STRIP_USABLE_LEDS-1) no indice
// fisico correspondente na fita, aplicando o offset de instalacao.
inline uint16_t pix(uint16_t logico) {
  return STRIP_OFFSET + logico;
}

// Apaga a fita inteira (todos os LEDs fisicos, nao so os utilizaveis) e envia.
// Usado no boot para eliminar as cores aleatorias dos LEDs fora da area util.
void clearWholeStrip() {
  strip.clear();  // limpa o buffer inteiro (STRIP_TOTAL_LEDS)
  strip.show();
}

// ---------------------------------------------------------------------------
// Heartbeat de lead-off na fita
//
// Enquanto estamos em lead-off (eletrodo solto), a fita pulsa em vermelho:
// todos os LEDs utilizaveis juntos, com rampa suave (fade in/out) de 0 ate
// 50% do brilho global, lento. Quando NAO estamos em lead-off, a fita fica
// apagada (por enquanto).
//
// Nao usa delay(): a fase da rampa e derivada de millis() e a fita e
// atualizada a cada chamada. Chamado a cada iteracao do loop().
// ---------------------------------------------------------------------------
// Desenha um quadro do heartbeat vermelho (pulso de repouso) na fita.
// A rampa vai de 0 a HEARTBEAT_MAX_PCT do valor de pixel, derivada de millis().
void stripHeartbeat() {
  // Rampa triangular 0 -> pico -> 0 ao longo do periodo.
  uint32_t fase = millis() % HEARTBEAT_PERIODO_MS;
  uint32_t meio = HEARTBEAT_PERIODO_MS / 2;
  // 0..255 subindo na primeira metade, descendo na segunda.
  uint32_t tri = (fase < meio)
                   ? (fase * 255 / meio)
                   : (255 - (fase - meio) * 255 / meio);

  // Teto da rampa = 50% do valor de pixel. O brilho global ja e aplicado pelo
  // setBrightness(STRIP_BRIGHTNESS); aqui a rampa vai de 0 ate 50% de 255, de
  // modo que o pico corresponda a metade do brilho configurado.
  uint8_t pico = (uint8_t)(255 * HEARTBEAT_MAX_PCT / 100);  // 127
  uint8_t nivel = (uint8_t)(tri * pico / 255);

  // Preenche apenas a area utilizavel, deslocada pelo offset.
  strip.fill(strip.Color(nivel, 0, 0), pix(0), STRIP_USABLE_LEDS);
  strip.show();
}

// ---------------------------------------------------------------------------
// Maquina de estados visual da fita
//
// Guiada pela deteccao de lead (estado ja filtrado pelo debounce):
//   REPOUSO    -> lead-off: heartbeat vermelho (pulso de repouso).
//   AQUISICAO  -> lead-on: barra de progresso laranja enchendo por AQUISICAO_MS.
//   BEAT       -> apos a aquisicao: scan laranja com periodo proporcional ao BPM.
//
// Qualquer lead-off retorna ao REPOUSO e zera o relogio da aquisicao.
// O BPM e SIMULADO por enquanto (BPM_SIMULADO), para avaliar o efeito visual.
// Parametros no bloco de config no topo do arquivo.
// ---------------------------------------------------------------------------

// Estados possiveis da fita.
enum EstadoFita { REPOUSO, AQUISICAO, BEAT };

// Estado atual e instante em que o lead-on comecou (base do relogio da aquisicao).
EstadoFita estadoFita = REPOUSO;
uint32_t   leadOnInicioMs = 0;

// ---------------------------------------------------------------------------
// Simulacao de variacao de BPM (para visualizar mudanca de ritmo sem medir)
//
// A cada novo ciclo de medicao (quando a aquisicao termina e o beat comeca),
// sorteamos um novo BPM a partir do anterior. A amplitude do sorteio depende
// de quanto tempo ficamos em lead-off desde o ultimo ciclo:
//   - lead-off curto  (< TIME_LEAD_OFF_DELTA): variacao pequena, +/- BPM_DELTA_1
//   - lead-off longo  (>= TIME_LEAD_OFF_DELTA): variacao maior, +/- BPM_DELTA_2
// O resultado e sempre limitado ao intervalo [BPM_MIN_VALID, BPM_MAX_VALID].
// ---------------------------------------------------------------------------
uint16_t bpmAtual = BPM_SIMULADO;   // BPM em uso no beat atual

// Instante em que o lead-off (repouso) comecou. -1 (indefinido) ate o primeiro.
// Usado para medir a duracao do repouso entre dois ciclos de medicao.
int32_t leadOffInicioMs = -1;

// Duracao do ultimo lead-off, CONGELADA no instante do lead-on (fim do
// repouso). Assim a medicao reflete so o tempo realmente solto, sem somar os
// 10s da barra de aquisicao. -1 = ainda nao houve um repouso medido.
int32_t ultimaDuracaoLeadOffMs = -1;

// Sorteia e aplica um novo BPM com base na duracao do ultimo lead-off (ms).
void sortearNovoBpm(uint32_t duracaoLeadOffMs) {
  int delta = (duracaoLeadOffMs < TIME_LEAD_OFF_DELTA) ? BPM_DELTA_1 : BPM_DELTA_2;

  uint16_t bpmAnterior = bpmAtual;
  bool leadOffLongo = (duracaoLeadOffMs >= TIME_LEAD_OFF_DELTA);
  bool naBorda = (bpmAnterior == BPM_MIN_VALID) || (bpmAnterior == BPM_MAX_VALID);

  int novo;
  int variacao;  // apenas para log; nao usado no sorteio livre

  if (naBorda && leadOffLongo) {
    // Preso na borda + repouso longo: sorteia um valor LIVRE em todo o
    // intervalo valido, ignorando o anterior. Assim "descola" da borda.
    novo = (int)random(BPM_MIN_VALID, BPM_MAX_VALID + 1);
    variacao = novo - (int)bpmAnterior;
  } else {
    // Caso normal: variacao uniforme em [-delta, +delta]. Sorteamos em
    // [0, 2*delta] e subtraimos delta para NUNCA passar argumento negativo ao
    // random() (evita vies do random() com sinal em algumas versoes do core).
    variacao = (int)random(0, 2 * delta + 1) - delta;
    novo = (int)bpmAtual + variacao;

    // Limita ao intervalo valido.
    if (novo < BPM_MIN_VALID) novo = BPM_MIN_VALID;
    if (novo > BPM_MAX_VALID) novo = BPM_MAX_VALID;
  }

  bpmAtual = (uint16_t)novo;

  // Reporta o BPM sorteado na serial (unico print ativo por ciclo).
  Serial.print("bpm=");
  Serial.print(bpmAtual);
  Serial.print(" (anterior=");
  Serial.print(bpmAnterior);
  Serial.print(", variacao=");
  Serial.print(variacao);
  Serial.print(", lead_off=");
  Serial.print(duracaoLeadOffMs);
  Serial.print("ms, delta=+/-");
  Serial.print(delta);
  if (naBorda && leadOffLongo) {
    Serial.print(", sorteio_livre");
  }
  Serial.println(")");
}

// Barra de progresso: acende os primeiros N LEDs proporcionalmente ao
// progresso 0..1 da aquisicao. A cor da barra migra de laranja (inicio) para
// vermelho puro (final): o verde vai de BARRA_G_INICIAL ate 0 conforme enche.
// Comunica "conectado, medindo" e "esquentando" ate virar vermelho.
void stripBarraProgresso(float progresso) {
  if (progresso < 0) progresso = 0;
  if (progresso > 1) progresso = 1;

  uint16_t acesos = (uint16_t)(progresso * STRIP_USABLE_LEDS + 0.5f);

  // Verde diminui com o progresso: laranja -> vermelho. R fica em 255.
  uint8_t g = (uint8_t)(BARRA_G_INICIAL * (1.0f - progresso));
  uint32_t cor = strip.Color(255, g, 0);

  for (uint16_t i = 0; i < STRIP_USABLE_LEDS; i++) {
    strip.setPixelColor(pix(i), (i < acesos) ? cor : 0);
  }
  strip.show();
}

// Scan vermelho com trail em fade, com DUAS cabecas simultaneas.
//
// Conceito: no periodo do batimento (60000/BPM, ~833 ms a 72 BPM) uma cabeca
// percorre METADE da fita utilizavel (30 LEDs). Logo, o percurso completo
// (0 -> fim) leva DOIS periodos. A cada periodo surge uma nova cabeca no
// inicio; como a anterior ainda esta na segunda metade, ficam duas cabecas /
// trails visiveis ao mesmo tempo, defasadas de meia fita. Cada uma segue ate
// o final. Nao-bloqueante: um quadro por chamada, posicoes derivadas de millis.
uint32_t beatFadeAplicadoMs = 0;  // ultimo instante em que aplicamos o fade

void stripBeatScan(uint16_t bpm) {
  const uint8_t FADE = BEAT_FADE;  // menor = rastro mais longo (config no topo)
  if (bpm < 20) bpm = 20;   // evita periodos absurdos / divisao por zero

  uint32_t periodoMs = 60000UL / bpm;   // periodo do batimento (~833 ms a 72)
  uint16_t metade = STRIP_USABLE_LEDS / 2;  // 30 LEDs = distancia por periodo

  // Fase 0..periodoMs -> avanco 0..metade dentro do periodo atual.
  uint32_t fase = millis() % periodoMs;
  int avanco = (int)((uint32_t)fase * metade / periodoMs);

  // Cabeca "jovem" na primeira metade; cabeca "velha" meia fita a frente.
  int cabecaA = avanco;             // 0 .. metade-1
  int cabecaB = avanco + metade;    // metade .. fim
  if (cabecaA >= STRIP_USABLE_LEDS) cabecaA = STRIP_USABLE_LEDS - 1;
  if (cabecaB >= STRIP_USABLE_LEDS) cabecaB = STRIP_USABLE_LEDS - 1;

  // Escurece a fita (trail). Limitamos o fade a ~1x por ms para o comprimento
  // do rastro nao depender da taxa do loop().
  uint32_t agora = millis();
  if (agora != beatFadeAplicadoMs) {
    beatFadeAplicadoMs = agora;
    for (uint16_t i = 0; i < STRIP_USABLE_LEDS; i++) {
      uint32_t cor = strip.getPixelColor(pix(i));
      uint8_t r = (uint8_t)(cor >> 16);
      uint8_t g = (uint8_t)(cor >> 8);
      uint8_t b = (uint8_t)cor;
      r = (r > FADE) ? (r - FADE) : 0;
      g = (g > FADE) ? (g - FADE) : 0;
      b = (b > FADE) ? (b - FADE) : 0;
      strip.setPixelColor(pix(i), r, g, b);
    }
  }

  // Acende as duas cabecas em vermelho puro.
  strip.setPixelColor(pix(cabecaA), strip.Color(255, 0, 0));
  strip.setPixelColor(pix(cabecaB), strip.Color(255, 0, 0));
  strip.show();
}

// Controlador da fita: decide o estado a partir do lead-off filtrado e desenha
// o quadro correspondente. Chamado a cada iteracao do loop().
void updateStrip(int leadOff) {
  if (leadOff) {
    // Lead-off detectado: volta ao repouso e zera o relogio da aquisicao.
    // Marca o inicio do repouso (so na transicao para REPOUSO) para medir sua
    // duracao no proximo ciclo de medicao.
    if (estadoFita != REPOUSO) {
      leadOffInicioMs = (int32_t)millis();
    }
    estadoFita = REPOUSO;
    stripHeartbeat();
    return;
  }

  // Lead-on. Se acabamos de entrar em lead-on, marca o inicio da aquisicao e
  // CONGELA a duracao do lead-off que acabou de terminar (tempo realmente
  // solto, sem contar a barra de aquisicao que vem a seguir).
  if (estadoFita == REPOUSO) {
    estadoFita = AQUISICAO;
    leadOnInicioMs = millis();
    ultimaDuracaoLeadOffMs = (leadOffInicioMs < 0)
                               ? -1  // ainda nao houve repouso medido (1o ciclo)
                               : (int32_t)(millis() - (uint32_t)leadOffInicioMs);
  }

  uint32_t decorrido = millis() - leadOnInicioMs;

  if (estadoFita == AQUISICAO) {
    if (decorrido >= AQUISICAO_MS) {
      // Aquisicao concluida -> novo ciclo de medicao. Sorteia novo BPM com
      // base na duracao do lead-off congelada no lead-on. No 1o ciclo (sem
      // repouso medido), trata como longo (delta maior).
      uint32_t duracaoLeadOff = (ultimaDuracaoLeadOffMs < 0)
                                  ? TIME_LEAD_OFF_DELTA
                                  : (uint32_t)ultimaDuracaoLeadOffMs;
      sortearNovoBpm(duracaoLeadOff);
      estadoFita = BEAT;
    } else {
      float progresso = (float)decorrido / (float)AQUISICAO_MS;
      stripBarraProgresso(progresso);
      return;
    }
  }

  // Estado BEAT: scan proporcional ao BPM atual (simulado).
  stripBeatScan(bpmAtual);
}

// Teste rapido da fita no boot: pisca vermelho, verde, azul em toda a fita
// e depois faz um "sweep" acendendo LED a LED. Serve para confirmar a
// ligacao, o pino e a contagem de LEDs.
void testStrip() {
  const uint32_t cores[] = {
    strip.Color(255, 0, 0),  // vermelho
    strip.Color(0, 255, 0),  // verde
    strip.Color(0, 0, 255),  // azul
  };
  for (uint32_t cor : cores) {
    // Preenche apenas a area utilizavel: fill(cor, primeiro, quantidade).
    strip.fill(cor, pix(0), STRIP_USABLE_LEDS);
    strip.show();
    delay(300);
  }

  // Sweep: acende um LED por vez do inicio ao fim da area utilizavel.
  clearWholeStrip();
  for (uint16_t i = 0; i < STRIP_USABLE_LEDS; i++) {
    strip.setPixelColor(pix(i), strip.Color(255, 255, 255));
    strip.show();
    delay(40);
    strip.setPixelColor(pix(i), 0);
  }

  // Apaga a fita ao terminar o teste.
  clearWholeStrip();
}

// Anima um "scan" vermelho percorrendo a fita, deixando um rastro (trail) que
// vai desaparecendo em fade atras da cabeca. Roda por duracaoMs ms e retorna.
//
// Nao usa delay(): a posicao da cabeca e derivada do tempo decorrido (millis)
// e o loop cede o processador com yield() a cada iteracao. Assim nao trava o
// watchdog nem o USB-CDC, mesmo em duracoes maiores.
void redScanTrail(uint32_t duracaoMs) {
  // Quanto o rastro escurece a cada quadro (0..255). Maior = trail mais curto.
  const uint8_t FADE = 5;
  // Intervalo alvo entre quadros (~60 fps). Mantem a animacao suave.
  const uint32_t QUADRO_MS = 16;

  const uint32_t inicio = millis();
  uint32_t proximoQuadro = inicio;
  int ultimaCabeca = -1;

  while (true) {
    uint32_t agora = millis();
    uint32_t decorrido = agora - inicio;
    if (decorrido >= duracaoMs) {
      break;
    }

    // So processa um novo quadro quando chega o momento; senao cede a CPU.
    if ((int32_t)(agora - proximoQuadro) < 0) {
      yield();
      continue;
    }
    proximoQuadro += QUADRO_MS;

    // Progresso 0..1 no intervalo -> posicao da cabeca na area utilizavel.
    float progresso = (float)decorrido / (float)duracaoMs;
    int cabeca = (int)(progresso * STRIP_USABLE_LEDS);
    if (cabeca >= STRIP_USABLE_LEDS) {
      cabeca = STRIP_USABLE_LEDS - 1;
    }

    // Escurece os LEDs utilizaveis: cria o rastro que some atras da cabeca.
    for (uint16_t i = 0; i < STRIP_USABLE_LEDS; i++) {
      uint32_t cor = strip.getPixelColor(pix(i));
      uint8_t r = (uint8_t)(cor >> 16);
      uint8_t g = (uint8_t)(cor >> 8);
      uint8_t b = (uint8_t)cor;
      r = (r > FADE) ? (r - FADE) : 0;
      g = (g > FADE) ? (g - FADE) : 0;
      b = (b > FADE) ? (b - FADE) : 0;
      strip.setPixelColor(pix(i), r, g, b);
    }

    // Acende a cabeca em vermelho cheio (inclui LEDs pulados se o quadro andou
    // mais de uma posicao, para o scan nao deixar buracos).
    int de = (ultimaCabeca < 0) ? cabeca : ultimaCabeca + 1;
    for (int i = de; i <= cabeca; i++) {
      strip.setPixelColor(pix(i), 255, 0, 0);
    }
    ultimaCabeca = cabeca;

    strip.show();
    yield();
  }

  // Garante a fita apagada ao final do ciclo.
  clearWholeStrip();
}

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

// ---------------------------------------------------------------------------
// Debounce do lead-off (filtro de estabilidade)
//
// Problema: por ruido/aterramento, o sinal de lead-off do AD8232 chaveia
// rapido em lead-off real, fazendo o LED oscilar entre vermelho e verde
// (aparenta amarelo). Precisamos de um estado ESTAVEL.
//
// Criterio: numa janela deslizante de tempo, contamos quantas amostras
// indicaram "contato ok" (on). So declaramos ON estavel se a fracao de "on"
// na janela for alta (>= limiar); caso contrario, mantemos OFF (solto). Ruido
// espurio durante lead-off nao sustenta essa fracao, entao o estado fica
// estavel em OFF. Quando o contato e real, o sinal fica solido em "on" e
// cruza o limiar rapidamente.
//
// A 250 Hz, uma janela de 200 ms tem ~50 amostras.
// ---------------------------------------------------------------------------
const uint32_t LEADOFF_JANELA_MS   = 200;  // tamanho da janela de avaliacao
const uint8_t  LEADOFF_ON_PCT_MIN  = 80;   // % min de "on" na janela p/ ON

uint32_t leadOffJanelaInicioMs = 0;  // inicio da janela atual
uint16_t leadOffAmostras       = 0;  // total de amostras na janela
uint16_t leadOffAmostrasOn     = 0;  // amostras "on" (contato ok) na janela
int      leadOffEstavel        = 1;  // estado estavel atual (1=solto ao ligar)

// Alimenta o filtro com uma leitura crua (leadOffCru: 1=solto, 0=contato ok)
// e retorna o estado ESTAVEL de lead-off (1=solto, 0=contato ok).
int leadOffDebounce(int leadOffCru, uint32_t agoraMs) {
  leadOffAmostras++;
  if (leadOffCru == 0) {
    leadOffAmostrasOn++;  // "on" = contato ok
  }

  // Fechou a janela? Decide o estado estavel e reinicia a contagem.
  if ((uint32_t)(agoraMs - leadOffJanelaInicioMs) >= LEADOFF_JANELA_MS) {
    uint32_t pctOn = (leadOffAmostras > 0)
                       ? ((uint32_t)leadOffAmostrasOn * 100 / leadOffAmostras)
                       : 0;
    leadOffEstavel = (pctOn >= LEADOFF_ON_PCT_MIN) ? 0 : 1;  // 0=ok, 1=solto

    leadOffJanelaInicioMs = agoraMs;
    leadOffAmostras   = 0;
    leadOffAmostrasOn = 0;
  }

  return leadOffEstavel;
}

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

  // Inicializa e testa a fita WS2812 externa (GPIO13) no boot.
  strip.begin();
  strip.setBrightness(STRIP_BRIGHTNESS);
  clearWholeStrip();  // apaga TODOS os LEDs fisicos (elimina cores aleatorias)
  testStrip();

  // // Animacao de boot: scan vermelho com trail em fade, 3 passagens de 0.5s.
  // for (int i = 0; i < 3; i++) {
  //   redScanTrail(1500);
  // }

  // Identificacao no boot. A saida CSV por amostra esta desativada por
  // enquanto; a serial reporta apenas o BPM sorteado a cada ciclo de medicao.
  Serial.println("Toque Cardia ECG POC");
  Serial.println("# saida CSV desativada; reportando apenas bpm por ciclo");

  // Semeia o gerador aleatorio com ruido do ADC (pino de ECG flutuante) para
  // que a simulacao de BPM varie entre execucoes, e nao repita a mesma sequencia.
  randomSeed(analogRead(ECG_PIN) ^ micros());

  nextSampleUs = micros();
  leadOffJanelaInicioMs = millis();  // inicia a janela do filtro de lead-off
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

  // Resumo de lead-off cru: 1 se qualquer um dos dois indicar eletrodo solto.
  int leadOffCru = (loMinus || loPlus) ? 1 : 0;

  // Estado ESTAVEL apos o filtro de debounce (elimina o chaveamento rapido).
  int leadOff = leadOffDebounce(leadOffCru, millis());

  // Indicadores visuais usam o estado estavel:
  //  - LED onboard: verde=contato ok, vermelho=eletrodo solto.
  //  - Fita: heartbeat vermelho em lead-off; apagada em contato ok.
  updateLeadOffLed(leadOff);
  updateStrip(leadOff);

  // Saida CSV por amostra desativada por enquanto (foco na simulacao visual).
  // Mantemos a amostragem/temporizacao rodando; so nao imprimimos as linhas.
  // Para reativar, descomente o bloco abaixo.
  // #if INCLUDE_TIME_US
  //   Serial.print(timeUs);
  //   Serial.print(',');
  // #endif
  //   Serial.print(adc);
  //   Serial.print(',');
  //   Serial.print(loMinus);
  //   Serial.print(',');
  //   Serial.print(loPlus);
  //   Serial.print(',');
  //   Serial.println(leadOffCru);
  (void)timeUs;
  (void)adc;
}
