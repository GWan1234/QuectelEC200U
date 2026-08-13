/*
  QuectelEC200U - Arduino library for Quectel EC200U
  Hardware Abstraction Layer (HAL)
  Author: misternegative21
  Maintainer: MisterNegative21 <misternegative21@gmail.com>
  Repository: https://github.com/MISTERNEGATIVE21/QuectelEC200U
  License: MIT (see LICENSE)
*/

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

// Backward compatibility aliases
#ifndef QUECTEL_SERIAL_PORT
  #define QUECTEL_SERIAL_PORT QUECTEL_HAL_SERIAL_TYPE
#endif
#ifndef QUECTEL_SERIAL_INIT
  #define QUECTEL_SERIAL_INIT QUECTEL_HAL_SERIAL_INIT
#endif
#ifndef QUECTEL_SERIAL_BEGIN
  #define QUECTEL_SERIAL_BEGIN QUECTEL_HAL_SERIAL_BEGIN
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
