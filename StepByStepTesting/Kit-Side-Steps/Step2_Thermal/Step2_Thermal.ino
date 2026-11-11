/*******************************************************************************
 * Step 2 - Thermal Camera Test
 *
 * Adds the Thermal Camera Wireling (AMG8833) on top of Step 1's OLED test.
 * Needs: TinyScreen+ + Wireling Adapter + Thermal Camera Wireling in Port 1
 * (same as THERMAL_WIRELING_PORT in the full sketch).
 *
 * Libraries: Wireling.h, Adafruit_AMG88xx.h
 *
 * Expected: if init fails, OLED shows "THERMAL ERR" and it halts. If it
 * works, OLED shows "THERMAL OK" then cycles the warm-pixel count every
 * second. Wave a hand in front of it - "WARM PX" should jump up and flip
 * the serial output to PRESENT, then back to ABSENT once you step away.
 *
 * If "THERMAL ERR" shows up: check the Wireling is seated in Port 1 (not
 * Port 0, that's for the TOF sensor), and that the adapter itself is fully
 * seated. There's a 100ms delay between selectPort() and begin() below -
 * if errors persist with correct wiring, try bumping that up.
 ******************************************************************************/

#include <SPI.h>
#include <Wire.h>
#include <TinyScreen.h>
#include <Wireling.h>
#include <Adafruit_AMG88xx.h>

const uint8_t  THERMAL_WIRELING_PORT     = 1;
const float    PRESENCE_TEMP_THRESHOLD_C = 28.0;
const uint8_t  PRESENCE_MIN_PIXELS       = 3;
const uint32_t POLL_INTERVAL_MS          = 1000;

TinyScreen display = TinyScreen(TinyScreenPlus);
Adafruit_AMG88xx thermalCam;
float thermalPixels[AMG88xx_PIXEL_ARRAY_SIZE];

void showStatus(const char* status) {
  display.clearScreen();
  display.setCursor(5, 25);
  display.print(status);
}

void setup() {
  SerialUSB.begin(115200);
  Wire.begin();

  display.begin();
  display.setBrightness(10);
  display.setFont(thinPixel7_10ptFontInfo);
  display.fontColor(TS_8b_White, TS_8b_Black);
  showStatus("OLED OK");

  Wireling.begin();
  Wireling.selectPort(THERMAL_WIRELING_PORT);
  delay(100); // let the sensor boot up after the mux switches ports

  if (!thermalCam.begin()) {
    showStatus("THERMAL ERR");
    SerialUSB.println("Step 2: thermalCam.begin() FAILED - check Wireling port/cable");
    while (true) { /* halt */ }
  }

  showStatus("THERMAL OK");
  SerialUSB.println("Step 2: THERMAL OK");
  delay(1000);
}

void loop() {
  static uint32_t lastPoll = 0;
  uint32_t now = millis();
  if (now - lastPoll < POLL_INTERVAL_MS) {
    return;
  }
  lastPoll = now;

  Wireling.selectPort(THERMAL_WIRELING_PORT);
  thermalCam.readPixels(thermalPixels, AMG88xx_PIXEL_ARRAY_SIZE);

  uint8_t warmPixelCount = 0;
  SerialUSB.print("Step 2 pixels (C): ");
  for (uint8_t i = 0; i < AMG88xx_PIXEL_ARRAY_SIZE; i++) {
    SerialUSB.print(thermalPixels[i], 1);
    SerialUSB.print(" ");
    if (thermalPixels[i] >= PRESENCE_TEMP_THRESHOLD_C) {
      warmPixelCount++;
    }
  }
  SerialUSB.println();

  bool present = (warmPixelCount >= PRESENCE_MIN_PIXELS);
  char line[24];
  snprintf(line, sizeof(line), "WARM PX: %d", warmPixelCount);
  showStatus(line);
  SerialUSB.print("Step 2: warm pixels = ");
  SerialUSB.print(warmPixelCount);
  SerialUSB.println(present ? " -> PRESENT" : " -> ABSENT");
}
