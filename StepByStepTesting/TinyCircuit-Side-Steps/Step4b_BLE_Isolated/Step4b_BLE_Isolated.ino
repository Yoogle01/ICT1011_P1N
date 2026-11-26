/*******************************************************************************
 * Step 4b - BLE isolation test (no Wireling Adapter / sensors at all)
 *
 * Step 4 hard-hung at "BLE: SPI INIT" (the BLE TinyShield's interrupt
 * line defaults to pin 2, which the Wireling Adapter may conflict with).
 * This test strips out everything except BLE - run it with ONLY the
 * TinyScreen+ and BLE TinyShield stacked.
 * See ../../README.md for the expected results and what each outcome means.
 ******************************************************************************/

#include <SPI.h>
#include <Wire.h>
#include <TinyScreen.h>
#include <string.h>
#include <STBLE.h>

#define ADV_INTERVAL_MIN_MS 50
#define ADV_INTERVAL_MAX_MS 100
#define BLE_PAIRING_DEBUG true

TinyScreen display = TinyScreen(TinyScreenPlus);

volatile uint8_t set_connectable = 1;
uint16_t         connection_handle = 0;
bool             ownerPhoneConnected = false;

void showStatus(const char* status) {
  display.clearScreen();
  display.setCursor(5, 25);
  display.print(status);
}

void initBlePairing() {
  SerialUSB.println("Step 4b: HCI_Init()...");
  HCI_Init();
  SerialUSB.println("Step 4b: HCI_Init() done");

  showStatus("BLE: SPI INIT");
  SerialUSB.println("Step 4b: BNRG_SPI_Init()...");
  BNRG_SPI_Init();
  SerialUSB.println("Step 4b: BNRG_SPI_Init() done");

  SerialUSB.println("Step 4b: BlueNRG_RST()...");
  BlueNRG_RST();
  SerialUSB.println("Step 4b: BlueNRG_RST() done");

  uint8_t bdaddr[] = {0x12, 0x34, 0x00, 0xE1, 0x80, 0x02};
  showStatus("BLE: CONFIG");
  tBleStatus ret = aci_hal_write_config_data(CONFIG_DATA_PUBADDR_OFFSET, CONFIG_DATA_PUBADDR_LEN, bdaddr);
  SerialUSB.print("Step 4b: aci_hal_write_config_data() returned ");
  SerialUSB.println(ret);
  if (ret) {
    showStatus("BLE CFG ERR");
    SerialUSB.println("Step 4b: HALT - aci_hal_write_config_data failed");
    while (true) { /* halt */ }
  }

  showStatus("BLE: GATT INIT");
  ret = aci_gatt_init();
  SerialUSB.print("Step 4b: aci_gatt_init() returned ");
  SerialUSB.println(ret);
  if (ret) {
    showStatus("BLE GATT ERR");
    SerialUSB.println("Step 4b: HALT - aci_gatt_init failed");
    while (true) { /* halt */ }
  }

  showStatus("BLE: GAP INIT");
  uint16_t service_handle, dev_name_char_handle, appearance_char_handle;
  ret = aci_gap_init_IDB05A1(GAP_PERIPHERAL_ROLE_IDB05A1, 0, 0x07, &service_handle,
                             &dev_name_char_handle, &appearance_char_handle);
  SerialUSB.print("Step 4b: aci_gap_init_IDB05A1() returned ");
  SerialUSB.println(ret);
  if (ret) {
    showStatus("BLE GAP ERR");
    SerialUSB.println("Step 4b: HALT - aci_gap_init_IDB05A1 failed");
    while (true) { /* halt */ }
  }

  const char *name = "LockGuard";
  aci_gatt_update_char_value(service_handle, dev_name_char_handle, 0,
                              strlen(name), (uint8_t *)name);
  aci_hal_set_tx_power_level(1, 3);

  showStatus("BLE: ADV SETUP");
  setConnectable();
  SerialUSB.println("Step 4b: initBlePairing() complete");
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
  SerialUSB.print("Step 4b: BLE connection from: ");
  for (int i = 5; i >= 0; i--) {
    if (addr[i] < 0x10) SerialUSB.print("0");
    SerialUSB.print(addr[i], HEX);
    if (i > 0) SerialUSB.print("-");
  }
  SerialUSB.println();
#endif
  connection_handle = handle;
  ownerPhoneConnected = true;
  showStatus("BLE: CONNECTED");
}

void GAP_DisconnectionComplete_CB(void) {
  ownerPhoneConnected = false;
  set_connectable = TRUE;
  SerialUSB.println("Step 4b: BLE disconnected");
  showStatus("BLE: WAITING");
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

  display.begin();
  display.setBrightness(10);
  display.setFont(thinPixel7_10ptFontInfo);
  display.fontColor(TS_8b_White, TS_8b_Black);
  showStatus("OLED OK");
  SerialUSB.println("Step 4b: OLED OK");
  delay(500);

  SerialUSB.println("Step 4b: starting initBlePairing() - NO Wireling/sensors in this test");
  initBlePairing();

  showStatus("BLE: WAITING");
  SerialUSB.println("Step 4b: BLE advertising as 'LockGuard' - connect with a phone BLE scanner app");
}

void loop() {
  HCI_Process();
  if (set_connectable) {
    setConnectable();
    set_connectable = 0;
  }

  static uint32_t lastPoll = 0;
  uint32_t now = millis();
  if (now - lastPoll < 1000) {
    return;
  }
  lastPoll = now;

  int8_t rssi = 0;
  if (isPairedPhoneNearby(&rssi)) {
    char line[24];
    snprintf(line, sizeof(line), "RSSI: %d", rssi);
    showStatus(line);
    SerialUSB.print("Step 4b: connected, RSSI = ");
    SerialUSB.println(rssi);
  }
}
