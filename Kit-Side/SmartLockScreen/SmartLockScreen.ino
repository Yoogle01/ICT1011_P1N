/*******************************************************************************
 * Smart Lock-Screen - thermal + BLE + face presence
 *
 * Kit: TinyScreen+ (ATSAMD21) + Wireling Adapter + Thermal Camera Wireling
 * (AMG8833, port 1) + TOF Wireling (VL53L0X, port 0) + BLE TinyShield
 * (ST BlueNRG).
 *
 * Stays UNLOCKED while all of these hold:
 *   1. thermal cam sees a warm body OR the host companion script says the
 *      owner's face is in frame
 *   2. a phone is connected over BLE
 *   3. the host companion script (Host-Companion/presence_companion.py) is
 *      actively connected over serial
 * If any of these drop for LOCK_DELAY_MS it locks (Win+L via USB HID),
 * printing a MCU:WARN a bit earlier as a heads up.
 *
 * Can't currently tell teammates' phones apart - Android gives out a new
 * random BLE address every connection, so for now ANY BLE connection to
 * this device counts as "phone nearby" (see GAP_ConnectionComplete_CB()).
 *
 * ../../main.asm has an MSP430 assembly version of just the state machine
 * (no real sensors there, just the timing logic as a standalone demo).
 *
 * Libraries: Wireling.h, TinyScreen.h, Adafruit_AMG88xx.h, VL53L0X.h
 * (Pololu), Keyboard.h (built in), and the STBLE library (zip from
 * TinyCircuits' GitHub, not in the Library Manager).
 ******************************************************************************/

#include <Wire.h>
#include <SPI.h>
#include <TinyScreen.h>
#include <Wireling.h>
#include <Adafruit_AMG88xx.h>
#include <VL53L0X.h>
#include <Keyboard.h>
#include <string.h>
#include <STBLE.h>

// ---------------------------------------------------------------------------
// Configuration - tune these during real-world testing
// ---------------------------------------------------------------------------
const uint8_t  THERMAL_WIRELING_PORT     = 1;     // thermal cam defaults to port 1
const uint8_t  TOF_WIRELING_PORT         = 0;     // TOF sensor defaults to port 0
const float    PRESENCE_TEMP_THRESHOLD_C = 28.0;  // pixel reading above this = "warm body" (tune per room)
const uint8_t  PRESENCE_MIN_PIXELS       = 3;     // need a few adjacent warm pixels, not just noise
const int8_t   BLE_RSSI_THRESHOLD_DBM    = -75;   // rough "too far" cutoff, tune empirically
const uint32_t LOCK_DELAY_MS             = 30000; // how long presence can fail before it actually locks (30s)
const uint32_t SENSOR_POLL_INTERVAL_MS   = 1000;  // how often to re-check both sensors
const uint32_t LOCK_WARNING_LEAD_MS      = 15000; // send MCU:WARN this long before the actual lock
const uint32_t PC_SIGNAL_TIMEOUT_MS      = 5000;  // how stale a PC:FACE= reading can get before it's "absent"

// ---------------------------------------------------------------------------
// BLE owner-phone pairing configuration
// ---------------------------------------------------------------------------
// Originally tried to match a fixed phone address, but Android hands out a
// new random BLE address every connection, so that never worked reliably.
// For now, any BLE connection to this device counts as "owner present" -
// see GAP_ConnectionComplete_CB() below.
#define BLE_PAIRING_DEBUG true

// ---------------------------------------------------------------------------
// BLE globals (populated by the HCI event callbacks further down)
// ---------------------------------------------------------------------------
volatile uint8_t set_connectable = 1;
uint16_t         connection_handle = 0;
bool             ownerPhoneConnected = false;

#define ADV_INTERVAL_MIN_MS 50
#define ADV_INTERVAL_MAX_MS 100

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
TinyScreen display = TinyScreen(TinyScreenPlus);
Adafruit_AMG88xx thermalCam;
float thermalPixels[AMG88xx_PIXEL_ARRAY_SIZE];
VL53L0X distanceSensor;

uint32_t lastBothPresentTimestamp = 0;
bool     currentlyLocked          = false;

bool     lockWarningSent          = false; // one "MCU:WARN" per absence cycle

bool     pcFacePresent            = false; // latest "PC:FACE=" value received
uint32_t lastPcMessageTime        = 0;     // millis() of last valid "PC:FACE=" line

// ---------------------------------------------------------------------------
// Inline ARM assembly (ATSAMD21 / Cortex-M0+ Thumb)
// ---------------------------------------------------------------------------
// Same unsigned "has this timed out yet?" check as main.asm's MSP430
// version, just written in the Thumb instructions the ATSAMD21 actually
// runs (compiled with arm-none-eabi-gcc via the Arduino SAMD core).
static inline bool asmTimeoutElapsed(uint32_t elapsed, uint32_t timeoutMs) {
  uint32_t result;
  asm volatile (
    "cmp   %1, %2      \n\t"   // elapsed vs timeoutMs
    "bhs   1f          \n\t"   // branch if elapsed >= timeoutMs
    "movs  %0, #0      \n\t"   // not timed out
    "b     2f          \n\t"
    "1:                \n\t"
    "movs  %0, #1      \n\t"   // timed out
    "2:                \n\t"
    : "=r" (result)
    : "r" (elapsed), "r" (timeoutMs)
    : "cc"
  );
  return result != 0;
}

void setup() {
  SerialUSB.begin(115200); // native USB CDC, also used for BLE_PAIRING_DEBUG output

  // Display has to init before any showStatus() call, otherwise the error
  // paths below just print to a dead OLED and the board silently hangs.
  display.begin();
  display.setBrightness(10);
  display.setFont(thinPixel7_10ptFontInfo); // "thinFont" isn't a real symbol in the lib
  display.fontColor(TS_8b_White, TS_8b_Black);

  // sanity check so a bad config doesn't just silently never warn/lock
  if (LOCK_WARNING_LEAD_MS >= LOCK_DELAY_MS) {
    showStatus("CFG ERR");
    while (true) { /* halt */ }
  }

  // BLE pairing has to happen before Wire.begin()/Wireling.begin() - the
  // STBLE library's BNRG_SPI_Init() attaches an interrupt on pin 2, and
  // doing that after the Wireling Adapter's bus was already touched caused
  // a hard hang (stuck on "BLE: SPI INIT"). See
  // StepByStepTesting/Kit-Side-Steps/Step5b_Combined_BLE_First/ for the
  // isolated test.
  initBlePairing();

  Wire.begin();

  Wireling.begin();
  Wireling.selectPort(THERMAL_WIRELING_PORT);
  delay(100); // let the sensor boot up after the mux switches ports -
              // skipping this can make begin() fail even with good wiring

  if (!thermalCam.begin()) {
    // Thermal sensor didn't init over I2C - check the Wireling port/cable.
    showStatus("THERMAL ERR");
    while (true) { /* halt */ }
  }

  // Shared I2C bus, so re-select the TOF port before init'ing it too.
  // Distance reading itself isn't used by the lock logic right now, just
  // kept running in case it's useful later.
  Wireling.selectPort(TOF_WIRELING_PORT);
  if (!distanceSensor.init()) {
    showStatus("TOF ERR");
    while (true) { /* halt */ }
  }
  distanceSensor.setTimeout(500);
  distanceSensor.setMeasurementTimingBudget(200000); // trades speed for range/accuracy
  distanceSensor.startContinuous();

  Keyboard.begin();   // native USB HID keyboard emulation

  lastBothPresentTimestamp = millis();
  showStatus("UNLOCKED");
}

void loop() {
  // HCI_Process() pumps BLE connect/disconnect events - needs to run every
  // loop, which is why sensor checks below use millis() instead of delay().
  HCI_Process();
  if (set_connectable) {
    setConnectable();
    set_connectable = 0;
  }

  // drain host-companion serial input every loop so "PC:FACE=" stays fresh
  pollHostLink();

  static uint32_t lastSensorCheckTime = 0;
  uint32_t now = millis();
  if (now - lastSensorCheckTime < SENSOR_POLL_INTERVAL_MS) {
    return; // not time to re-check yet
  }
  lastSensorCheckTime = now;

  SerialUSB.println("DBG: before isThermalPresenceDetected()"); // TEMP DEBUG
  bool personPresent     = isThermalPresenceDetected();
  SerialUSB.println("DBG: after isThermalPresenceDetected(), before isPairedPhoneNearby()"); // TEMP DEBUG
  bool phoneNearby       = isPairedPhoneNearby();
  SerialUSB.println("DBG: after isPairedPhoneNearby()"); // TEMP DEBUG

  // laptop companion script counts as "connected" if a PC:FACE= line showed
  // up within PC_SIGNAL_TIMEOUT_MS - stale/missing just means disconnected
  bool laptopConnected   = (now - lastPcMessageTime) < PC_SIGNAL_TIMEOUT_MS;
  bool facePresent       = laptopConnected && pcFacePresent;

  // thermal or recognized face, either one is enough
  bool anyBodyPresent    = personPresent || facePresent;

  // unlocked needs all three: a body present, phone in BLE range, and the
  // laptop companion script connected
  bool ownerPresent      = anyBodyPresent && phoneNearby && laptopConnected;

  if (ownerPresent) {
    lastBothPresentTimestamp = millis();
    lockWarningSent = false;
    if (currentlyLocked) {
      currentlyLocked = false;
      showStatus("UNLOCKED");
      SerialUSB.println("MCU:STATE=UNLOCKED");
    }
  } else {
    uint32_t absentDuration = millis() - lastBothPresentTimestamp;
    if (!currentlyLocked && !lockWarningSent &&
        asmTimeoutElapsed(absentDuration, LOCK_DELAY_MS - LOCK_WARNING_LEAD_MS)) {
      SerialUSB.println("MCU:WARN"); // heads-up before the actual lock
      lockWarningSent = true;
    }
    if (!currentlyLocked && asmTimeoutElapsed(absentDuration, LOCK_DELAY_MS)) {
      lockScreen();
      currentlyLocked = true;
      showStatus("LOCKED");
      SerialUSB.println("MCU:STATE=LOCKED");
    }
  }
}

// ---------------------------------------------------------------------------
// Reads "PC:FACE=0"/"PC:FACE=1" lines from presence_companion.py over
// serial. Non-blocking, ignores anything else arriving on the port.
// ---------------------------------------------------------------------------
void pollHostLink() {
  static char    lineBuf[32];
  static uint8_t lineLen = 0;

  while (SerialUSB.available()) {
    char c = SerialUSB.read();
    if (c == '\n' || c == '\r') {
      if (lineLen > 0) {
        lineBuf[lineLen] = '\0';
        if (strncmp(lineBuf, "PC:FACE=", 8) == 0) {
          pcFacePresent     = (lineBuf[8] == '1');
          lastPcMessageTime = millis();
        }
        lineLen = 0;
      }
    } else if (lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = c;
    }
  }
}

// ---------------------------------------------------------------------------
// Thermal presence check (Thermal Camera Wireling / AMG8833)
// ---------------------------------------------------------------------------
bool isThermalPresenceDetected() {
  // shared I2C bus with the TOF sensor, re-select this port every read
  Wireling.selectPort(THERMAL_WIRELING_PORT);
  thermalCam.readPixels(thermalPixels, AMG88xx_PIXEL_ARRAY_SIZE);

  uint8_t warmPixelCount = 0;
  for (uint8_t i = 0; i < AMG88xx_PIXEL_ARRAY_SIZE; i++) {
    if (thermalPixels[i] >= PRESENCE_TEMP_THRESHOLD_C) {
      warmPixelCount++;
    }
  }
  return (warmPixelCount >= PRESENCE_MIN_PIXELS);
}

// ---------------------------------------------------------------------------
// BLE proximity check (BLE TinyShield / ST BlueNRG)
// ---------------------------------------------------------------------------
// Same GAP peripheral/advertising setup as TinyCircuits' official STBLE
// UART example, trimmed to just connection + RSSI (no GATT data service
// needed here).
// ---------------------------------------------------------------------------
void initBlePairing() {
  HCI_Init();
  BNRG_SPI_Init();
  BlueNRG_RST();

  // this device's own BLE address - any static random address works, phone
  // side doesn't need to match it
  uint8_t bdaddr[] = {0x12, 0x34, 0x00, 0xE1, 0x80, 0x02};
  aci_hal_write_config_data(CONFIG_DATA_PUBADDR_OFFSET, CONFIG_DATA_PUBADDR_LEN, bdaddr);

  aci_gatt_init();

  uint16_t service_handle, dev_name_char_handle, appearance_char_handle;
  aci_gap_init_IDB05A1(GAP_PERIPHERAL_ROLE_IDB05A1, 0, 0x07, &service_handle,
                        &dev_name_char_handle, &appearance_char_handle);

  const char *name = "LockGuard";
  aci_gatt_update_char_value(service_handle, dev_name_char_handle, 0,
                              strlen(name), (uint8_t *)name);

  aci_hal_set_tx_power_level(1, 3); // +4 dBm, same as the official example

  setConnectable();
}

void setConnectable(void) {
  const char local_name[] = {AD_TYPE_COMPLETE_LOCAL_NAME,
                              'L', 'o', 'c', 'k', 'G', 'u', 'a', 'r', 'd'};
  hci_le_set_scan_resp_data(0, NULL);
  aci_gap_set_discoverable(ADV_IND,
                            (ADV_INTERVAL_MIN_MS * 1000) / 625,
                            (ADV_INTERVAL_MAX_MS * 1000) / 625,
                            STATIC_RANDOM_ADDR, NO_WHITE_LIST_USE,
                            sizeof(local_name), local_name, 0, NULL, 0, 0);
}

bool isPairedPhoneNearby() {
  // ownerPhoneConnected is set by GAP_ConnectionComplete_CB() via
  // HCI_Process() in loop()
  if (!ownerPhoneConnected) {
    return false;
  }

  // BYPASS: hci_read_rssi() below hangs the board right after a connection
  // forms (never returns, needs a physical replug) - commented out for now,
  // so any active connection just counts as "nearby" without the RSSI
  // distance check.
  return true;

  /*
  int8_t rssi;
  int ret = hci_read_rssi(&connection_handle, &rssi);
  if (ret != 0) {
    return false; // RSSI read failed - treat as "not nearby"
  }

  return (rssi > BLE_RSSI_THRESHOLD_DBM);
  */
}

// ---------------------------------------------------------------------------
// BLE event callbacks - called from HCI_Event_CB() below via HCI_Process().
// Names/signatures match TinyCircuits' official UART.ino example.
// ---------------------------------------------------------------------------
void GAP_ConnectionComplete_CB(uint8_t addr[6], uint16_t handle) {
#if BLE_PAIRING_DEBUG
  SerialUSB.print("BLE connection from: ");
  for (int i = 5; i >= 0; i--) {
    if (addr[i] < 0x10) SerialUSB.print("0");
    SerialUSB.print(addr[i], HEX);
    if (i > 0) SerialUSB.print("-");
  }
  SerialUSB.println();
#endif

  // accepted trade-off: used to check against a fixed owner phone address,
  // but Android rotates a random BLE address per connection so that never
  // worked. Any connection is accepted as "owner present" for now - means
  // a teammate's phone would also pass if it connected to this device.
  connection_handle = handle;
  ownerPhoneConnected = true;
  SerialUSB.println("DBG: GAP_ConnectionComplete_CB() finished, handle set"); // TEMP DEBUG
}

void GAP_DisconnectionComplete_CB(void) {
  ownerPhoneConnected = false;
  set_connectable = TRUE; // make the device connectable again
}

void HCI_Event_CB(void *pckt) {
  hci_uart_pckt *hci_pckt = (hci_uart_pckt *)pckt;
  hci_event_pckt *event_pckt = (hci_event_pckt *)hci_pckt->data;

  if (hci_pckt->type != HCI_EVENT_PKT) return;

  switch (event_pckt->evt) {
    case EVT_DISCONN_COMPLETE:
      GAP_DisconnectionComplete_CB();
      break;

    case EVT_LE_META_EVENT: {
      evt_le_meta_event *evt = (evt_le_meta_event *)event_pckt->data;
      if (evt->subevent == EVT_LE_CONN_COMPLETE) {
        evt_le_connection_complete *cc = (evt_le_connection_complete *)evt->data;
        GAP_ConnectionComplete_CB(cc->peer_bdaddr, cc->handle);
      }
      break;
    }
  }
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------
void lockScreen() {
  // Windows lock-screen combo (Win + L). For macOS, use something like
  // Control+Command+Q instead - adjust per the demo machine's OS.
  Keyboard.press(KEY_LEFT_GUI);
  Keyboard.press('l');
  Keyboard.releaseAll();
}

void showStatus(const char* status) {
  display.clearScreen();
  display.setCursor(5, 25);
  display.print(status);
}
