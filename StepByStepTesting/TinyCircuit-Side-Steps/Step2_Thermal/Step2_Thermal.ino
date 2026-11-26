/*******************************************************************************
 * Step 2 - Thermal Camera Test
 *
 * Adds the Thermal Camera Wireling (AMG8833) on top of Step 1. Needs the
 * Wireling Adapter with the thermal camera in Port 1.
 * Libraries: Wireling, Adafruit_AMG88xx.
 * See ../../README.md for the expected results and troubleshooting.
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

// --- Thermal heatmap display layout ---
// The AMG8833 is 8x8; the 96x64 OLED draws each cell as a 12x6 block,
// leaving a 16px strip at the bottom for the "WARM PX" text.
const uint8_t THERMAL_GRID_SIZE   = 8;
const uint8_t THERMAL_CELL_WIDTH  = 12;
const uint8_t THERMAL_CELL_HEIGHT = 6;
const bool    THERMAL_FLIP_X      = false; // set true if the image is mirrored left/right
const bool    THERMAL_FLIP_Y      = false; // set true if the image is upside down

// Bucket the temperature range into named colors for a simple
// cold (blue) -> hot (red) heatmap.
const float THERMAL_COLOR_MIN_C = 20.0; // treated as "coldest" -> dark blue
const float THERMAL_COLOR_MAX_C = 36.0; // treated as "hottest" -> red (skin is ~30-34C)

TinyScreen display = TinyScreen(TinyScreenPlus);
Adafruit_AMG88xx thermalCam;
float thermalPixels[AMG88xx_PIXEL_ARRAY_SIZE];

void showStatus(const char* status) {
  display.clearScreen();
  display.setCursor(5, 25);
  display.print(status);
}

// Maps a temperature reading to a coarse heatmap color.
uint8_t thermalColorForTemp(float tempC) {
  float t = (tempC - THERMAL_COLOR_MIN_C) / (THERMAL_COLOR_MAX_C - THERMAL_COLOR_MIN_C);
  if (t < 0.0) t = 0.0;
  if (t > 1.0) t = 1.0;
  if (t < 0.17) return TS_8b_DarkBlue;
  if (t < 0.34) return TS_8b_Blue;
  if (t < 0.50) return TS_8b_DarkGreen;
  if (t < 0.67) return TS_8b_Green;
  if (t < 0.84) return TS_8b_Yellow;
  return TS_8b_Red;
}

// Draw the 8x8 reading onto the OLED as a grid of colored blocks.
void drawThermalImage(const float* pixels) {
  for (uint8_t row = 0; row < THERMAL_GRID_SIZE; row++) {
    for (uint8_t col = 0; col < THERMAL_GRID_SIZE; col++) {
      uint8_t color = thermalColorForTemp(pixels[row * THERMAL_GRID_SIZE + col]);
      uint8_t drawCol = THERMAL_FLIP_X ? (THERMAL_GRID_SIZE - 1 - col) : col;
      uint8_t drawRow = THERMAL_FLIP_Y ? (THERMAL_GRID_SIZE - 1 - row) : row;
      display.drawRect(drawCol * THERMAL_CELL_WIDTH, drawRow * THERMAL_CELL_HEIGHT,
                        THERMAL_CELL_WIDTH, THERMAL_CELL_HEIGHT, TSRectangleFilled, color);
    }
  }
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
  delay(100); // Let the sensor boot up after the mux switches ports.

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

  display.clearScreen();
  drawThermalImage(thermalPixels);
  char line[24];
  snprintf(line, sizeof(line), "WARM PX: %d", warmPixelCount);
  display.setCursor(5, 50);
  display.print(line);

  SerialUSB.print("Step 2: warm pixels = ");
  SerialUSB.print(warmPixelCount);
  SerialUSB.println(present ? " -> PRESENT" : " -> ABSENT");
}
