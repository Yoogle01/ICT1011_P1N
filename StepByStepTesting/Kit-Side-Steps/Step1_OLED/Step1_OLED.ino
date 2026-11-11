/*******************************************************************************
 * Step 1 - OLED Bring-Up Test
 *
 * Just checks the TinyScreen+'s OLED works on its own before adding any
 * sensors/BLE on top. Only needs the TinyScreen+ itself, USB powered.
 *
 * Expected: OLED shows "OLED OK" within ~1s, Serial Monitor (115200 baud)
 * prints "Step 1: OLED OK" once.
 *
 * If the screen stays black: double check Board/Port are set right and the
 * upload actually succeeded (not just compiled) - if it still won't light
 * up, it's probably a hardware/seating issue, not firmware.
 ******************************************************************************/

#include <SPI.h>
#include <Wire.h>
#include <TinyScreen.h>

TinyScreen display = TinyScreen(TinyScreenPlus);

void setup() {
  SerialUSB.begin(115200);

  display.begin();
  display.setBrightness(10);
  display.setFont(thinPixel7_10ptFontInfo);
  display.fontColor(TS_8b_White, TS_8b_Black);
  display.clearScreen();
  display.setCursor(5, 25);
  display.print("OLED OK");

  SerialUSB.println("Step 1: OLED OK");
}

void loop() {
  // Nothing to do - the static "OLED OK" message is the whole test.
}
