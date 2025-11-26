# 📻🎛️ Tuner FM sur ESP32

<div align="center">

**Mini-projet ESP32 + SI4703 + OLED**
Fabriquons une petite radio FM qui clignote et vibre au rythme des stations ✨


[![PlatformIO](https://img.shields.io/badge/PlatformIO-Ready-F5822A?logo=platformio&logoColor=white)](#)
[![Arduino](https://img.shields.io/badge/Arduino-Framework-00979D?logo=arduino&logoColor=white)](#)
[![PU2CLR SI470X](https://img.shields.io/badge/PU2CLR-SI470X-6C63FF)](#)
[![GyverOLED](https://img.shields.io/badge/GyverOLED-SSH1106-222222)](#)

</div>

---

## 🧭 Sommaire

| Chapitre | Description |
| - | - |
| ⚙️ Aperçu rapide | Pourquoi ce projet |
| 🔌 Matériel | Ce qu'il faut brancher |
| 🪛 Câblage | Pinout détaillé |
| 📺 Firmware | Comportement à l'écran |

---

## ⚙️ Aperçu rapide

- 🎯 Objectif : piloter un tuner SI4703 via le ESP32 et afficher fréquence, RSSI et volume sur un OLED 128x64.

---

## 🔌 Matériel requis

| 🧩 Pièce | Détails |
| --- | --- |
| 🟦 Carte | ESP32-WROVER |
| 📻 Tuner | Module SI4703 (breakout SDA/SCL/RESET) |
| 🖥️ Afficheur | OLED I2C 128x64 (SSH1106) |
| 🎚️ Contrôles | 2 potentiomètres 10 kΩ (fréquence / volume) |
| 🔊 Sortie audio | Casque ou mini HP + simple antenne |

<p align="center">
  <img src="https://img.shields.io/badge/OLED-128x64-000000?logo=arduino&logoColor=white" alt="OLED badge">
  <img src="https://img.shields.io/badge/Tuner-SI4703-FF6F00" alt="SI4703 badge">
</p>

---

## 🪛 Câblage (style STM32)

| Fonction | Broche ESP32 | Commentaire |
| --- | --- | --- |
| SDA I2C | `21` | Bus partagé OLED + SI4703 |
| SCL I2C | `22` | Bus partagé OLED + SI4703 |
| Reset SI4703 | `25` | Maintenu haut avant init radio |
| Pot fréquence | `13` | Ajuster `FREQ_POT_PIN` si besoin |
| Pot volume | `12` | Ajuster `VOL_POT_PIN` si besoin |

Macros disponibles dans `src/main.cpp` :

```cpp
constexpr uint8_t ESP32_I2C_SDA = 21;
constexpr uint8_t ESP32_I2C_SCL = 22;
constexpr uint8_t RADIO_RESET_PIN = 25;
```


ℹ️ Tout est préconfiguré dans `platformio.ini` (`env:freenove_esp32_wrover`, dépendances GyverOLED + PU2CLR SI470X).

---

## 📺 Firmware en action

- Bande FM Europe par défaut (87.5 - 108 MHz) avec preset 97.5 MHz.
- Lecture analogique des potentiomètres avec deadband pour éviter les retunes intempestifs.
- Rafraîchissement OLED toutes les 500 ms : fréquence, RSSI en dB, volume sur 15 niveaux.
- En cas d'échec I2C/SI4703, message d'erreur en clair sur l'écran pour guider le debug.

---

<div align="center">

⭐ *Keep it simple. Ship often. Écoute la radio en codant !* ⭐

</div>
