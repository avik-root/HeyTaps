<div align="center">

# 💧 HeyTaps — Wireless Water Tank Monitor

[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![Arduino](https://img.shields.io/badge/Platform-Arduino%20%7C%20ESP32-blue?logo=arduino)](https://www.arduino.cc/)
[![NRF24L01](https://img.shields.io/badge/Radio-NRF24L01%2B-orange)](https://www.nordicsemi.com/)
[![HC-SR04](https://img.shields.io/badge/Sensor-HC--SR04%20Ultrasonic-9cf)](https://cdn.sparkfun.com/datasheets/Sensors/Proximity/HCSR04.pdf)
[![Status](https://img.shields.io/badge/Status-Production%20Ready-brightgreen)]()

> **HeyTaps** is a low-cost, wireless water-tank level monitoring system built on two ESP32 boards communicating over 2.4 GHz NRF24L01+ radio. The transmitter reads distance with an HC-SR04 ultrasonic sensor and temperature/humidity with a DHT11, then sends the data wirelessly to a receiver that drives an OLED display, coloured LEDs, a buzzer alarm, and a local Wi-Fi web dashboard.

</div>

---

## 📋 Table of Contents

- [System Architecture](#system-architecture)
- [Hardware Components](#hardware-components)
- [Pin Connection Tables](#pin-connection-tables)
  - [Transmitter – ESP32-C3 Super Mini](#transmitter--esp32-c3-super-mini)
  - [Receiver – ESP32 (38-pin DevKit)](#receiver--esp32-38-pin-devkit)
- [HC-SR04 Wiring Notes](#hc-sr04-wiring-notes)
- [NRF24L01+ Wiring Notes](#nrf24l01-wiring-notes)
- [Tank Calibration](#tank-calibration)
- [Software Setup](#software-setup)
  - [Required Libraries](#required-libraries)
  - [Board Definitions](#board-definitions)
  - [Flash Settings](#flash-settings)
- [Web Dashboard](#web-dashboard)
- [LED & Buzzer Alarm Logic](#led--buzzer-alarm-logic)
- [Serial Debug Reference](#serial-debug-reference)
- [Troubleshooting](#troubleshooting)
- [Project Structure](#project-structure)
- [License](#license)

---

## System Architecture

```
┌──────────────────────────────────┐         2.4 GHz NRF24L01+         ┌────────────────────────────────────────┐
│        TRANSMITTER NODE          │ ─────────────────────────────────▶ │          RECEIVER NODE                 │
│      ESP32-C3 Super Mini         │        250 kbps | CH 108           │       ESP32 38-pin DevKit              │
│                                  │        Pipe: "HTAP1"               │                                        │
│  ┌─────────────┐                 │                                     │  ┌──────────────┐  ┌──────────────┐   │
│  │  HC-SR04    │ Distance(mm)    │                                     │  │  SSD1306     │  │   Web AP     │   │
│  │  Ultrasonic │────────────────▶│  SensorData {                       │  │  OLED 128×64 │  │ 192.168.4.1  │   │
│  └─────────────┘                 │    float temperature;    ├─────────▶│  └──────────────┘  └──────────────┘   │
│  ┌─────────────┐                 │    float humidity;       │           │  ┌──────────────┐  ┌──────────────┐   │
│  │   DHT11     │ Temp + Hum ─────│    uint8_t waterLevel;   │           │  │  4× LEDs     │  │   Buzzer     │   │
│  └─────────────┘                 │    uint16_t distance_mm; │           │  │  R/Y/G/B     │  │   Alarm      │   │
└──────────────────────────────────┘  }                                  │  └──────────────┘  └──────────────┘   │
                                                                         └────────────────────────────────────────┘
```

---

## Hardware Components

| Qty | Component | Role |
|-----|-----------|------|
| 1 | **ESP32-C3 Super Mini** | Transmitter MCU |
| 1 | **ESP32 38-pin DevKit** (or Wroom-32) | Receiver MCU |
| 2 | **NRF24L01+ with PA+LNA** | 2.4 GHz wireless link |
| 1 | **HC-SR04** Ultrasonic Module | Distance measurement (tank level) |
| 1 | **DHT11** Temperature & Humidity Sensor | Environmental sensing |
| 1 | **SSD1306 OLED** 128×64 I2C | Receiver display |
| 4 | **5mm LEDs** (Red / Yellow / Green / Blue) | Water level indicators |
| 4 | **220 Ω resistors** | LED current limiting |
| 1 | **5V Active Buzzer** | Alarm output |
| 2 | **100 µF electrolytic capacitors** | NRF24L01 power decoupling |
| — | Jumper wires, breadboard or PCB | Connections |

> **⚡ Power Note:** The NRF24L01+ module runs on **3.3 V only**. Place a **100 µF capacitor** across its VCC and GND pins as close to the module as possible to prevent voltage drooping during transmission bursts.

---

## Pin Connection Tables

### Transmitter – ESP32-C3 Super Mini

#### NRF24L01+ → ESP32-C3

| NRF24L01+ Pin | ESP32-C3 Pin | Notes |
|---------------|-------------|-------|
| VCC | 3V3 | **3.3 V only** – add 100 µF cap |
| GND | GND | Common ground |
| CE | GPIO **3** | Chip Enable |
| CSN | GPIO **10** | SPI Chip Select |
| SCK | GPIO **4** | SPI Clock (SPI1 CLK) |
| MOSI | GPIO **6** | SPI Data Out |
| MISO | GPIO **5** | SPI Data In |
| IRQ | — | Not used (polling mode) |

#### HC-SR04 Ultrasonic → ESP32-C3

| HC-SR04 Pin | ESP32-C3 Pin | Notes |
|-------------|-------------|-------|
| VCC | **5V** (VBUS) | Requires 5 V supply |
| GND | GND | Common ground |
| TRIG | GPIO **1** | 10 µs trigger pulse output |
| ECHO | GPIO **2** | PWM echo input (3.3 V tolerant on C3) |

> **⚠ Voltage Divider:** Standard HC-SR04 ECHO pin outputs **5 V**. While most ESP32-C3 GPIOs are **5 V tolerant on input**, using a simple **voltage divider (10 kΩ / 20 kΩ)** or a logic level shifter is recommended for long-term reliability.

#### DHT11 → ESP32-C3

| DHT11 Pin | ESP32-C3 Pin | Notes |
|-----------|-------------|-------|
| VCC | 3V3 | Also works at 5 V |
| GND | GND | — |
| DATA | GPIO **0** | 10 kΩ pull-up to VCC recommended |

---

### Receiver – ESP32 (38-pin DevKit)

#### NRF24L01+ → ESP32

| NRF24L01+ Pin | ESP32 Pin | Notes |
|---------------|-----------|-------|
| VCC | 3V3 | **3.3 V only** – add 100 µF cap |
| GND | GND | Common ground |
| CE | GPIO **4** | Chip Enable |
| CSN | GPIO **5** | SPI Chip Select |
| SCK | GPIO **18** | VSPI Clock |
| MOSI | GPIO **23** | VSPI MOSI |
| MISO | GPIO **19** | VSPI MISO |
| IRQ | — | Not used |

#### SSD1306 OLED → ESP32

| OLED Pin | ESP32 Pin | Notes |
|----------|-----------|-------|
| VCC | 3V3 | — |
| GND | GND | — |
| SDA | GPIO **21** | I2C Data (default) |
| SCL | GPIO **22** | I2C Clock (default) |

> Default I2C address: **0x3C**. If your OLED uses 0x3D, change the `display.begin()` call accordingly.

#### LEDs → ESP32

| LED Colour | ESP32 Pin | Condition |
|------------|-----------|-----------|
| 🔴 Red | GPIO **32** | Water level < 25% |
| 🟡 Yellow | GPIO **33** | Water level 25–49% |
| 🟢 Green | GPIO **25** | Water level 50–74% |
| 🔵 Blue | GPIO **26** | Water level ≥ 75% |

> Connect each LED **anode → GPIO pin** through a **220 Ω resistor**, cathode to GND.

#### Buzzer → ESP32

| Buzzer Pin | ESP32 Pin | Notes |
|------------|-----------|-------|
| + (positive) | GPIO **27** | Active buzzer – driven HIGH to beep |
| − (negative) | GND | — |

---

## HC-SR04 Wiring Notes

```
         VCC (5V)         GND
           │               │
      ┌────┴───────────────┴────┐
      │       HC-SR04           │
      │  TRIG   ECHO            │
      └────┬──────┬─────────────┘
           │      │
      GPIO1│      │──[10kΩ]──[20kΩ]──GND
           │      │
         ESP32-C3 GPIO2
```

**How it works:**
1. MCU sends a **10 µs HIGH pulse** on TRIG.
2. HC-SR04 fires 8 × 40 kHz ultrasonic bursts.
3. ECHO goes HIGH for the duration of the round-trip.
4. Distance (mm) = `pulse_duration_µs × 0.1715`

**Valid range:** ~20 mm – ~4000 mm (4 m)  
**Accuracy:** ±3 mm

---

## NRF24L01+ Wiring Notes

Both modules are configured identically:

| Setting | Value | Reason |
|---------|-------|--------|
| Channel | **108** (2508 MHz) | Above most Wi-Fi traffic (2.4–2.48 GHz) |
| Data Rate | **250 kbps** | Best sensitivity and range |
| PA Level | **HIGH** | Maximum range |
| CRC | **16-bit** | Data integrity |
| Auto-ACK | **Enabled** | Reliable delivery confirmation |
| Pipe Address | `"HTAP1"` | Matches both ends |
| TX retries | 15 × 5 delay | Up to ~19 ms retry window |

---

## Tank Calibration

Open `Transiver/esp32c3.ino` and set these two constants near the top:

```cpp
const float TANK_EMPTY_CM = 30.0;  // distance (cm) from sensor to tank bottom when EMPTY
const float TANK_FULL_CM  =  5.0;  // distance (cm) from sensor to water surface when FULL
```

**How to measure:**
1. With the tank **completely empty**, hold the HC-SR04 at its mounting position and note the distance → `TANK_EMPTY_CM`
2. With the tank **completely full**, measure the distance to the water surface → `TANK_FULL_CM`

The firmware uses the formula:

```
waterLevel% = ((TANK_EMPTY_CM − measured_distance) / (TANK_EMPTY_CM − TANK_FULL_CM)) × 100
```

---

## Software Setup

### Required Libraries

Install all libraries via **Arduino IDE → Tools → Manage Libraries**:

| Library | Author | Install Name |
|---------|--------|--------------|
| RF24 | TMRh20 | `RF24` |
| DHT sensor library | Adafruit | `DHT sensor library` |
| Adafruit GFX Library | Adafruit | `Adafruit GFX Library` |
| Adafruit SSD1306 | Adafruit | `Adafruit SSD1306` |

> The **Adafruit BusIO** library will be installed automatically as a dependency of SSD1306.

### Board Definitions

**Add the ESP32 board package to Arduino IDE:**

1. Open **File → Preferences**
2. Add this URL to *Additional Boards Manager URLs*:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. Go to **Tools → Board → Boards Manager**, search **esp32**, install **esp32 by Espressif Systems**.

**Transmitter (ESP32-C3 Super Mini):**

| Setting | Value |
|---------|-------|
| Board | `ESP32C3 Dev Module` |
| USB CDC On Boot | `Enabled` (for Serial over USB) |
| Flash Mode | `DIO` |
| Flash Size | `4MB (32Mb)` |
| Upload Speed | `921600` |

**Receiver (ESP32 DevKit):**

| Setting | Value |
|---------|-------|
| Board | `ESP32 Dev Module` |
| Flash Frequency | `80MHz` |
| Flash Mode | `QIO` |
| Flash Size | `4MB (32Mb)` |
| Upload Speed | `921600` |
| Partition Scheme | `Default 4MB with spiffs` |

### Flash Settings

The receiver uses `Preferences` (NVS) to persist alarm thresholds across reboots. No special partition changes are needed — the default partition table includes NVS.

---

## Web Dashboard

The receiver creates a **Wi-Fi Access Point** and serves a web dashboard:

| Field | Value |
|-------|-------|
| SSID | `HeyTaps` |
| Password | `12345678` |
| IP Address | `192.168.4.1` |
| Port | `80` |
| Auto-refresh | Every **5 seconds** |

**Features:**
- Live water level bar graph
- Sensor cards: Level %, Distance (mm), Temperature, Humidity
- Status badge: Normal / Low Water / Overflow Risk
- Last-received timestamp
- Configurable low/high alarm thresholds (persisted to flash)

---

## LED & Buzzer Alarm Logic

### LED Indicators

| LED | Colour | Water Level Range | Meaning |
|-----|--------|------------------|---------|
| GPIO 32 | 🔴 Red | < 25% | Critically Low |
| GPIO 33 | 🟡 Yellow | 25–49% | Getting Low |
| GPIO 25 | 🟢 Green | 50–74% | Good |
| GPIO 26 | 🔵 Blue | ≥ 75% | Full |

### Buzzer Alarm

| Condition | Beep Pattern | Interval |
|-----------|-------------|---------|
| Water ≤ Low threshold (default 20%) | Slow beep | 600 ms |
| Water ≥ High threshold (default 95%) | Fast beep | 150 ms |
| Normal | Silent | — |

Buzzer control is **non-blocking** (uses `millis()` timer), so the web server and radio polling remain responsive during alarms.

---

## Serial Debug Reference

Both boards output at **115200 baud**.

**Transmitter output example:**
```
[INFO] Transmitter ready.
[INFO] Tank empty distance : 30.00 cm
[INFO] Tank full  distance :  5.00 cm
[TX] Dist=  127mm  Water= 73%  Temp=28.5°C  Hum=61.0%  RF=OK
[TX] Dist=  130mm  Water= 72%  Temp=28.6°C  Hum=61.0%  RF=OK
```

**Receiver output example:**
```
[INFO] HeyTaps Receiver booting...
[INFO] Thresholds: LOW=20%  HIGH=95%
[INFO] AP IP: 192.168.4.1
[INFO] HTTP server started
[INFO] NRF24 receiver listening.
[INFO] RF Channel: 108  (2508 MHz)
[RX] Dist=  127mm  Water= 73%  Temp=28.5°C  Hum=61.0%
```

---

## Troubleshooting

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| `NRF24L01 not found!` | Bad wiring or insufficient 3.3 V current | Re-check SPI pins; add **100 µF** cap on NRF VCC/GND |
| TX always shows `RF=FAIL` | Channel mismatch, address mismatch, range | Confirm both use `RF_CHANNEL=108` and `PIPE_ADDR="HTAP1"` |
| Receiver never gets data | Struct size mismatch or pipe number wrong | Ensure `SensorData` is **identical** on both; receiver uses `pipe 1` |
| HC-SR04 always returns 0 mm | Wiring / echo timeout | Check TRIG=GPIO1, ECHO=GPIO2; verify 5 V on VCC |
| DHT11 read fails | Wiring / missing pull-up | Add **10 kΩ** pull-up resistor on DATA line |
| OLED blank | Wrong I2C address | Try `0x3D` instead of `0x3C` in `display.begin()` |
| Transmitter hangs at boot | `while(!Serial)` blocking | Already removed in fixed code; re-check firmware is up to date |
| Web page not loading | Wrong IP or wrong Wi-Fi network | Connect to SSID `HeyTaps`, navigate to `192.168.4.1` |
| No "NO SIGNAL" warning | `dataReceived` never set | Happens until first successful packet — normal at startup |

---

## Project Structure

```
HeyTaps/
├── Transiver/
│   └── esp32c3.ino      ← Transmitter firmware (ESP32-C3 Super Mini)
├── Receiver/
│   └── esp32.ino        ← Receiver firmware (ESP32 38-pin DevKit)
└── README.md            ← This file
```

---

## License

This project is released under the [MIT License](LICENSE).

```
MIT License  Copyright (c) 2026 HeyTaps Contributors
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction...
```

---

<div align="center">

Made with ❤️ for the maker community · Pull requests welcome!

</div>
