/*
  PPPoS Demo for Quectel EC200U

  This example demonstrates how to set up PPP (Point-to-Point Protocol over Serial)
  using the Quectel EC200U modem with unified MCU Hardware Abstraction Layer.
*/

#include <QuectelEC200U.h>
#include <QuectelHAL.h>

#define EC200U_RX 18
#define EC200U_TX 17
#define PW_KEY    10

QUECTEL_HAL_SERIAL_INIT(modemSerial, EC200U_RX, EC200U_TX);
QuectelEC200U modem(modemSerial);

const char* APN = "internet";

void setup() {
  Serial.begin(115200);
  quectelPowerPulse(PW_KEY);
  QUECTEL_HAL_SERIAL_BEGIN(modemSerial, 115200, EC200U_RX, EC200U_TX);

  Serial.println("Initializing Quectel EC200U for PPPoS...");
  if (modem.begin()) {
    Serial.println("Modem ready.");
  } else {
    Serial.println("Modem initialization failed!");
    return;
  }

  Serial.println("Setting APN...");
  modem.setAPN(APN);

  Serial.println("Entering PPP mode (ATD*99#)...");
  if (modem.sendAT("ATD*99#", "CONNECT", 10000)) {
    Serial.println("PPP Connection Established! Serial stream switched to PPP data mode.");
  } else {
    Serial.println("PPP dial failed.");
  }
}

void loop() {
  modem.tick();
  delay(1000);
}
