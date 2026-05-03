<div align="center">

# 💧 HeyTaps — Secure Wireless Water Tank Monitor

[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![Arduino](https://img.shields.io/badge/Platform-Arduino%20%7C%20ESP32-blue?logo=arduino)](https://www.arduino.cc/)
[![NRF24L01](https://img.shields.io/badge/Radio-NRF24L01%2B-orange)](https://www.nordicsemi.com/)
[![HC-SR04](https://img.shields.io/badge/Sensor-HC--SR04%20Ultrasonic-9cf)](https://cdn.sparkfun.com/datasheets/Sensors/Proximity/HCSR04.pdf)
[![Status](https://img.shields.io/badge/Status-Production%20Ready-brightgreen)]()

> **HeyTaps** is a low-cost, wireless water-tank level monitoring system built on two ESP32 boards communicating over 2.4 GHz NRF24L01+ radio. The system features a **2-Way E2E Handshake Protocol** with strictly unique MAC-based hardware addressing and dynamic pipe routing, ensuring interference-free communication even with identical models nearby.

</div>

---

## 📋 Table of Contents

- [System Architecture](#system-architecture)
- [Hardware Components](#hardware-components)
- [Pin Connection Tables](#pin-connection-tables)
  - [Transceiver (T1) – ESP32 (38-pin DevKit)](#transceiver-t1--esp32-38-pin-devkit)
  - [Receiver (R1) – ESP32 (38-pin DevKit)](#receiver-r1--esp32-38-pin-devkit)
- [Security & Handshake](#security--handshake)
- [Initial Setup (Pairing)](#initial-setup-pairing)
- [Web Dashboards](#web-dashboards)
- [Tank Calibration](#tank-calibration)
- [Software Setup](#software-setup)
- [Project Structure](#project-structure)

---

## System Architecture

```
┌──────────────────────────────────┐        E2E Encrypted Handshake        ┌────────────────────────────────────────┐
│        TRANSCEIVER (T1)          │ ─────────────────────────────────▶ │           RECEIVER (R1)                │
│       ESP32 38-pin DevKit        │ ◀───────────────────────────────── │         ESP32 38-pin DevKit            │
│        AP: "HeyTaps T1"          │      250 kbps | Dynamic Pipes      │          AP: "HeyTap R1"               │
│                                  │                                     │                                        │
│  ┌─────────────┐                 │                                     │  ┌──────────────┐  ┌──────────────┐   │
│  │  HC-SR04    │ Distance(mm)    │  DataPacket {                       │  │  SSD1306     │  │ Web Portals  │   │
│  │  Ultrasonic │────────────────▶│    uint32_t senderId;               │  │  OLED 128×64 │  │ Setup & Docs │   │
│  └─────────────┘                 │    uint32_t targetId;               │  └──────────────┘  └──────────────┘   │
│  ┌─────────────┐                 │    float temperature;               │  ┌──────────────┐  ┌──────────────┐   │
│  │   DHT11     │ Temp + Hum ─────│    float humidity;                  │  │  4× LEDs     │  │   Buzzer     │   │
│  └─────────────┘                 │    uint16_t distance_mm;            │  │  R/Y/G/B     │  │   Alarm      │   │
│  ┌─────────────┐                 │    uint8_t msgType;                 │  └──────────────┘  └──────────────┘   │
│  │ SSD1306 OLED│ ◀─── ACK ───────│                                     └────────────────────────────────────────┘
│  │ & 4× LEDs   │                 │  }                                  
└──────────────────────────────────┘                                     
```

---

## Hardware Components

| Qty | Component | Role |
|-----|-----------|------|
| 2 | **ESP32 38-pin DevKit** (or Wroom-32) | Transceiver (T1) & Receiver (R1) |
| 2 | **NRF24L01+ with PA+LNA** | 2.4 GHz wireless link |
| 1 | **HC-SR04** Ultrasonic Module | Distance measurement (tank level) |
| 1 | **DHT11** Temperature & Humidity Sensor | Environmental sensing |
| 2 | **SSD1306 OLED** 128×64 I2C | System display (Both T1 & R1) |
| 8 | **5mm LEDs** (Red / Yellow / Green / Blue) | Water level indicators (4 per board) |
| 8 | **220 Ω resistors** | LED current limiting |
| 1 | **5V Active Buzzer** | Alarm output (R1) |
| 2 | **100 µF electrolytic capacitors** | NRF24L01 power decoupling |

> **⚡ Power Note:** The NRF24L01+ module runs on **3.3 V only**. Place a **100 µF capacitor** across its VCC and GND pins as close to the module as possible to prevent voltage drooping during transmission bursts.

---

## Pin Connection Tables

### Transceiver (T1) – ESP32 (38-pin DevKit)

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

#### HC-SR04 Ultrasonic → ESP32
| HC-SR04 Pin | ESP32 Pin | Notes |
|-------------|-----------|-------|
| VCC | **5V** (VIN/VBUS)| Requires 5 V supply |
| GND | GND | Common ground |
| TRIG | GPIO **12** | Trigger pulse output |
| ECHO | GPIO **14** | PWM echo input |

#### DHT11 → ESP32
| DHT11 Pin | ESP32 Pin | Notes |
|-----------|-----------|-------|
| VCC | 3V3 | Also works at 5 V |
| GND | GND | — |
| DATA | GPIO **27** | 10 kΩ pull-up to VCC recommended |

#### SSD1306 OLED → ESP32
| OLED Pin | ESP32 Pin | Notes |
|----------|-----------|-------|
| VCC | 3V3 | — |
| GND | GND | — |
| SDA | GPIO **21** | I2C Data (default) |
| SCL | GPIO **22** | I2C Clock (default) |

#### LEDs → ESP32
| Component | ESP32 Pin | Notes |
|-----------|-----------|-------|
| 🔴 Red LED | GPIO **32** | Critical (< 25%) |
| 🟡 Yellow LED | GPIO **33** | Low (25–49%) |
| 🟢 Green LED | GPIO **25** | Good (50–74%) |
| 🔵 Blue LED | GPIO **26** | Full (≥ 75%) |

---

### Receiver (R1) – ESP32 (38-pin DevKit)

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

#### SSD1306 OLED → ESP32
| OLED Pin | ESP32 Pin | Notes |
|----------|-----------|-------|
| VCC | 3V3 | — |
| GND | GND | — |
| SDA | GPIO **21** | I2C Data (default) |
| SCL | GPIO **22** | I2C Clock (default) |

#### LEDs & Buzzer → ESP32
| Component | ESP32 Pin | Notes |
|-----------|-----------|-------|
| 🔴 Red LED | GPIO **32** | Critical (< 25%) |
| 🟡 Yellow LED | GPIO **33** | Low (25–49%) |
| 🟢 Green LED | GPIO **25** | Good (50–74%) |
| 🔵 Blue LED | GPIO **26** | Full (≥ 75%) |
| 🔊 Buzzer + | GPIO **27** | Active buzzer |
| 🔲 Mute Button | GPIO **14** | Connects to GND (INPUT_PULLUP) |

---

## Security & Handshake

HeyTaps uses a **2-Way E2E (End-to-End) Handshake** to ensure maximum reliability and isolation.
1. **Unique Identification:** Both ESP32s hash their internal MAC address into a unique, fixed 8-character ID.
2. **Dynamic Pipe Routing:** Using the unique IDs, the NRF24 modules open dynamically calculated reading/writing pipes. This provides hardware-level filtering where a device will physically ignore packets not addressed to its unique ID.
3. **Application ACK:** T1 transmits an encrypted `DATA` packet containing both `Sender ID` and `Target ID`. R1 receives, verifies both IDs, and replies immediately with an encrypted `ACK` packet. T1 logs the transmission as a success only if the ACK is verified.

This ensures **zero interference**, even if your neighbors deploy the exact same HeyTaps system.

---

## Initial Setup (Pairing)

On first boot, the devices require pairing:
1. Power up **Transceiver (T1)** and **Receiver (R1)**.
2. Connect to the Transceiver's Wi-Fi network:
   - **SSID:** `HeyTaps T1`
   - **Password:** `savewater`
3. Connect to the Receiver's Wi-Fi network:
   - **SSID:** `HeyTap R1`
   - **Password:** `savewater`
4. Navigate to `192.168.4.1` on each network.
5. You will see a unique **8-character Device Code** generated on each portal.
6. Swap codes: Enter T1's code on R1's portal, and R1's code on T1's portal.
7. Both devices will lock in, open their secure hardware pipes, and begin communicating.

---

## Web Dashboards

Both the Receiver and Transceiver host embedded, responsive Web Dashboards accessible at `192.168.4.1`.
- **R1 Dashboard:** Displays real-time sensor measurements, system connection statuses, tank calibration menus, and password settings.
- **T1 Dashboard:** Displays pairing status, packet transmission counts, and password settings.
- **Documentation:** The Receiver hosts a beautiful `/docs` page detailing the system architecture right on the device.
- **Hard Reset:** Both dashboards include a secure "Hard Reset" button to erase the NVS memory and clear pairings.

---

## Tank Calibration

Tank calibration is set from the **Receiver Web Dashboard** — no need to reflash firmware.
1. With the tank **completely empty**, measure the distance from the HC-SR04 to the tank bottom → enter as **Empty Distance (cm)**
2. With the tank **completely full**, measure the distance from the HC-SR04 to the water surface → enter as **Full Distance (cm)**
3. Click **Save Calibration** — values are stored in flash memory.

---

## Software Setup

### Required Libraries
Install via **Arduino IDE → Tools → Manage Libraries**:
- `RF24` by TMRh20
- `DHT sensor library` by Adafruit
- `Adafruit GFX Library` by Adafruit
- `Adafruit SSD1306` by Adafruit

### Board Definitions
Ensure you have the `esp32 by Espressif Systems` board package installed.
- **Transceiver:** Select `ESP32 Dev Module`
- **Receiver:** Select `ESP32 Dev Module`

---

## Project Structure

```
HeyTaps/
├── Transceiver/
│   └── esp32_T1_v1.ino   ← Transceiver firmware (ESP32)
├── Receiver/
│   └── esp32_R1_v1.ino   ← Receiver firmware (ESP32)
└── README.md             ← This documentation file
```

---

<div align="center">
Made with ❤️ for the maker community · Pull requests welcome!
</div>
