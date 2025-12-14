#include <Arduino.h>
#include <driver/i2s.h>
#include <string.h>

// ==== BLE LIGERO (NimBLE) ====
#include <NimBLEDevice.h>

// ==== EDGE IMPULSE ====
#include <alexisjhair14-project-1_inferencing.h>
#include <model-parameters/model_metadata.h>

// ==== PINES I2S ====
#define I2S_WS_PIN   25
#define I2S_SCK_PIN  26
#define I2S_SD_PIN   32
#define I2S_PORT     I2S_NUM_0

// ==== AUDIO / EI ====
#define SAMPLE_RATE            16000
#define BITS_PER_SAMPLE        I2S_BITS_PER_SAMPLE_32BIT
#define I2S_READ_CHUNK_I32     512

// ---- UMBRALES RMS (ajústalos a tu entorno) ----
#define WHISPER_RMS_THRESHOLD     150.0f   // "no ruido" / muy bajo (modo verde)
#define MODEL_MIN_RMS_THRESHOLD   450.0f   // "decibelios mínimos" para activar el modelo

// ==== BUFFERS ====
static int32_t  i2s_buf[I2S_READ_CHUNK_I32];
static float    float_buf[EI_CLASSIFIER_RAW_SAMPLE_COUNT];

// HPF
static int32_t hp_x_prev = 0, hp_y_prev = 0;
static const float HP_A = 0.995f;

// ==== MOTORES VIBRACIÓN ====
#define LEFT_MOTOR_PIN   27
#define RIGHT_MOTOR_PIN  14

#define MOTOR_ACTIVE_HIGH 1
#define MOTOR_OFF  (MOTOR_ACTIVE_HIGH ? LOW  : HIGH)
#define MOTOR_ON   (MOTOR_ACTIVE_HIGH ? HIGH : LOW)

#define CONF_THRESHOLD 0.60f

struct PatternStep { uint16_t duration_ms; bool level; };

const PatternStep PATTERN_SIREN[]    = { {120, true}, {80, false} };
const PatternStep PATTERN_CAR_HORN[] = { {200, true} };
const PatternStep PATTERN_SHORT[]    = { {80,  true}, {120, false} };

struct MotorPlayer {
  int pin;  
  const PatternStep* pattern;
  uint8_t length;
  uint8_t index;
  unsigned long step_deadline;
  bool playing;
  bool loop;
};

MotorPlayer leftPlayer   = { LEFT_MOTOR_PIN,  nullptr, 0,0,0,false,false };
MotorPlayer rightPlayer  = { RIGHT_MOTOR_PIN, nullptr, 0,0,0,false,false };

static char last_top_label[32] = "";

// ==== LEDS ESTADO ====
// Verde (sin ruido)
#define LED_GREEN_PIN   4    
// Amarillo (susurro)
#define LED_YELLOW_PIN  19   
// Rojo (peligro)
#define LED_RED_PIN     18   

static void leds_set(bool g, bool y, bool r) {
  digitalWrite(LED_GREEN_PIN,  g ? HIGH : LOW);
  digitalWrite(LED_YELLOW_PIN, y ? HIGH : LOW);
  digitalWrite(LED_RED_PIN,    r ? HIGH : LOW);
}

// ==== BLE (NimBLE) ====
#define BLE_DEVICE_NAME "LentesSordos"
#define SERVICE_UUID    "12345678-1234-1234-1234-1234567890ab"
#define CHAR_EVENT_UUID "abcdefab-1234-5678-9abc-1234567890ab"

NimBLEServer*         pServer    = nullptr;
NimBLECharacteristic* pEventChr  = nullptr;
bool bleConnected                = false;

class MyServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo &connInfo) override {
    bleConnected = true;
    Serial.println("BLE conectado");
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo &connInfo, int reason) override {
    bleConnected = false;
    Serial.println("BLE desconectado, advertising...");
    NimBLEDevice::startAdvertising();
  }
};

static void motor_stop(MotorPlayer &mp) {
  mp.playing = false;
  mp.pattern = nullptr;
  mp.length = 0;
  mp.index = 0;
  mp.loop = false;
  digitalWrite(mp.pin, MOTOR_OFF);
}

static void motor_start(MotorPlayer &mp, const PatternStep* p, uint8_t len, bool loop) {
  if (!p || len == 0) { motor_stop(mp); return; }
  mp.pattern = p;
  mp.length = len;
  mp.index = 0;
  mp.playing = true;
  mp.loop = loop;

  digitalWrite(mp.pin, p[0].level ? MOTOR_ON : MOTOR_OFF);
  mp.step_deadline = millis() + p[0].duration_ms;
}

static void motor_update(MotorPlayer &mp, unsigned long now) {
  if (!mp.playing || !mp.pattern) return;
  if (now < mp.step_deadline) return;

  mp.index++;
  if (mp.index >= mp.length) {
    if (mp.loop) mp.index = 0;
    else { motor_stop(mp); return; }
  }
  const PatternStep &s = mp.pattern[mp.index];
  digitalWrite(mp.pin, s.level ? MOTOR_ON : MOTOR_OFF);
  mp.step_deadline = now + s.duration_ms;
}

static void get_pattern_for_label(const char* label,
                                  const PatternStep* &p,
                                  uint8_t &len,
                                  bool &loop) {
  if (strcmp(label, "siren") == 0) {
    p = PATTERN_SIREN;
    len = sizeof(PATTERN_SIREN)/sizeof(PATTERN_SIREN[0]);
    loop = true;
  } else if (strcmp(label, "car_horn") == 0) {
    p = PATTERN_CAR_HORN;
    len = sizeof(PATTERN_CAR_HORN)/sizeof(PATTERN_CAR_HORN[0]);
    loop = true;
  } else {
    p = PATTERN_SHORT;
    len = sizeof(PATTERN_SHORT)/sizeof(PATTERN_SHORT[0]);
    loop = false;
  }
}

// Etiquetas de peligro (SOLO siren y car_horn para vibración)
static bool is_danger_label(const char* label) {
  return (strcmp(label, "siren") == 0 ||
          strcmp(label, "car_horn") == 0);
}

// ==== BLE helpers ====
void ble_init() {
  NimBLEDevice::init(BLE_DEVICE_NAME);
  NimBLEDevice::setPower(ESP_PWR_LVL_P7);

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  NimBLEService* pService = pServer->createService(SERVICE_UUID);

  pEventChr = pService->createCharacteristic(
      CHAR_EVENT_UUID,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );

  pEventChr->createDescriptor("2902"); // CCCD

  pService->start();

  NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
  pAdv->addServiceUUID(SERVICE_UUID);

  NimBLEDevice::startAdvertising();

  Serial.println("BLE iniciado (NimBLE), anunciando como \"LentesSordos\"");
}

// ---- NUEVAS FUNCIONES DE MAPEO CORTO ----
static const char* map_sound_code(const char* label) {
  if (strcmp(label, "siren") == 0)            return "Si";
  if (strcmp(label, "car_horn") == 0)         return "Ca";
  if (strcmp(label, "drilling") == 0)         return "Dr";
  if (strcmp(label, "engine_idling") == 0)    return "En";
  if (strcmp(label, "air_conditioner") == 0)  return "Ai";
  // Voz (modo LED amarillo)
  if (strcmp(label, "voz") == 0)              return "Vz";
  if (strcmp(label, "voice") == 0)            return "Vz";
  return "Un"; // unknown
}

static const char* map_side_code(const char* lado) {
  if (strcmp(lado, "izquierda") == 0) return "Iz";
  if (strcmp(lado, "derecha")   == 0) return "Der";
  if (strcmp(lado, "centro")    == 0) return "Ce";
  return "Ce";
}

// ---- JSON CORTO (<= 20 caracteres) ----
void ble_notify_event(const char* top, const char* lado, float conf) {
  if (!bleConnected || pEventChr == nullptr) return;

  const char* sCode = map_sound_code(top);
  const char* lCode = map_side_code(lado);

  // Ejemplo: {"S":"Dr","L":"Der"}
  char buf[32];
  snprintf(buf, sizeof(buf),
           "{\"S\":\"%s\",\"L\":\"%s\"}",
           sCode, lCode);

  pEventChr->setValue((uint8_t*)buf, strlen(buf));
  pEventChr->notify();

  Serial.print("[BLE JSON enviado] → ");
  Serial.println(buf);

  Serial.printf("[DEBUG] S=%s  L=%s  (label original=%s, lado original=%s)\n",
                sCode, lCode, top, lado);
}

// ==== AUDIO ====
static inline int16_t i24_to_i16(int32_t s32) {
  return (int16_t)(s32 >> 16);
}

static void i2s_install_and_start() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = BITS_PER_SAMPLE,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 512,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pins = {
    .bck_io_num = I2S_SCK_PIN,
    .ws_io_num = I2S_WS_PIN,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD_PIN
  };

  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_PORT, &pins);
  i2s_set_clk(I2S_PORT, SAMPLE_RATE, BITS_PER_SAMPLE, I2S_CHANNEL_STEREO);
}

// *** AQUÍ ESTÁ LA INVERSIÓN DE LADOS ***
static const char* detect_side(float rmsL, float rmsR, float thr = 1.25f) {
  // Micros físicos están cruzados: invertimos etiquetas
  if (rmsL > rmsR * thr) return "derecha";    // antes "izquierda"
  if (rmsR > rmsL * thr) return "izquierda";  // antes "derecha"
  return "centro";
}

static void capture_fill_mono(float &rmsL, float &rmsR) {
  rmsL = 0; rmsR = 0;
  hp_x_prev = 0;
  hp_y_prev = 0;

  int filled = 0;
  while (filled < EI_CLASSIFIER_RAW_SAMPLE_COUNT) {
    size_t bytes_read = 0;
    i2s_read(I2S_PORT, (void*)i2s_buf, sizeof(i2s_buf),
             &bytes_read, portMAX_DELAY);

    int samples32 = bytes_read / sizeof(int32_t);
    if (samples32 < 2) continue;

    unsigned long now = millis();
    motor_update(leftPlayer, now);
    motor_update(rightPlayer, now);

    for (int i = 0; i + 1 < samples32 && filled < EI_CLASSIFIER_RAW_SAMPLE_COUNT; i += 2) {
      int16_t L = i24_to_i16(i2s_buf[i]);
      int16_t R = i24_to_i16(i2s_buf[i+1]);

      rmsL += (float)L * L;
      rmsR += (float)R * R;

      int32_t m = ((int32_t)L + R) / 2;

      int32_t x = m;
      int32_t y = x - hp_x_prev + (int32_t)(HP_A * hp_y_prev);
      hp_x_prev = x;
      hp_y_prev = y;

      if (y > 32767) y = 32767;
      if (y < -32768) y = -32768;

      float_buf[filled++] = (float)y;
    }
  }

  float N = (float)EI_CLASSIFIER_RAW_SAMPLE_COUNT;
  rmsL = sqrtf(rmsL / N);
  rmsR = sqrtf(rmsR / N);
}

static void print_plotter_line_all_labels(float zero,
                                          const ei_impulse_result_t *res) {
  for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
    const char *label = ei_classifier_inferencing_categories[i];
    float v = res ? res->classification[i].value * 100.0f : zero;
    Serial.print(label); Serial.print(":"); Serial.print(v, 2);
    if (i+1 < EI_CLASSIFIER_LABEL_COUNT) Serial.print(" ");
  }
  Serial.println();
}

// ==== SETUP ====
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nESP32 + 2x INMP441 + Vibración L/R + NimBLE BLE + LEDs estado");

  pinMode(LEFT_MOTOR_PIN, OUTPUT);
  pinMode(RIGHT_MOTOR_PIN, OUTPUT);
  digitalWrite(LEFT_MOTOR_PIN, MOTOR_OFF);
  digitalWrite(RIGHT_MOTOR_PIN, MOTOR_OFF);

  // LEDs
  pinMode(LED_GREEN_PIN, OUTPUT);
  pinMode(LED_YELLOW_PIN, OUTPUT);
  pinMode(LED_RED_PIN, OUTPUT);
  leds_set(true, false, false); // Arranca en verde

  ble_init();
  i2s_install_and_start();

  print_plotter_line_all_labels(0.0f, nullptr);
}

// ==== LOOP ====
void loop() {
  float rmsL = 0, rmsR = 0;
  capture_fill_mono(rmsL, rmsR);

  float rmsMax = (rmsL > rmsR ? rmsL : rmsR);

  // 1) VERDE -> casi sin ruido (por debajo de susurro)
  if (rmsMax < WHISPER_RMS_THRESHOLD) {
    motor_stop(leftPlayer);
    motor_stop(rightPlayer);
    last_top_label[0] = '\0';
    print_plotter_line_all_labels(0, nullptr);

    // LED verde: no ruido / no detección
    leds_set(true, false, false);

    delay(5);
    return;
  }

  // 2) AMARILLO -> susurro / voz baja, pero por debajo del mínimo para el modelo
  if (rmsMax >= WHISPER_RMS_THRESHOLD && rmsMax < MODEL_MIN_RMS_THRESHOLD) {
    motor_stop(leftPlayer);
    motor_stop(rightPlayer);
    last_top_label[0] = '\0';
    print_plotter_line_all_labels(0, nullptr);

    leds_set(false, true, false);

    const char* lado = detect_side(rmsL, rmsR);

    // 🔊 Nuevo: enviar evento de VOZ por BLE
    ble_notify_event("voz", lado, 0.0f);

    Serial.printf("[AMARILLO] Susurro / voz baja RMS_L=%.1f RMS_R=%.1f rmsMax=%.1f lado=%s\n",
                  rmsL, rmsR, rmsMax, lado);
    delay(5);
    return;
  }

  // 3) rmsMax >= MODEL_MIN_RMS_THRESHOLD
  //    -> ya superó los "decibelios mínimos" -> activamos modelo
  signal_t signal;
  if (numpy::signal_from_buffer(float_buf, EI_CLASSIFIER_RAW_SAMPLE_COUNT, &signal) != 0) {
    Serial.println("ERR: signal_from_buffer");
    return;
  }

  ei_impulse_result_t result = {0};
  if (run_classifier(&signal, &result, false) != EI_IMPULSE_OK) {
    Serial.println("ERR: run_classifier");
    return;
  }

  // Línea para el Plotter (todas las clases en %)
  print_plotter_line_all_labels(0, &result);

  // Top-1
  const char *best_label = "";
  float best_val = -1.0f;
  for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
    if (result.classification[i].value > best_val) {
      best_val   = result.classification[i].value;
      best_label = result.classification[i].label;
    }
  }

  const char* lado = detect_side(rmsL, rmsR);

  Serial.printf("[MODELO ACTIVO] TOP=%s conf=%.2f lado=%s RMS_L=%.1f RMS_R=%.1f rmsMax=%.1f\n",
                best_label, best_val, lado, rmsL, rmsR, rmsMax);

  // Notificación BLE mini JSON (corto)
  ble_notify_event(best_label, lado, best_val);

  // ---------- Vibración + LEDs ----------
  if (best_val >= CONF_THRESHOLD && is_danger_label(best_label)) {
    // Evento peligroso reconocido -> LED ROJO
    leds_set(false, false, true);

    bool changed = (strcmp(best_label, last_top_label) != 0);

    if (changed) {
      const PatternStep *patt;
      uint8_t plen;
      bool loop;
      get_pattern_for_label(best_label, patt, plen, loop);

      unsigned long now = millis();

      if (strcmp(lado, "izquierda") == 0) {
        motor_start(leftPlayer, patt, plen, loop);
        motor_stop(rightPlayer);
      } else if (strcmp(lado, "derecha") == 0) {
        motor_start(rightPlayer, patt, plen, loop);
        motor_stop(leftPlayer);
      } else {
        motor_start(leftPlayer, patt, plen, loop);
        motor_start(rightPlayer, patt, plen, loop);
      }

      strncpy(last_top_label, best_label, sizeof(last_top_label)-1);
      last_top_label[sizeof(last_top_label)-1] = '\0';

      motor_update(leftPlayer, now);
    } else {
      unsigned long now = millis();
      motor_update(leftPlayer, now);
      motor_update(rightPlayer, now);
    }
  } else {
    // Sonido por encima del umbral del modelo, pero sin label peligroso confiable
    // -> mantenemos AMARILLO
    leds_set(false, true, false);

    motor_stop(leftPlayer);
    motor_stop(rightPlayer);
    last_top_label[0] = '\0';
  }
}
