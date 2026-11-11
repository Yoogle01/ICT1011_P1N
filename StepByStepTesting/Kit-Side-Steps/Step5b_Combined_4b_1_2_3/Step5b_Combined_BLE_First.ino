/*******************************************************************************
 * Step 5b - Combined, BLE-first init order (variant of
 * Step5_Combined_1_2_3_4b)
 *
 * Step5_Combined_1_2_3_4b inits in order OLED -> Thermal -> TOF -> BLE
 * (BLE last, after the Wireling Adapter's bus is already touched), and
 * that file's header predicts this is likely to hang from a pin conflict
 * between BLE's interrupt line and the Wireling Adapter on pin 2.
 *
 * This variant moves BLE init to right after the OLED, strictly before
 * Wire.begin()/Wireling.begin() touch anything - testing whether the hang
 * depends on init order, or is a fixed pin conflict regardless of order
 * (in which case this should still hang, just at a different point).
 *
 * Hardware: same as Step5_Combined_1_2_3_4b.
 *
 * No logic changed from that file - same OLED/Thermal/TOF/BLE code, just
 * reordered in setup(), with the print prefix changed to "Step 5b:" so the
 * two files' serial output can be told apart.
 *
 * If BLE-first avoids the hang: OLED should progress all the way through
 * to THERMAL OK/TOF OK/combined status, serial stays connected throughout.
 * If it still hangs (likely right around Wire.begin()/Wireling.begin()):
 * that points at the hardware fix on the BLE TinyShield (moving its
 * interrupt off pin 2) as the real next step.
 ******************************************************************************/

#include <SPI.h>
#include <Wire.h>
#include <TinyScreen.h>
#include <Wireling.h>
#include <Adafruit_AMG88xx.h>
#include <VL53L0X.h>
#include <string.h>
#include <STBLE.h>

const uint8_t  THERMAL_WIRELING_PORT     = 1;
const uint8_t  TOF_WIRELING_PORT         = 0;
const float    PRESENCE_TEMP_THRESHOLD_C = 28.0;
const uint8_t  PRESENCE_MIN_PIXELS       = 3;
const uint32_t POLL_INTERVAL_MS          = 1000;

#define ADV_INTERVAL_MIN_MS 50
#define ADV_INTERVAL_MAX_MS 100
#define BLE_PAIRING_DEBUG true

TinyScreen display = TinyScreen(TinyScreenPlus);
Adafruit_AMG88xx thermalCam;
float thermalPixels[AMG88xx_PIXEL_ARRAY_SIZE];
VL53L0X distanceSensor;

volatile uint8_t set_connectable = 1;
uint16_t         connection_handle = 0;
bool             ownerPhoneConnected = false;

void showStatus(const char* status) {
  display.clearScreen();
  display.setCursor(5, 25);
  display.print(status);
}

// ---------------------------------------------------------------------------
// BLE setup/callbacks - same as Step 4b/Step5_Combined, just called at a
// different point in setup() in this variant.
// ---------------------------------------------------------------------------
void initBlePairing() {
  SerialUSB.println("Step 5b: HCI_Init()...");
  HCI_Init();
  SerialUSB.println("Step 5b: HCI_Init() done");

  showStatus("BLE: SPI INIT");
  SerialUSB.println("Step 5b: BNRG_SPI_Init()...");
  BNRG_SPI_Init();
  SerialUSB.println("Step 5b: BNRG_SPI_Init() done");

  SerialUSB.println("Step 5b: BlueNRG_RST()...");
  BlueNRG_RST();
  SerialUSB.println("Step 5b: BlueNRG_RST() done");

  uint8_t bdaddr[] = {0x12, 0x34, 0x00, 0xE1, 0x80, 0x02};
  showStatus("BLE: CONFIG");
  tBleStatus ret = aci_hal_write_config_data(CONFIG_DATA_PUBADDR_OFFSET, CONFIG_DATA_PUBADDR_LEN, bdaddr);
  SerialUSB.print("Step 5b: aci_hal_write_config_data() returned ");
  SerialUSB.println(ret);
  if (ret) {
    showStatus("BLE CFG ERR");
    SerialUSB.println("Step 5b: HALT - aci_hal_write_config_data failed");
    while (true) { /* halt */ }
  }

  showStatus("BLE: GATT INIT");
  ret = aci_gatt_init();
  SerialUSB.print("Step 5b: aci_gatt_init() returned ");
  SerialUSB.println(ret);
  if (ret) {
    showStatus("BLE GATT ERR");
    SerialUSB.println("Step 5b: HALT - aci_gatt_init failed");
    while (true) { /* halt */ }
  }

  showStatus("BLE: GAP INIT");
  uint16_t service_handle, dev_name_char_handle, appearance_char_handle;
  ret = aci_gap_init_IDB05A1(GAP_PERIPHERAL_ROLE_IDB05A1, 0, 0x07, &service_handle,
                             &dev_name_char_handle, &appearance_char_handle);
  SerialUSB.print("Step 5b: aci_gap_init_IDB05A1() returned ");
  SerialUSB.println(ret);
  if (ret) {
    showStatus("BLE GAP ERR");
    SerialUSB.println("Step 5b: HALT - aci_gap_init_IDB05A1 failed");
    while (true) { /* halt */ }
  }

  const char *name = "LockGuard";
  aci_gatt_update_char_value(service_handle, dev_name_char_handle, 0,
                              strlen(name), (uint8_t *)name);
  aci_hal_set_tx_power_level(1, 3);

  showStatus("BLE: ADV SETUP");
  setConnectable();
  SerialUSB.println("Step 5b: initBlePairing() complete");
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

bool isPairedPhoneNearby(int8_t *rssiOut) {
  if (!ownerPhoneConnected) {
    return false;
  }
  int8_t rssi;
  int ret = hci_read_rssi(&connection_handle, &rssi);
  if (ret != 0) {
    return false;
  }
  *rssiOut = rssi;
  return true;
}

void GAP_ConnectionComplete_CB(uint8_t addr[6], uint16_t handle) {
#if BLE_PAIRING_DEBUG
  SerialUSB.print("Step 5b: BLE connection from: ");
  for (int i = 5; i >= 0; i--) {
    if (addr[i] < 0x10) SerialUSB.print("0");
    SerialUSB.print(addr[i], HEX);
    if (i > 0) SerialUSB.print("-");
  }
  SerialUSB.println();
#endif
  connection_handle = handle;
  ownerPhoneConnected = true;
}

void GAP_DisconnectionComplete_CB(void) {
  ownerPhoneConnected = false;
  set_connectable = TRUE;
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

void setup() {
  SerialUSB.begin(115200);

  // --- Step 1: OLED (kept first - needed so the BLE status messages below
  // can actually be shown on screen; OLED init doesn't touch Wire/Wireling
  // so it's unaffected by this reordering experiment either way) ---
  display.begin();
  display.setBrightness(10);
  display.setFont(thinPixel7_10ptFontInfo);
  display.fontColor(TS_8b_White, TS_8b_Black);
  showStatus("OLED OK");
  SerialUSB.println("Step 5b: OLED OK");
  delay(500);

  // --- Step 4b: BLE (standard library, interrupt-based) - MOVED BEFORE
  // Wire.begin()/Wireling.begin()/Thermal/TOF (Step5_Combined_1_2_3_4b.ino
  // does this last, after the Wireling Adapter is already brought up). This
  // is the actual experiment: does initializing BLE's interrupt before the
  // Wireling Adapter touches the shared bus avoid the predicted hang? ---
  SerialUSB.println("Step 5b: starting initBlePairing() (BEFORE Wireling/sensors)...");
  initBlePairing();
  showStatus("BLE: WAITING");
  SerialUSB.println("Step 5b: BLE initialized - advertising as 'LockGuard'");
  delay(500);

  // --- Step 2: Thermal Camera Wireling ---
  Wire.begin();
  Wireling.begin();
  Wireling.selectPort(THERMAL_WIRELING_PORT);
  delay(100); // settling delay - see Step 2's header comment

  if (!thermalCam.begin()) {
    showStatus("THERMAL ERR");
    SerialUSB.println("Step 5b: thermalCam.begin() FAILED");
    while (true) { /* halt */ }
  }
  showStatus("THERMAL OK");
  SerialUSB.println("Step 5b: THERMAL OK");
  delay(500);

  // --- Step 3: TOF Distance Sensor Wireling ---
  Wireling.selectPort(TOF_WIRELING_PORT);
  if (!distanceSensor.init()) {
    showStatus("TOF ERR");
    SerialUSB.println("Step 5b: distanceSensor.init() FAILED");
    while (true) { /* halt */ }
  }
  distanceSensor.setTimeout(500);
  distanceSensor.setMeasurementTimingBudget(200000);
  distanceSensor.startContinuous();
  showStatus("TOF OK");
  SerialUSB.println("Step 5b: TOF OK");
  delay(500);

  showStatus("BLE: WAITING");
  SerialUSB.println("Step 5b: all four subsystems initialized - BLE advertising as 'LockGuard'");
}

void loop() {
  HCI_Process();
  if (set_connectable) {
    setConnectable();
    set_connectable = 0;
  }

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

  // TOF read (Port 0)
  Wireling.selectPort(TOF_WIRELING_PORT);
  uint16_t distanceMm = distanceSensor.readRangeContinuousMillimeters();
  bool tofTimedOut = distanceSensor.timeoutOccurred();

  // BLE RSSI
  int8_t rssi = 0;
  bool phoneNearby = isPairedPhoneNearby(&rssi);

  char line[24];
  if (phoneNearby) {
    snprintf(line, sizeof(line), "T:%d D:%u R:%d", warmPixelCount,
             tofTimedOut ? 0 : distanceMm, rssi);
  } else {
    snprintf(line, sizeof(line), "T:%d D:%u BLE:--", warmPixelCount,
             tofTimedOut ? 0 : distanceMm);
  }
  showStatus(line);

  SerialUSB.print("Step 5b: warm=");
  SerialUSB.print(warmPixelCount);
  SerialUSB.print(", dist=");
  if (tofTimedOut) SerialUSB.print("TIMEOUT"); else SerialUSB.print(distanceMm);
  SerialUSB.print("mm, BLE=");
  if (phoneNearby) {
    SerialUSB.print("connected RSSI=");
    SerialUSB.println(rssi);
  } else {
    SerialUSB.println("waiting");
  }
}
