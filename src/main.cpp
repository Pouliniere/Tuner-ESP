#include <Arduino.h>
#include <Wire.h>
#include <GyverOLED.h>

const uint8_t OLED_ADDR = 0x3C;
const uint8_t SI4703_ADDR = 0x10;
const uint8_t RADIO_RESET_PIN = 14;
const uint8_t ESP32_I2C_SDA = 21;
const uint8_t ESP32_I2C_SCL = 22;
const uint8_t FREQ_POT_PIN = 18;
const uint8_t VOL_POT_PIN = 17;

const float FM_MIN_MHZ = 87.5f;
const float FM_MAX_MHZ = 108.0f;
const float FM_STEP_MHZ = 0.1f;
const uint16_t FM_MAX_CHANNEL =
    static_cast<uint16_t>((FM_MAX_MHZ - FM_MIN_MHZ) / FM_STEP_MHZ);
const uint8_t MAX_VOLUME = 15;
const uint8_t CHANNEL_DEADBAND = 1;
const uint16_t UI_REFRESH_MS = 500;
const uint16_t ANALOG_MAX = 4095;

const uint8_t REG_POWERCFG = 0x02;
const uint8_t REG_CHANNEL = 0x03;
const uint8_t REG_SYSCONFIG1 = 0x04;
const uint8_t REG_SYSCONFIG2 = 0x05;
const uint8_t REG_STATUSRSSI = 0x0A;
const uint8_t REG_TEST1 = 0x07;

const uint16_t CHANNEL_MASK = 0x03FF;
const uint16_t CHANNEL_TUNE_BIT = 1u << 15;
const uint16_t STATUS_STC_BIT = 1u << 14;
const uint16_t STATUS_SFBL_BIT = 1u << 13;

GyverOLED<SSH1106_128x64> display(OLED_ADDR);

uint16_t si4703Regs[16] = {};
bool radioReady = false;
uint16_t currentChannel = 0;
uint8_t currentVolume = 0;
unsigned long lastScreenUpdate = 0;

// Lit les 16 registres séquentiels (ref. Skyworks SI4702/03-C19 §4.5).
bool readRegs() {
  Wire.beginTransmission(SI4703_ADDR);
  Wire.write(REG_STATUSRSSI);  // datasheet: lire en commençant par 0x0A
  if (Wire.endTransmission() != 0) {
    return false;
  }

  const uint8_t received =
      Wire.requestFrom(SI4703_ADDR, static_cast<uint8_t>(32));
  if (received != 32) return false;

  for (uint8_t i = 0; i < 16; ++i) {
    const uint8_t highByte = Wire.read();
    const uint8_t lowByte = Wire.read();
    const uint8_t regIndex =
        (REG_STATUSRSSI + i) & 0x0F;  // wrap 0x0A..0x0F -> 0x00..0x09
    si4703Regs[regIndex] =
        static_cast<uint16_t>(highByte) << 8 | static_cast<uint16_t>(lowByte);
  }
  return true;
}

// Écrit REG02 à REG07 conformément à la fenêtre writeable de la datasheet.
bool writeRegs() {
  Wire.beginTransmission(SI4703_ADDR);
  Wire.write(REG_POWERCFG);
  for (uint8_t reg = REG_POWERCFG; reg <= 0x07; ++reg) {
    Wire.write(static_cast<uint8_t>(si4703Regs[reg] >> 8));
    Wire.write(static_cast<uint8_t>(si4703Regs[reg] & 0x00FF));
  }
  return Wire.endTransmission() == 0;
}

// Attend le flag STC (StatusRSSI[14]) pour valider TUNE/SEEK.
bool waitForStc(uint16_t timeoutMs = 200) {
  const unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    if (!readRegs()) {
      return false;
    }
    if (si4703Regs[REG_STATUSRSSI] & STATUS_STC_BIT) {
      return true;
    }
    delay(5);
  }
  return false;
}

// Convertit une fréquence MHz en canal 100 kHz (spacing = 100 kHz).
uint16_t freqToChannel(float mhz) {
  mhz = constrain(mhz, FM_MIN_MHZ, FM_MAX_MHZ);
  const float offset = mhz - FM_MIN_MHZ;
  uint16_t channel = static_cast<uint16_t>(offset / FM_STEP_MHZ + 0.5f);
  if (channel > FM_MAX_CHANNEL) channel = FM_MAX_CHANNEL;
  return channel;
}

// Convertit un numéro de canal en fréquence MHz (band 87.5-108 MHz).
float channelToFreq(uint16_t channel) {
  if (channel > FM_MAX_CHANNEL) channel = FM_MAX_CHANNEL;
  return FM_MIN_MHZ + static_cast<float>(channel) * FM_STEP_MHZ;
}

// Programme le volume via les bits 3:0 de SYSCONFIG2 (datasheet §5.4.3).
bool setVolume(uint8_t volume) {
  if (!readRegs()) return false;
  const uint8_t clamped = (volume > MAX_VOLUME) ? MAX_VOLUME : volume;
  si4703Regs[REG_SYSCONFIG2] &= 0xFFF0;
  si4703Regs[REG_SYSCONFIG2] |= clamped;
  if (!writeRegs()) return false;
  currentVolume = clamped;
  return true;
}

// Force la fréquence via REG03.CHAN et le bit TUNE (datasheet §5.4.1).
bool setChannel(uint16_t channel) {
  if (channel > FM_MAX_CHANNEL) channel = FM_MAX_CHANNEL;
  if (!readRegs()) return false;

  const uint16_t channelBits = channel & CHANNEL_MASK;
  si4703Regs[REG_CHANNEL] &= ~CHANNEL_MASK;
  si4703Regs[REG_CHANNEL] |= channelBits;
  si4703Regs[REG_CHANNEL] |= CHANNEL_TUNE_BIT;
  if (!writeRegs()) return false;

  if (!waitForStc()) {
    Serial.println(F("[RADIO][ERROR] STC timeout pendant TUNE"));
    return false;
  }

  si4703Regs[REG_CHANNEL] &= ~CHANNEL_TUNE_BIT;
  if (!writeRegs()) return false;

  if (!readRegs()) return false;
  if (si4703Regs[REG_STATUSRSSI] & STATUS_SFBL_BIT) {
    Serial.println(F("[RADIO][WARN] Station hors bande ou introuvable"));
  }

  currentChannel = channelBits;
  return true;
}

// Retourne le RSSI (REG0A[7:0]) comme indiqué dans §5.4.5.
uint8_t readRssi() {
  if (!readRegs()) return 0;
  return static_cast<uint8_t>(si4703Regs[REG_STATUSRSSI] & 0x00FF);
}

// Suit la séquence reset + activation du crystal (datasheet §3.5).
bool initRadio() {
  pinMode(RADIO_RESET_PIN, OUTPUT);
  digitalWrite(RADIO_RESET_PIN, LOW);
  delay(10);
  digitalWrite(RADIO_RESET_PIN, HIGH);
  delay(10);

  if (!readRegs()) {
    Serial.println(F("[RADIO][ERROR] Impossible de lire les registres"));
    return false;
  }

  si4703Regs[REG_TEST1] = 0x8100;
  if (!writeRegs()) {
    Serial.println(F("[RADIO][ERROR] Impossible d'activer l'oscillateur"));
    return false;
  }
  delay(500);

  if (!readRegs()) {
    Serial.println(F("[RADIO][ERROR] Lecture apres osc KO"));
    return false;
  }

  si4703Regs[REG_POWERCFG] = 0x4001;
  si4703Regs[REG_SYSCONFIG1] |= (1 << 11);
  si4703Regs[REG_SYSCONFIG2] &= ~(0x3 << 4);
  si4703Regs[REG_SYSCONFIG2] |= (0x1 << 4);
  si4703Regs[REG_SYSCONFIG2] &= ~(0x3 << 6);
  si4703Regs[REG_SYSCONFIG2] &= 0xFFF0;

  if (!writeRegs()) {
    Serial.println(F("[RADIO][ERROR] Configuration initiale impossible"));
    return false;
  }
  delay(110);
  return true;
}

// Mappe le potentiomètre de fréquence sur 87.5-108 MHz.
float readFreqPot() {
  const int raw = analogRead(FREQ_POT_PIN);
  const float ratio = static_cast<float>(raw) / static_cast<float>(ANALOG_MAX);
  return FM_MIN_MHZ + ratio * (FM_MAX_MHZ - FM_MIN_MHZ);
}

// Mappe le potentiomètre de volume sur 0-15 (4 bits).
uint8_t readVolPot() {
  const int raw = analogRead(VOL_POT_PIN);
  const uint32_t scaled = static_cast<uint32_t>(raw) * (MAX_VOLUME + 1);
  uint8_t volume = static_cast<uint8_t>(scaled / (ANALOG_MAX + 1));
  if (volume > MAX_VOLUME) {
    volume = MAX_VOLUME;
  }
  return volume;
}

// Affiche fréquence, RSSI et volume sur l’OLED.
void drawScreen(float freqMHz, uint8_t rssi, uint8_t volume) {
  display.clear();

  display.setScale(1);
  display.setCursorXY(0, 0);
  display.println(F("FM SI4703"));

  display.setScale(2);
  display.setCursorXY(0, 16);
  display.printf("%05.2f", freqMHz);
  display.setScale(1);
  display.setCursorXY(96, 24);
  display.print(F("MHz"));

  display.setCursorXY(0, 44);
  display.printf("Signal : %3udB", rssi);

  display.setCursorXY(0, 56);
  display.printf("Volume : %2u/15", volume);

  display.update();
}

// Affiche un message d’erreur simple.
void showError(const char* line1, const char* line2) {
  display.clear();
  display.setScale(1);
  display.setCursorXY(0, 0);
  display.println(F("FM Radio"));
  display.setCursorXY(0, 16);
  display.println(F("Erreur init"));
  display.setCursorXY(0, 32);
  display.println(line1);
  if (line2) {
    display.setCursorXY(0, 44);
    display.println(line2);
  }
  display.update();
}

// Ajuste la fréquence selon le potentiomètre (utilise le canal SI4703).
void applyPotFrequency() {
  const float requestedMHz = readFreqPot();
  const uint16_t targetChannel = freqToChannel(requestedMHz);
  const uint16_t delta =
      (currentChannel > targetChannel) ? (currentChannel - targetChannel)
                                       : (targetChannel - currentChannel);
  if (delta >= CHANNEL_DEADBAND && !setChannel(targetChannel)) {
    Serial.println(F("[RADIO][ERROR] Reglage frequence impossible"));
  } else if (delta >= CHANNEL_DEADBAND) {
    Serial.printf("[RADIO] Frequence demandee %.2f MHz -> canal %u\n",
                  requestedMHz, targetChannel);
  }
}

// Ajuste le volume si la position du potentiomètre change.
void applyPotVolume() {
  const uint8_t requestedVolume = readVolPot();
  if (requestedVolume != currentVolume) {
    if (!setVolume(requestedVolume)) {
      Serial.println(F("[RADIO][ERROR] Reglage volume KO"));
    } else {
      Serial.printf("[RADIO] Volume -> %u\n", requestedVolume);
    }
  }
}

// Initialise série, I2C, radio et écran.
void setup() {
  Serial.begin(115200);
  Serial.println(F("[SYSTEM] Demarrage radio SI4703 sans librairie"));

  pinMode(FREQ_POT_PIN, INPUT);
  pinMode(VOL_POT_PIN, INPUT);

  Wire.begin(ESP32_I2C_SDA, ESP32_I2C_SCL);
  Wire.setClock(400000);

  if (!initRadio()) {
    display.init();
    showError("SI4703 absent", "Verif I2C");
    return;
  }

  const float startMHz = readFreqPot();
  if (!setChannel(freqToChannel(startMHz))) {
    display.init();
    showError("Frequence KO", nullptr);
    Serial.println(F("[SYSTEM][ERROR] Impossible de caler la frequence"));
    return;
  }

  const uint8_t startVolume = readVolPot();
  if (!setVolume(startVolume)) {
    display.init();
    showError("Volume KO", nullptr);
    Serial.println(F("[SYSTEM][ERROR] Impossible de regler le volume"));
    return;
  }

  display.init();
  drawScreen(channelToFreq(currentChannel), readRssi(), currentVolume);
  radioReady = true;
  Serial.println(F("[SYSTEM] Pret a recevoir les commandes des potentiometres"));
}

// Boucle principale : lit les potars et actualise l’affichage.
void loop() {
  if (!radioReady) {
    return;
  }

  applyPotFrequency();
  applyPotVolume();

  const unsigned long now = millis();
  if (now - lastScreenUpdate >= UI_REFRESH_MS) {
    lastScreenUpdate = now;
    const float freqMHz = channelToFreq(currentChannel);
    const uint8_t rssi = readRssi();
    drawScreen(freqMHz, rssi, currentVolume);
  }
}
