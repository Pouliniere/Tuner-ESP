#include <Arduino.h>
#include <GyverOLED.h>
#include <SI470X.h>
#include <Wire.h>

constexpr uint8_t OLED_ADDR = 0x3C;
constexpr uint8_t RADIO_RESET_PIN = 14;
constexpr uint8_t ESP32_I2C_SDA = 21;
constexpr uint8_t ESP32_I2C_SCL = 22;

constexpr float FM_MIN_MHZ = 87.5f;
constexpr float FM_MAX_MHZ = 108.0f;
constexpr uint16_t FM_MIN_01MHZ = 8750;   // hundredths of MHz
constexpr uint16_t FM_MAX_01MHZ = 10800;  // hundredths of MHz
constexpr uint8_t FM_STEP_01MHZ = 10;     // 0.1 MHz step

constexpr uint8_t VOLUME_MAX = 15;
constexpr uint16_t UI_REFRESH_MS = 400;
constexpr uint16_t SWEEP_INTERVAL_MS = 250;  // temps entre chaque saut de frequence

// Modifiez ces deux variables pour changer la station et le volume.
uint16_t targetFrequency01 = 10170;  // 101.70 MHz (MHz * 100)
uint8_t targetVolume = 8;            // 0..15

SI470X radio;
GyverOLED<SSH1106_128x64> oled(OLED_ADDR);

uint16_t tunedFrequency01 = FM_MIN_01MHZ;
uint8_t tunedVolume = 0;
unsigned long lastUiUpdate = 0;
unsigned long lastSweepUpdate = 0;
bool radioReady = false;

uint16_t quantizeFrequency(uint16_t freq01) {
  freq01 = constrain(freq01, FM_MIN_01MHZ, FM_MAX_01MHZ);
  const uint16_t remainder = freq01 % FM_STEP_01MHZ;
  if (remainder >= FM_STEP_01MHZ / 2) {
    freq01 += (FM_STEP_01MHZ - remainder);
  } else {
    freq01 -= remainder;
  }
  return constrain(freq01, FM_MIN_01MHZ, FM_MAX_01MHZ);
}

float toMHz(uint16_t freq01) {
  return static_cast<float>(freq01) / 100.0f;
}

void drawUi() {
  const int rssi = radio.getRssi();
  const bool stereo = radio.isStereo();

  oled.clear();
  oled.setScale(1);
  oled.setCursorXY(0, 0);
  oled.println(F("SI470X + ESP32"));

  oled.setScale(2);
  oled.setCursorXY(0, 16);
  oled.printf("%05.2f", toMHz(tunedFrequency01));
  oled.setScale(1);
  oled.setCursorXY(98, 26);
  oled.print(F("MHz"));

  oled.setCursorXY(0, 44);
  oled.printf("RSSI: %3ddB", rssi);

  oled.setCursorXY(0, 56);
  oled.printf("Vol: %2u/15 %s", tunedVolume, stereo ? "ST" : "MO");

  oled.update();
}

void setupRadio() {
  radio.setup(RADIO_RESET_PIN, ESP32_I2C_SDA);
  radio.setBand(FM_BAND_USA_EU);
  radio.setSpace(1);  // 100 kHz
  radio.setMono(false);
  radio.setMute(false);
  radio.setSoftmute(false);

  tunedFrequency01 = quantizeFrequency(targetFrequency01);
  radio.setFrequency(tunedFrequency01);

  tunedVolume = (targetVolume > VOLUME_MAX) ? VOLUME_MAX : targetVolume;
  radio.setVolume(tunedVolume);
}

void setup() {
  Serial.begin(115200);
  Serial.println(F("[SYSTEM] Demarrage du tuner SI470X (PU2CLR)"));

  Wire.begin(ESP32_I2C_SDA, ESP32_I2C_SCL);
  Wire.setClock(400000);

  oled.init();
  oled.clear();
  oled.setScale(1);
  oled.setCursorXY(0, 24);
  oled.println(F("Init radio..."));
  oled.update();

  setupRadio();
  radioReady = true;

  drawUi();
  Serial.println(F("[SYSTEM] Pret - frequence/volume fixes en code"));
}

void loop() {
  if (!radioReady) return;

  const unsigned long now = millis();

  if (now - lastSweepUpdate >= SWEEP_INTERVAL_MS) {
    lastSweepUpdate = now;
    tunedFrequency01 += FM_STEP_01MHZ;
    if (tunedFrequency01 > FM_MAX_01MHZ) {
      tunedFrequency01 = FM_MIN_01MHZ;
    }
    radio.setFrequency(tunedFrequency01);
  }

  if (now - lastUiUpdate >= UI_REFRESH_MS) {
    lastUiUpdate = now;
    drawUi();
  }
}
