#include <Arduino.h>
#include <Wire.h>
#include <GyverOLED.h>

const uint8_t OLED_ADDR = 0x3C;
const uint8_t SI4703_ADDR = 0x10;
const uint8_t RADIO_RESET_PIN = 14;
const uint8_t ESP32_I2C_SDA = 21;
const uint8_t ESP32_I2C_SCL = 22;
const uint8_t REG_STATUSRSSI = 0x0A;
//const uint16_t FREQ_KHZ = 104300;
//const uint16_t BASE_KHZ = 87500;
const uint16_t STEP_KHZ = 200;

GyverOLED<SSH1106_128x64> display(OLED_ADDR);

char txt[25];
int si4703Regs[16];

bool lecture_regs() {

  Wire.beginTransmission(SI4703_ADDR);
  Wire.write(0xA0);
  Wire.write(0x00);
  Wire.write(0x00);
  Wire.endTransmission();

  Wire.beginTransmission(SI4703_ADDR);
  Wire.write(REG_STATUSRSSI);
  Wire.endTransmission();

  Wire.requestFrom(SI4703_ADDR,32);
  
    for (uint8_t i = 0x0A; i <= 0x0F; i++)
    {
    uint8_t hi = Wire.read();
    uint8_t lo = Wire.read(); 
    si4703Regs[i] = (static_cast<uint16_t>(hi) << 8) | lo;
    }

    for (uint8_t i = 0x00; i <= 0x09; i++)
    {
    uint8_t hi = Wire.read();
    uint8_t lo = Wire.read(); 
    si4703Regs[i] = (static_cast<uint16_t>(hi) << 8) | lo;
    }
  
  return true;
}

void Ecriture_Regs() 
{
  Wire.beginTransmission(SI4703_ADDR);
  for (uint8_t reg = 0x02; reg <= 0x07; reg++)
   {
    Wire.write(static_cast<uint8_t>(si4703Regs[reg] >> 8));
    Wire.write(static_cast<uint8_t>(si4703Regs[reg] & 0xFF));
   }
  Wire.endTransmission();
}



void Affiche_Regs() 
{
  display.clear();
  display.setCursorXY(0, 0);
  

  sprintf(txt,"Ox%04X 0x%04X 0x%04X",si4703Regs[0],si4703Regs[1],si4703Regs[2]);
  display.println(txt);
  display.setCursorXY(0, 8);
 sprintf(txt,"0x%04X 0x%04X 0x%04X",si4703Regs[3],si4703Regs[4],si4703Regs[5]);
  display.println(txt);
  display.setCursorXY(0, 16);
sprintf(txt,"0x%04X 0x%04X 0x%04X",si4703Regs[6],si4703Regs[7],si4703Regs[8]);
  display.println(txt);
  display.setCursorXY(0, 24);
sprintf(txt,"0x%04X 0x%04X 0x%04X",si4703Regs[9],si4703Regs[10],si4703Regs[11]);
  display.println(txt);
  display.setCursorXY(0, 32);
sprintf(txt,"0x%04X 0x%04X 0x%04X",si4703Regs[12],si4703Regs[13],si4703Regs[14]);
  display.println(txt);
  display.setCursorXY(0, 40);
sprintf(txt,"0x%04X",si4703Regs[15]);
  display.println(txt);
display.update();
}

void resetRadio() 
{
  digitalWrite(RADIO_RESET_PIN, LOW);
  delay(10);
  digitalWrite(RADIO_RESET_PIN, HIGH);
  delay(10);
}


void setup() {
  display.init();
  display.clear();
  resetRadio();
  lecture_regs();
  Affiche_Regs();
}


void loop() {
}

/*
bool readRegs() {
  Wire.beginTransmission(SI4703_ADDR);
  Wire.write(REG_STATUSRSSI);
  if (Wire.endTransmission() != 0) return false;

  if (Wire.requestFrom(SI4703_ADDR, static_cast<uint8_t>(32)) != 32)
    return false;

  for (uint8_t i = 0; i < 16; i++) {
    uint8_t hi = Wire.read();
    uint8_t lo = Wire.read();
    uint8_t idx = (REG_STATUSRSSI + i) & 0x0F;  
    si4703Regs[idx] = (static_cast<uint16_t>(hi) << 8) | lo;
  }
  return true;
}

bool writeRegs() {
  Wire.beginTransmission(SI4703_ADDR);
  for (uint8_t reg = 0x02; reg <= 0x07; reg++) {
    Wire.write(static_cast<uint8_t>(si4703Regs[reg] >> 8));
    Wire.write(static_cast<uint8_t>(si4703Regs[reg] & 0xFF));
  }
  return Wire.endTransmission() == 0;
}

bool tune1043() {
  uint16_t channel = (FREQ_KHZ - BASE_KHZ) / STEP_KHZ;
  si4703Regs[0x03] &= 0xFE00;   
  si4703Regs[0x03] |= channel;  
  si4703Regs[0x03] |= (1u << 15);  

  if (!writeRegs()) return false;

  for (uint8_t i = 0; i < 40; i++) {
    delay(10);
    if (!readRegs()) return false;
    if (si4703Regs[REG_STATUSRSSI] & (1u << 14)) break;
  }

  si4703Regs[0x03] &= ~(1u << 15); 
  return writeRegs();
}

void showRegs() {
  display.clear();
  uint8_t y = 0;
  for (uint8_t reg = 0; reg < 16; reg += 2) {
    display.setCursorXY(0, y);
    display.printf("%02X:%04X %02X:%04X", reg, si4703Regs[reg],
                   reg + 1, si4703Regs[reg + 1]);
    y += 8;
  }
  display.update();
}

void setup() {
  Serial.begin(115200);
  display.init();

  Wire.begin(ESP32_I2C_SDA, ESP32_I2C_SCL);
  Wire.setClock(400000);

  pinMode(RADIO_RESET_PIN, OUTPUT);
  resetRadio(); 

  if (!readRegs()) {
    display.clear();
    display.setCursorXY(0, 0);
    display.println(F("Reg 0x00 KO"));
    display.update();
    Serial.println(F("[ERROR] Lecture reg00 impossible"));
    return;
  }

  if (!tune1043()) {
    display.clear();
    display.setCursorXY(0, 0);
    display.println(F("Tuning 104.3 KO"));
    display.update();
    Serial.println(F("[ERROR] Echec tune 104.3 MHz"));
    return;
  }

  Serial.println(F("[INFO] Tune RTL 104.3 OK"));
}

void loop() {
  if (!readRegs()) {
    Serial.println(F("[ERROR] Lecture reg KO"));
    delay(500);
    return;
  }

  showRegs(); 
  delay(700);
}
*/