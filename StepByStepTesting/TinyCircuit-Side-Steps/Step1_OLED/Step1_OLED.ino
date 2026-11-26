/*******************************************************************************
 * Step 1 - OLED Bring-Up Test
 *
 * Checks the TinyScreen+'s OLED on its own. Needs only the TinyScreen+,
 * USB powered. See ../../README.md for the expected results.
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
