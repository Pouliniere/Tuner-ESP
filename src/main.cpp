#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <GyverOLED.h>
#include <SI470X.h>
#include <cmath>

constexpr uint8_t OLED_ADDR = 0x3C;
constexpr uint8_t RADIO_RESET_PIN = 25;
constexpr uint8_t ESP32_I2C_SDA = 21;
constexpr uint8_t ESP32_I2C_SCL = 22;
constexpr uint8_t DEFAULT_VOLUME = 15;    // always max volume
constexpr uint16_t UI_REFRESH_MS = 500;
constexpr uint8_t FREQ_POT_PIN = 13;      // analog pot controlling frequency (GPIO13)
constexpr uint8_t SD_CS_PIN = 5;          // VSPI CS for SD module
constexpr uint8_t SD_SCK_PIN = 18;
constexpr uint8_t SD_MISO_PIN = 19;
constexpr uint8_t SD_MOSI_PIN = 23;
constexpr char PAIRING_TRACK_PATH[] = "/pairing.mp3";
constexpr float FM_MIN_MHZ = 87.5f;       // France band limits
constexpr float FM_MAX_MHZ = 108.0f;
constexpr uint16_t POT_DEADBAND_STEPS = 2;  // 0.02 MHz tolerance before retune
constexpr uint16_t FM_MIN_STEPS = static_cast<uint16_t>(FM_MIN_MHZ * 100);
constexpr uint16_t FM_MAX_STEPS = static_cast<uint16_t>(FM_MAX_MHZ * 100);
constexpr float STATION_HOLD_THRESHOLD_MHZ = 0.15f;  // stay latched if pot jitters +/- 150kHz
constexpr float LOCAL_STATIONS_MHZ[] = {
    87.6f, 88.2f, 89.3f, 90.0f, 91.0f, 92.6f, 93.9f, 95.0f,
    96.0f, 97.5f, 99.0f, 100.4f, 101.4f, 102.8f, 104.3f, 106.3f};
constexpr size_t LOCAL_STATIONS_COUNT = sizeof(LOCAL_STATIONS_MHZ) / sizeof(float);
float baseFrequencyMHz = 97.5f;             // initial preset for France
constexpr uint8_t PRESET_SLOT = 1;          // display "radio no."

GyverOLED<SSH1106_128x64> display(OLED_ADDR);
SI470X radio;
AudioGeneratorMP3 *pairingPlayer = nullptr;
AudioFileSourceSD *pairingFile = nullptr;
AudioFileSourceID3 *pairingId3 = nullptr;
AudioOutputI2S *pairingOutput = nullptr;

unsigned long lastScreenUpdate = 0;
bool radioReady = false;
uint16_t lastTunedSteps = 0;
bool sdReady = false;

uint16_t mhzToSteps(float mhz) {
  return static_cast<uint16_t>(mhz * 100.0f + 0.5f);
}

void cleanupPairingAudio() {
  if (pairingPlayer) {
    if (pairingPlayer->isRunning()) {
      pairingPlayer->stop();
    }
    delete pairingPlayer;
    pairingPlayer = nullptr;
  }
  if (pairingId3) {
    delete pairingId3;
    pairingId3 = nullptr;
  }
  if (pairingFile) {
    pairingFile->close();
    delete pairingFile;
    pairingFile = nullptr;
  }
  if (pairingOutput) {
    pairingOutput->stop();
    delete pairingOutput;
    pairingOutput = nullptr;
  }
}

bool initSdCard() {
  if (sdReady) {
    return true;
  }
  Serial.printf("[SD] Initialisation... CS=%u MOSI=%u MISO=%u SCK=%u\n",
                SD_CS_PIN, SD_MOSI_PIN, SD_MISO_PIN, SD_SCK_PIN);
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);
  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  if (!SD.begin(SD_CS_PIN, SPI, 20000000)) {
    Serial.println(F("[SD][ERROR] Card init failed (verifier MOSI/MISO/SCK/CS)"));
    return false;
  }
  sdReady = true;
  const uint32_t sizeMB = static_cast<uint32_t>(SD.cardSize() / (1024ULL * 1024ULL));
  Serial.printf("[SD] Card ready (%u MB)\n", sizeMB);
  return true;
}

bool playPairingTrack() {
  if (!sdReady && !initSdCard()) {
    Serial.println(F("[AUDIO][ERROR] SD init impossible, skip pairing.mp3"));
    return false;
  }

  pairingFile = new AudioFileSourceSD(PAIRING_TRACK_PATH);
  if (!pairingFile || !pairingFile->isOpen()) {
    Serial.println(F("[AUDIO][ERROR] pairing.mp3 introuvable sur la SD"));
    cleanupPairingAudio();
    return false;
  }

  pairingId3 = new AudioFileSourceID3(pairingFile);
  pairingOutput = new AudioOutputI2S(0, AudioOutputI2S::INTERNAL_DAC);
  pairingOutput->SetGain(1.0f);            // drive PAM8403 via DAC
  pairingOutput->SetChannels(2);           // stéréo
  pairingOutput->SetOutputModeMono(false);
  pairingPlayer = new AudioGeneratorMP3();
  pairingPlayer->begin(pairingId3, pairingOutput);

  Serial.println(F("[AUDIO] Lecture de pairing.mp3 sur DAC1=GPIO25 (L) / DAC2=GPIO26 (R) vers PAM8403"));
  unsigned long lastLog = millis();
  while (pairingPlayer->isRunning()) {
    if (!pairingPlayer->loop()) {
      pairingPlayer->stop();
      break;
    }
    const unsigned long now = millis();
    if (now - lastLog > 1000) {
      Serial.println(F("[AUDIO] ...lecture en cours..."));
      lastLog = now;
    }
    delay(1);
  }
  Serial.println(F("[AUDIO] Lecture terminée"));
  cleanupPairingAudio();
  return true;
}

float readPotFrequencyMHz() {
  const uint16_t raw = analogRead(FREQ_POT_PIN);
  const float ratio = static_cast<float>(raw) / 4095.0f;
  return FM_MIN_MHZ + ratio * (FM_MAX_MHZ - FM_MIN_MHZ);
}

float snapToStation(float requestedMHz) {
  if (LOCAL_STATIONS_COUNT == 0) {
    return requestedMHz;
  }

  if (lastTunedSteps != 0) {
    const float lastMHz = static_cast<float>(lastTunedSteps) / 100.0f;
    if (fabsf(requestedMHz - lastMHz) <= STATION_HOLD_THRESHOLD_MHZ) {
      return lastMHz;
    }
  }

  float snappedMHz = LOCAL_STATIONS_MHZ[0];
  float bestDiff = fabsf(requestedMHz - snappedMHz);
  for (size_t i = 1; i < LOCAL_STATIONS_COUNT; ++i) {
    const float diff = fabsf(requestedMHz - LOCAL_STATIONS_MHZ[i]);
    if (diff < bestDiff) {
      bestDiff = diff;
      snappedMHz = LOCAL_STATIONS_MHZ[i];
    }
  }
  return snappedMHz;
}

void updateFrequencyFromPot() {
  const float requestedMHz = readPotFrequencyMHz();
  const float snappedMHz = snapToStation(requestedMHz);
  const uint16_t targetSteps = mhzToSteps(snappedMHz);
  const int32_t delta = static_cast<int32_t>(targetSteps) - static_cast<int32_t>(lastTunedSteps);
  if (abs(delta) >= POT_DEADBAND_STEPS) {
    radio.setFrequency(targetSteps);
    lastTunedSteps = targetSteps;
    Serial.printf("[FREQ] Pot %.02f MHz -> snapped %.02f MHz (%u steps)\n", requestedMHz, snappedMHz, targetSteps);
  }
}

void drawRadioScreen(uint16_t freqSteps, uint8_t rssi, uint8_t volume) {
  display.clear();

  display.setScale(1);
  display.setCursorXY(0, 0);
  display.printf("Radio #%u", PRESET_SLOT);

  const float freqMHz = static_cast<float>(freqSteps) / 100.0f;
  display.setScale(2);
  display.setCursorXY(0, 16);
  display.printf("%05.2f", freqMHz);
  display.setScale(1);
  display.setCursorXY(96, 24);
  display.print(F("MHz"));

  display.setCursorXY(0, 44);
  display.printf("Signal: %3udB", rssi);

  display.setCursorXY(0, 56);
  display.printf("Volume: %2u/15", volume);

  display.update();
}

void showError(const char *message) {
  display.clear();
  display.setScale(1);
  display.setCursorXY(0, 0);
  display.println(F("FM Radio"));
  display.setCursorXY(0, 16);
  display.println(F("Init erreur :"));
  display.setCursorXY(0, 28);
  display.println(message);
  display.update();
}

bool initRadio() {
  radio.setup(RADIO_RESET_PIN, ESP32_I2C_SDA);
  delay(100);
  radio.setSeekThreshold(25);
  radio.setVolume(DEFAULT_VOLUME);
  radio.setFmDeemphasis(1);  // 50us Europe
  Serial.println(F("[RADIO] SI470X configured (seek=25, deemphasis=50us)"));
  const uint16_t freqSteps = mhzToSteps(baseFrequencyMHz);
  if (freqSteps < FM_MIN_STEPS || freqSteps > FM_MAX_STEPS) {
    Serial.printf("[RADIO][ERROR] Base frequency %.02f MHz out of range\n", baseFrequencyMHz);
    return false;
  }
  radio.setFrequency(freqSteps);
  lastTunedSteps = freqSteps;
  Serial.printf("[RADIO] Tuned to initial preset %.02f MHz\n", baseFrequencyMHz);
  return true;
}

void setup() {
  Serial.begin(115200);
  Serial.println(F("[AUDIO] Demarrage pairing mp3"));
  const bool pairingOk = playPairingTrack();
  Serial.printf("[AUDIO] pairing.mp3 %s\n", pairingOk ? "OK" : "ECHEC");

  Serial.println(F("[SYSTEM] Booting FM radio demo"));

  pinMode(FREQ_POT_PIN, INPUT);
  Wire.begin(ESP32_I2C_SDA, ESP32_I2C_SCL);
  Serial.println(F("[SYSTEM] GPIO and I2C ready"));

  if (!initRadio()) {
    display.init();
    showError("Frequence");
    Serial.println(F("[SYSTEM][ERROR] Radio init failed"));
    return;
  }
  radioReady = true;

  display.init();  // init after radio to avoid I2C conflicts
  Serial.println(F("[DISPLAY] OLED initialized"));
  drawRadioScreen(radio.getFrequency(), radio.getRssi(), radio.getVolume());
  Serial.println(F("[SYSTEM] Setup complete"));
}

void loop() {
  if (!radioReady) {
    return;
  }

  updateFrequencyFromPot();

  const unsigned long now = millis();
  if (now - lastScreenUpdate >= UI_REFRESH_MS) {
    lastScreenUpdate = now;
    const uint16_t freq = radio.getFrequency();
    const uint8_t rssi = radio.getRssi();
    const uint8_t volume = radio.getVolume();
    drawRadioScreen(freq, rssi, volume);
    Serial.printf("[UI] freq=%.02fMHz rssi=%udB volume=%u\n", static_cast<float>(freq) / 100.0f, rssi, volume);
  }
}