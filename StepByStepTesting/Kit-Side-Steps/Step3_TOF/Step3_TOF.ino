/*******************************************************************************
 * Step 3 - TOF Distance Sensor Test
 *
 * Adds the TOF Distance Sensor Wireling (VL53L0X) on top of Step 2, keeping
 * both sensors running together - this is what actually proves they can
 * share the Wireling Adapter's I2C mux without interfering. TOF goes in
 * Port 0, thermal stays in Port 1.
 *
 * Library: VL53L0X.h (Pololu's version - same as TinyCircuits' own tutorial)
 *
 * Note: starts with a 200ms delay for TOF startup time, same as
 * TinyCircuits' own example sketch.
 *
 * Expected: if TOF init fails, OLED shows "TOF ERR" and it halts. If both
 * work, OLED cycles "T:x D:xmm" every second (warm pixel count + distance).
 * Move your hand close to the TOF sensor and the distance should drop
 * while the thermal reading keeps working fine - confirms the two sensors
 * aren't stepping on each other over the shared bus.
 *
 * If "TOF ERR" shows up: check it's seated in Port 0 (not Port 1), and
 * that the VL53L0X library installed is actually Pololu's (there are a
 * few similarly-named ones, and setMeasurementTimingBudget()/
 * startContinuous() are specific to Pololu's API).
 ******************************************************************************/

#include <SPI.h>
#include <Wire.h>
#include <TinyScreen.h>
#include <Wireling.h>
#include <Adafruit_AMG88xx.h>
#include <VL53L0X.h>

const uint8_t  THERMAL_WIRELING_PORT     = 1;
const uint8_t  TOF_WIRELING_PORT         = 0;
const float    PRESENCE_TEMP_THRESHOLD_C = 28.0;
const uint8_t  PRESENCE_MIN_PIXELS       = 3;
const uint32_t POLL_INTERVAL_MS          = 1000;

TinyScreen display = TinyScreen(TinyScreenPlus);
Adafruit_AMG88xx thermalCam;
float thermalPixels[AMG88xx_PIXEL_ARRAY_SIZE];
VL53L0X distanceSensor;

void showStatus(const char* status) {
  display.clearScreen();
  display.setCursor(5, 25);
  display.print(status);
}

void setup() {
  delay(200); // TOF sensor startup time

  SerialUSB.begin(115200);
  Wire.begin();

  display.begin();
  display.setBrightness(10);
  display.setFont(thinPixel7_10ptFontInfo);
  display.fontColor(TS_8b_White, TS_8b_Black);
  showStatus("OLED OK");

  Wireling.begin();
  Wireling.selectPort(THERMAL_WIRELING_PORT);
  delay(100); // let the thermal sensor boot up after the mux switches ports

  if (!thermalCam.begin()) {
    showStatus("THERMAL ERR");
    SerialUSB.println("Step 3: thermalCam.begin() FAILED - check Wireling port/cable");
    while (true) { /* halt */ }
  }
  showStatus("THERMAL OK");
  SerialUSB.println("Step 3: THERMAL OK");
  delay(500);

  // shared I2C bus - re-select the TOF sensor's own port before using it
  Wireling.selectPort(TOF_WIRELING_PORT);
  if (!distanceSensor.init()) {
    showStatus("TOF ERR");
    SerialUSB.println("Step 3: distanceSensor.init() FAILED - check Wireling port/cable");
    while (true) { /* halt */ }
  }
  distanceSensor.setTimeout(500);
  distanceSensor.setMeasurementTimingBudget(200000);
  distanceSensor.startContinuous();

  showStatus("TOF OK");
  SerialUSB.println("Step 3: TOF OK");
  delay(1000);
}

void loop() {
  static uint32_t lastPoll = 0;
  uint32_t now = millis();
  if (now - lastPoll < POLL_INTERVAL_MS) {
    return;
  }
  lastPoll = now;

  // Thermal read (Port 1)
  Wireling.selectPort(THERMAL_WIRELING_PORT);
  thermalCam.readPixels(thermalPixels, AMG88xx_PIXEL_ARRAY_SIZE);
  uint8_t warmPixelCount = 0;
  for (uint8_t i = 0; i < AMG88xx_PIXEL_ARRAY_SIZE; i++) {
    if (thermalPixels[i] >= PRESENCE_TEMP_THRESHOLD_C) {
      warmPixelCount++;
    }
  }

  // TOF read (Port 0) - re-select since the thermal read above left the
  // mux on Port 1
  Wireling.selectPort(TOF_WIRELING_PORT);
  uint16_t distanceMm = distanceSensor.readRangeContinuousMillimeters();
  bool tofTimedOut = distanceSensor.timeoutOccurred();

  char line[24];
  if (tofTimedOut) {
    snprintf(line, sizeof(line), "T:%d D:TIMEOUT", warmPixelCount);
  } else {
    snprintf(line, sizeof(line), "T:%d D:%umm", warmPixelCount, distanceMm);
  }
  showStatus(line);

  SerialUSB.print("Step 3: warm pixels = ");
  SerialUSB.print(warmPixelCount);
  SerialUSB.print(", distance = ");
  if (tofTimedOut) {
    SerialUSB.println("TIMEOUT (reading failed)");
  } else {
    SerialUSB.print(distanceMm);
    SerialUSB.println("mm");
  }
}
