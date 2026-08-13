# QuectelEC200U v4.0.0 Architecture & WebUI Redesign Spec

## Overview
This document specifies the architecture, hardware abstraction layer (HAL), core logic refactoring, and WebUI UX overhaul for version **4.0.0** of the **QuectelEC200U** Arduino library.

The release aims to unify system calls across all supported microcontroller platforms, provide robust non-blocking AT processing and URC callbacks, modernize the WebUI Hotspot dashboard, and prepare the project for the `v4.0.0` release.

---

## 1. Hardware Abstraction Layer (`src/QuectelHAL.h`)

### 1.1 Scope & MCU Target Support
`QuectelHAL.h` provides a unified, single-header abstraction for hardware serial ports, software serial fallback, pin control, timing loops, and memory macros across:
- **ESP32 series**: ESP32, ESP32-S2, ESP32-S3, ESP32-C3, ESP32-C6
- **RP2040 & RP2350**: Raspberry Pi Pico, Pico W, Pico 2, Pico 2 W
- **STM32**: STM32F1, STM32F4, etc.
- **AVR**: Arduino Uno, Mega 2560, Nano
- **SAMD**: SAMD21, SAMD51
- **Teensy**: Teensy 3.x, 4.x
- **Zephyr RTOS**: Arduino Zephyr target environment

### 1.2 Unified Serial Abstraction
- `QUECTEL_HAL_SERIAL_TYPE`: HardwareSerial, SerialUART, or SoftwareSerial depending on architecture.
- `QUECTEL_HAL_SERIAL_INIT(name, rx, tx)`: Instantiates the serial object cleanly across platforms.
- `QUECTEL_HAL_SERIAL_BEGIN(name, baud, rx, tx)`: Configures baud rate and pin mapping uniformly.

### 1.3 GPIO, Timing & Watchdog Protection
- `QUECTEL_PIN_MODE(pin, mode)`, `QUECTEL_DIGITAL_WRITE(pin, val)`, `QUECTEL_DIGITAL_READ(pin)`.
- `QUECTEL_DELAY_MS(ms)`: Invokes `vTaskDelay` or `yield()` on RTOS target boards to prevent task watchdog (WDT) resets during waiting loops.
- `QUECTEL_PROGMEM`: Standardizes flash memory macro usage across AVR, ARM Cortex-M, and Xtensa architectures.
- `quectelPowerPulse(pin, activeLevel, pulseMs)`: Helper for non-blocking or safe PWRKEY / RESET hardware pulsing.

---

## 2. Core Logic & Async Engine (`QuectelEC200U.h` / `QuectelEC200U.cpp`)

### 2.1 State Machine Engine & Non-Blocking Polling
- `modem.tick()`: Non-blocking routine to read serial buffer, process incoming URC responses, and maintain active AT transactions.
- Synchronous API methods (`sendAT`, `getSignalQuality`, `httpPOST`, `mqttPublish`) remain 100% backward compatible but utilize the unified HAL layer underneath.

### 2.2 Unsolicited Result Code (URC) Callback System
Provides callback registration interface for asynchronous modem events:
- `onNetworkStatus(NetworkCallback cb)`: Triggered when register status (+CREG / +CGREG / +CEREG) changes.
- `onSMSReceived(SMSCallback cb)`: Triggered on incoming SMS notification (+CMTI / +CMGR).
- `onMQTTData(MQTTCallback cb)`: Triggered on incoming MQTT topic payloads (+QMTRECV).
- `onCallStatus(CallCallback cb)`: Triggered on voice call ring/hangup (+CLIP / RING / NO CARRIER).

### 2.3 Error Management & Diagnostic Logging
- Complete `ErrorCode` enum mapping all failure domains (Modem response, SIM state, GPRS, PDP, HTTP, MQTT, SSL, Filesystem).
- Intelligent retry policy (`setRetryCount()`, `setCommandTimeout()`).
- Optional diagnostic stream (`setDebugStream(&Serial)`) for logging raw AT activity.

---

## 3. Revamped WebUI Hotspot & UX (`examples/WebUI_Hotspot`)

### 3.1 Interface Design (`index_html.h`)
- Responsive dark/light theme dashboard built with HTML5, CSS3, and JavaScript.
- **Telemetry Cards**: Signal RSSI/BER meter, Network Operator name, PDP context state, GNSS position data, SIM card readiness, Wi-Fi AP client count.
- **Interactive Web AT Terminal**: Live output feed, history buffer, fast-access command buttons.
- **SMS Hub**: Read inbox list, compose and send SMS messages directly via browser.
- **APN & MQTT Configuration**: Full interface for setting cellular APN credentials and testing MQTT pub/sub brokers.

### 3.2 Flash & HTTP Performance Optimization
- Gzip-compressed asset storage (`index_html_gz`) stored in `PROGMEM`.
- HTTP server sends `Content-Encoding: gzip` header, reducing flash usage by ~70% and accelerating page loads.

### 3.3 Microcontroller Portability
- `WebUI_Hotspot.ino` rewritten to use `QuectelHAL.h` macros, ensuring direct compilation on ESP32, Pico W, and other target boards.

---

## 4. Release & Versioning Specifications (v4.0.0)

### 4.1 Documentation & Metadata Updates
- Bump version to `4.0.0` in `library.properties`.
- Document all v4.0.0 additions and breaking/non-breaking updates in `CHANGELOG.md`.
- Update `README.md` features matrix, supported microcontrollers table, and code snippets.

### 4.2 Git Release & Tagging
- Commit changes with descriptive commit messages.
- Create git tag `v4.0.0`.
- Execute `gh release create v4.0.0` with formatted release notes and assets.
