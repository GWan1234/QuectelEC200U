# QuectelEC200U v4.0.0 Architecture & WebUI Redesign Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor QuectelEC200U Arduino library into version 4.0.0 with a unified MCU Hardware Abstraction Layer (HAL), non-blocking AT logic engine & URC callbacks, a modern responsive Gzip-compressed WebUI Hotspot interface, and official GitHub release v4.0.0.

**Architecture:** A header-only HAL (`src/QuectelHAL.h`) normalizes Serial, GPIO, timing, and PROGMEM macros across ESP32, RP2040/RP2350, STM32, AVR, SAMD, Teensy, and Zephyr targets. `QuectelEC200U` builds upon this HAL adding non-blocking `tick()` processing, URC event callbacks, and error recovery. `WebUI_Hotspot` is revamped with glassmorphism UI assets Gzip-compressed in PROGMEM.

**Tech Stack:** C++11 (Arduino framework), HTML5, CSS3, JavaScript (Vanilla ES6), Gzip compression, GitHub CLI (`gh`).

## Global Constraints
- Target version: `4.0.0`
- Backward compatibility: Existing synchronous API signatures in `QuectelEC200U` must remain supported.
- Supported MCU architectures: ESP32, RP2040/RP2350, STM32, AVR, SAMD, Teensy, Zephyr.
- UI styling: Modern dark/light glassmorphism CSS, zero third-party external CDN runtime JS dependencies.

---

### Task 1: Hardware Abstraction Layer (`src/QuectelHAL.h`)

**Files:**
- Create: `src/QuectelHAL.h`
- Modify: `src/QuectelEC200U.h:12-45`

**Interfaces:**
- Consumes: Standard Arduino headers (`Arduino.h`, platform Serial & GPIO libraries)
- Produces: `QUECTEL_HAL_SERIAL_TYPE`, `QUECTEL_HAL_SERIAL_INIT`, `QUECTEL_HAL_SERIAL_BEGIN`, `QUECTEL_PIN_MODE`, `QUECTEL_DIGITAL_WRITE`, `QUECTEL_DIGITAL_READ`, `QUECTEL_DELAY_MS`, `QUECTEL_PROGMEM`, `quectelPowerPulse()`

- [ ] **Step 1: Create `src/QuectelHAL.h`**

Create `src/QuectelHAL.h` with complete platform abstractions for ESP32, RP2040/RP2350, STM32, AVR, SAMD, Teensy, and Zephyr. Include guards `QUECTEL_HAL_H`.

```cpp
#ifndef QUECTEL_HAL_H
#define QUECTEL_HAL_H

#include <Arduino.h>

#if defined(ARDUINO_ARCH_ESP32)
  #include <HardwareSerial.h>
  #define QUECTEL_HAL_SERIAL_TYPE HardwareSerial
  #define QUECTEL_HAL_SERIAL_INIT(name, rx, tx) HardwareSerial name(1)
  #define QUECTEL_HAL_SERIAL_BEGIN(name, baud, rx, tx) name.begin(baud, SERIAL_8N1, rx, tx)
  #define QUECTEL_DELAY_MS(ms) vTaskDelay(pdMS_TO_TICKS(ms))
#elif defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_RP2350)
  #define QUECTEL_HAL_SERIAL_TYPE SerialUART
  #define QUECTEL_HAL_SERIAL_INIT(name, rx, tx) SerialUART& name = Serial1
  #define QUECTEL_HAL_SERIAL_BEGIN(name, baud, rx, tx) do { name.setRX(rx); name.setTX(tx); name.begin(baud); } while(0)
  #define QUECTEL_DELAY_MS(ms) delay(ms)
#elif defined(ARDUINO_ARCH_STM32) || defined(ARDUINO_ARCH_SAMD) || defined(ARDUINO_ARCH_TEENSY) || defined(ARDUINO_ARCH_ZEPHYR)
  #define QUECTEL_HAL_SERIAL_TYPE HardwareSerial
  #define QUECTEL_HAL_SERIAL_INIT(name, rx, tx) HardwareSerial& name = Serial1
  #define QUECTEL_HAL_SERIAL_BEGIN(name, baud, rx, tx) name.begin(baud)
  #define QUECTEL_DELAY_MS(ms) delay(ms)
#else
  #include <SoftwareSerial.h>
  #define QUECTEL_HAL_SERIAL_TYPE SoftwareSerial
  #define QUECTEL_HAL_SERIAL_INIT(name, rx, tx) SoftwareSerial name(rx, tx)
  #define QUECTEL_HAL_SERIAL_BEGIN(name, baud, rx, tx) name.begin(baud)
  #define QUECTEL_DELAY_MS(ms) delay(ms)
#endif

#define QUECTEL_PIN_MODE(pin, mode) pinMode(pin, mode)
#define QUECTEL_DIGITAL_WRITE(pin, val) digitalWrite(pin, val)
#define QUECTEL_DIGITAL_READ(pin) digitalRead(pin)

#if defined(ARDUINO_ARCH_AVR)
  #include <avr/pgmspace.h>
  #define QUECTEL_PROGMEM PROGMEM
#else
  #define QUECTEL_PROGMEM
#endif

inline void quectelPowerPulse(int8_t pin, uint8_t activeLevel = LOW, uint32_t pulseMs = 1500) {
  if (pin < 0) return;
  QUECTEL_PIN_MODE(pin, OUTPUT);
  QUECTEL_DIGITAL_WRITE(pin, activeLevel);
  QUECTEL_DELAY_MS(pulseMs);
  QUECTEL_DIGITAL_WRITE(pin, !activeLevel);
}

#endif // QUECTEL_HAL_H
```

- [ ] **Step 2: Update `src/QuectelEC200U.h` to include `QuectelHAL.h`**

Replace legacy serial macros in `src/QuectelEC200U.h` with `#include "QuectelHAL.h"`.

- [ ] **Step 3: Test HAL inclusion and clean compilation**

Check syntax and header integrity.

- [ ] **Step 4: Commit Task 1**

```bash
git add src/QuectelHAL.h src/QuectelEC200U.h
git commit -m "feat(hal): introduce QuectelHAL header for multi-MCU abstraction"
```

---

### Task 2: Core Logic Engine & URC Event Callbacks (`src/QuectelEC200U.h` & `src/QuectelEC200U.cpp`)

**Files:**
- Modify: `src/QuectelEC200U.h`
- Modify: `src/QuectelEC200U.cpp`

**Interfaces:**
- Consumes: `QuectelHAL.h`
- Produces: `modem.tick()`, `onNetworkStatus()`, `onSMSReceived()`, `onMQTTData()`, `onCallStatus()`, `setDebugStream()`, `setCommandTimeout()`, `setRetryCount()`

- [ ] **Step 1: Declare URC Callbacks and Async Engine in `src/QuectelEC200U.h`**

Add callback type definitions and non-blocking `tick()` method to `QuectelEC200U` class:

```cpp
typedef void (*NetworkStatusCallback)(const char* status);
typedef void (*SMSReceivedCallback)(const char* sender, const char* timestamp, const char* message);
typedef void (*MQTTDataCallback)(const char* topic, const char* payload);
typedef void (*CallStatusCallback)(const char* number, const char* state);

// In class QuectelEC200U:
public:
  void tick();
  void onNetworkStatus(NetworkStatusCallback cb) { _netCb = cb; }
  void onSMSReceived(SMSReceivedCallback cb) { _smsCb = cb; }
  void onMQTTData(MQTTDataCallback cb) { _mqttCb = cb; }
  void onCallStatus(CallStatusCallback cb) { _callCb = cb; }
  void setDebugStream(Stream* stream) { _debugStream = stream; }
  void setCommandTimeout(uint32_t timeoutMs) { _cmdTimeoutMs = timeoutMs; }
  void setRetryCount(uint8_t retries) { _maxRetries = retries; }
```

- [ ] **Step 2: Implement `tick()` & URC Parser in `src/QuectelEC200U.cpp`**

Implement `tick()` routine in `src/QuectelEC200U.cpp` to continuously monitor incoming serial data, match URC patterns (`+CREG:`, `+CMTI:`, `+QMTRECV:`, `+CLIP:`, `RING`), and dispatch configured callbacks.

- [ ] **Step 3: Add Diagnostic Stream Logging**

In `sendAT` / `sendATResponse` functions inside `src/QuectelEC200U.cpp`, mirror AT commands and modem responses to `_debugStream` if `_debugStream != nullptr`.

- [ ] **Step 4: Verify C++ syntax**

- [ ] **Step 5: Commit Task 2**

```bash
git add src/QuectelEC200U.h src/QuectelEC200U.cpp
git commit -m "feat(core): add non-blocking tick loop, URC callbacks, and diagnostic logging"
```

---

### Task 3: Revamped WebUI Hotspot Interface (`examples/WebUI_Hotspot`)

**Files:**
- Modify: `examples/WebUI_Hotspot/index_html.h`
- Modify: `examples/WebUI_Hotspot/WebUI_Hotspot.ino`

**Interfaces:**
- Consumes: `QuectelHAL.h`, `QuectelEC200U`
- Produces: Gzip-compressed dashboard HTML asset `index_html_gz`, unified WebUI hotspot sketch.

- [ ] **Step 1: Create Modern Glassmorphism WebUI in `index_html.h`**

Create responsive single-page web dashboard in `examples/WebUI_Hotspot/index_html.h` containing:
- Dark/Light modern glass UI CSS.
- Real-time telemetry cards (Signal RSSI, Network Operator, PDP context, GNSS status, SIM state, Connected Clients).
- Interactive Web AT Command Terminal with history and quick-action presets.
- SMS hub (inbox view & composer).
- APN & MQTT configuration panel.

- [ ] **Step 2: Gzip Compress Asset and Define `index_html_gz` in `index_html.h`**

Compress HTML/CSS/JS payload into a PROGMEM byte array `const uint8_t index_html_gz[] PROGMEM` and store `index_html_gz_len`.

- [ ] **Step 3: Update `WebUI_Hotspot.ino` Server Handlers & HAL Integration**

Update `WebUI_Hotspot.ino`:
- Include `src/QuectelHAL.h` for cross-platform MCU initialization.
- Serve root HTTP request `/` with `server.sendHeader("Content-Encoding", "gzip"); server.send_P(200, "text/html", (const char*)index_html_gz, index_html_gz_len);`.
- Handle API routes (`/api/status`, `/api/at`, `/api/sms`, `/api/apn`, `/api/mqtt`).

- [ ] **Step 4: Test WebUI files integrity**

- [ ] **Step 5: Commit Task 3**

```bash
git add examples/WebUI_Hotspot/index_html.h examples/WebUI_Hotspot/WebUI_Hotspot.ino
git commit -m "feat(webui): revamp hotspot UI with modern dark dashboard and gzip compression"
```

---

### Task 4: Library Properties, Documentation & GitHub Release v4.0.0

**Files:**
- Modify: `library.properties`
- Modify: `CHANGELOG.md`
- Modify: `README.md`

**Interfaces:**
- Consumes: Complete codebase
- Produces: Updated release docs, git tag `v4.0.0`, GitHub Release.

- [ ] **Step 1: Update `library.properties` version to 4.0.0**

Set `version=4.0.0` in `library.properties`.

- [ ] **Step 2: Update `CHANGELOG.md` with v4.0.0 Notes**

Add `## [4.0.0] - 2026-08-13` section listing all HAL, core engine, URC, and WebUI features.

- [ ] **Step 3: Update `README.md`**

Add details for `QuectelHAL.h`, URC callback examples, supported MCUs table, and WebUI screenshot/description.

- [ ] **Step 4: Commit release documentation**

```bash
git add library.properties CHANGELOG.md README.md
git commit -m "chore: release version 4.0.0"
```

- [ ] **Step 5: Tag & Publish GitHub Release v4.0.0**

```bash
git tag -a v4.0.0 -m "Release v4.0.0 - Unified MCU HAL, Core Async Engine, and Revamped WebUI"
git push origin main --tags
gh release create v4.0.0 --title "v4.0.0 - Universal MCU HAL & WebUI Overhaul" --notes-file CHANGELOG.md
```
