/*******************************************************************************
 * Smart Lock-Screen for TinyScreen+ (ATSAMD21)
 *
 * Hardware: thermal camera (AMG8833, port 1), TOF sensor (VL53L0X, port 0),
 * BLE TinyShield (ST BlueNRG).
 *
 * Lock flow (checked every SENSOR_POLL_INTERVAL_MS):
 * - Owner recognized: cancel any countdown and close the privacy image.
 * - Owner absent, phone disconnected: lock after SILENT_LOCK_MS; no
 *   privacy image is shown.
 * - Owner absent, phone connected: request the privacy image. Start the
 *   lock countdown when the Jetson confirms it is open, or after
 *   PRIVACY_OPEN_FALLBACK_MS. Lock after PRIVACY_LOCK_HEAT_MS with heat,
 *   or PRIVACY_LOCK_NO_HEAT_MS without. If the phone disconnects
 *   mid-countdown, SILENT_LOCK_MS applies instead.
 * - After locking, seeing the owner for REARM_FACE_CONFIRM_MS rearms the
 *   kit (it cannot see whether the laptop was really unlocked).
 *
 * The Jetson sends face status over SerialUSB and relays lock/privacy
 * actions to the laptop over USB HID; this sketch sends no keystrokes
 * itself. Lock and privacy requests are repeated until acknowledged.
 * See ../../README.md for the full protocol and design notes.
 *
 * Three buttons manage the owner photo (upper-left deletes, upper-right
 * captures after a countdown, lower-right trains); the lower-left button
 * shows the Jetson's camera preview on the OLED while held.
 *
 * Any BLE connection currently counts as the owner's phone; the RSSI
 * distance check is bypassed because hci_read_rssi() hangs the board.
 *
 * Libraries: TinyScreen, Wireling, Adafruit_AMG88xx, Pololu VL53L0X, STBLE.
 ******************************************************************************/

#include <Wire.h>
#include <SPI.h>
#include <TinyScreen.h>
#include <Wireling.h>
#include <Adafruit_AMG88xx.h>
#include <VL53L0X.h>
#include <string.h>
#include <STBLE.h>

// ---------------------------------------------------------------------------
// Configuration - tune these during real-world testing
//
// The countdowns apply only while the owner is absent. Heat and phone do
// not trigger a lock on their own.
// ---------------------------------------------------------------------------
const uint8_t  THERMAL_WIRELING_PORT     = 1;     // Thermal sensor port
const uint8_t  TOF_WIRELING_PORT         = 0;     // TOF sensor port
const float    PRESENCE_TEMP_THRESHOLD_C = 28.0;  // Warm-pixel threshold; tune per room
const uint8_t  PRESENCE_MIN_PIXELS       = 3;     // Minimum warm pixels for presence
const int8_t   BLE_RSSI_THRESHOLD_DBM    = -75;   // RSSI cutoff (currently bypassed)
const uint32_t SILENT_LOCK_MS             = 5000;  // Lock delay when the phone is disconnected
const uint32_t PRIVACY_LOCK_HEAT_MS       = 5000;  // Lock delay with heat during privacy mode
const uint32_t PRIVACY_LOCK_NO_HEAT_MS    = 10000; // Lock delay without heat during privacy mode
const uint32_t PRIVACY_OPEN_FALLBACK_MS   = 5000;  // Start countdown if the Jetson never confirms
const uint32_t PRIVACY_RESEND_INTERVAL_MS = 2000;  // Privacy request retry interval
const uint32_t SENSOR_POLL_INTERVAL_MS   = 1000;  // Sensor check interval
const uint32_t LOCK_WARNING_LEAD_MS      = 2000;  // Warning time before lock
const uint32_t PC_SIGNAL_TIMEOUT_MS      = 5000;  // Face signal timeout
const uint32_t LOCK_RESEND_INTERVAL_MS   = 5000;  // Lock request retry interval
// The kit cannot detect a laptop unlock. It rearms after seeing the owner
// continuously for REARM_FACE_CONFIRM_MS.
const uint32_t REARM_FACE_CONFIRM_MS     = 2000;

// ---------------------------------------------------------------------------
// Owner photo buttons
// ---------------------------------------------------------------------------
const uint32_t BUTTON_DEBOUNCE_MS          = 300;  // Button debounce interval
const uint8_t  CAPTURE_COUNTDOWN_SEC       = 3;    // Delay before photo capture
const uint32_t STATUS_OVERRIDE_DURATION_MS = 2000; // Duration of temporary OLED messages

// ---------------------------------------------------------------------------
// BLE owner-phone pairing configuration
// ---------------------------------------------------------------------------
// Android randomizes BLE addresses, so any connection currently counts as
// the owner's phone. See GAP_ConnectionComplete_CB().
#define BLE_PAIRING_DEBUG true

// ---------------------------------------------------------------------------
// BLE state updated by the HCI event callbacks
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

// Which branch of the lock flow is active.
enum PresenceReason {
  REASON_OWNER_PRESENT, // Owner recognized
  REASON_PRIVACY_MODE,  // Owner absent, phone connected
  REASON_SILENT_LOCK    // Owner absent, phone disconnected
};

PresenceReason currentReason   = REASON_OWNER_PRESENT;
bool           currentlyLocked = false;

bool     lockWarningSent          = false; // Whether this countdown's warning was sent

// Lock flow state
uint32_t privacyStartTime         = 0;     // First privacy request time
uint32_t lastPrivacySendTime      = 0;     // Most recent privacy request time
bool     privacyImageAcked        = false; // Jetson confirmed the image opened
bool     lockCountdownRunning     = false; // Whether the lock timer is active
uint32_t lockCountdownStart       = 0;     // Lock timer start
bool     countdownHeatPresent     = false; // Heat reading captured at timer start
bool     lastPersonPresent        = false; // Latest thermal reading

// Retry the lock request until the Jetson confirms it sent Win+L or the
// owner returns.
bool     lockDeliveryPending      = false;
uint32_t lastLockSendTime         = 0;
bool     faceSeenWhileLocked      = false; // Owner currently seen while locked
uint32_t faceSeenSince            = 0;     // Start of continuous owner detection

bool     pcFacePresent            = false; // Latest face status from the Jetson
uint32_t lastPcMessageTime        = 0;     // Time of last valid face message

// Owner photo button state
uint32_t lastUpperLeftPressTime  = 0; // Debounce timestamps, one per button
uint32_t lastUpperRightPressTime = 0;
uint32_t lastLowerRightPressTime = 0;

bool     captureCountdownActive = false; // Whether the photo countdown is active
uint32_t captureCountdownStart  = 0;     // Photo countdown start
uint8_t  lastShownCountdownSec  = 255;   // Forces the first countdown display

bool     statusOverrideActive = false; // Whether a temporary message is displayed
uint32_t statusOverrideSetAt  = 0;      // Time the message appeared

// Live preview frames received from the Jetson over SerialUSB.
const uint8_t  FRAME_MAGIC_1 = 0xA5;  // Two-byte preview packet marker
const uint8_t  FRAME_MAGIC_2 = 0x5A;
const uint16_t PREVIEW_FRAME_BYTES = 96 * 64;
const uint32_t FRAME_RX_TIMEOUT_MS = 500;   // Discard incomplete frames after this
const uint32_t PREVIEW_PING_INTERVAL_MS = 1000; // Keep the Jetson stream active
const uint32_t NO_VIDEO_AFTER_MS   = 2000;  // Show "NO VIDEO" after this interval
enum RxState { RX_TEXT, RX_MAGIC2, RX_FRAME };
RxState  rxState         = RX_TEXT;
uint8_t  frameBuf[PREVIEW_FRAME_BYTES];
uint16_t frameLen        = 0;
uint32_t frameRxStart    = 0;
bool     previewMode     = false;
uint32_t lastFrameDrawTime = 0;
uint32_t lastPreviewPing = 0;
bool     noVideoShown    = false;
uint32_t lastLowerLeftDownTime = 0;      // Last time the preview button was held
const uint32_t PREVIEW_RELEASE_DEBOUNCE_MS = 100; // Release debounce interval

// ---------------------------------------------------------------------------
// Inline ARM assembly (ATSAMD21 / Cortex-M0+ Thumb)
// ---------------------------------------------------------------------------
// Return whether elapsed time has reached the timeout.
static inline bool asmTimeoutElapsed(uint32_t elapsed, uint32_t timeoutMs) {
  uint32_t result;
  asm volatile (
    "cmp   %1, %2      \n\t"   // Compare elapsed time with timeout
    "bhs   1f          \n\t"   // Branch if elapsed >= timeout
    "movs  %0, #0      \n\t"   // Not timed out
    "b     2f          \n\t"
    "1:                \n\t"
    "movs  %0, #1      \n\t"   // Timed out
    "2:                \n\t"
    : "=r" (result)
    : "r" (elapsed), "r" (timeoutMs)
    : "cc"
  );
  return result != 0;
}

void setup() {
  SerialUSB.begin(115200); // USB serial link to the Jetson

  // Initialize the display before any error messages.
  display.begin();
  // Flip the display vertically; button mapping is handled by the library.
  display.setFlip(1);
  display.setMirror(0);
  display.setBrightness(10);
  display.setFont(thinPixel7_10ptFontInfo);
  display.fontColor(TS_8b_White, TS_8b_Black);

  // The warning lead must stay below the shortest lock countdown,
  // otherwise the countdown subtraction in loop() would underflow.
  if (LOCK_WARNING_LEAD_MS >= SILENT_LOCK_MS || LOCK_WARNING_LEAD_MS >= PRIVACY_LOCK_HEAT_MS) {
    showStatus("CFG ERR");
    while (true) { /* halt */ }
  }

  // BLE must init before Wire/Wireling touch the bus - doing it after
  // caused a hard hang (see StepByStepTesting Step 5b).
  initBlePairing();

  Wire.begin();

  Wireling.begin();
  Wireling.selectPort(THERMAL_WIRELING_PORT);
  delay(100); // Let the sensor boot up after the mux switches ports.

  if (!thermalCam.begin()) {
    // Check the thermal sensor connection and Wireling port.
    showStatus("THERMAL ERR");
    while (true) { /* halt */ }
  }

  // Shared I2C bus - select the TOF port before initializing it. The TOF
  // reading is not used by the lock logic yet; kept running for later.
  Wireling.selectPort(TOF_WIRELING_PORT);
  if (!distanceSensor.init()) {
    showStatus("TOF ERR");
    while (true) { /* halt */ }
  }
  distanceSensor.setTimeout(500);
  distanceSensor.setMeasurementTimingBudget(200000); // Balance speed and range.
  distanceSensor.startContinuous();

  showStatus("UNLOCKED");
}

void loop() {
  // Process BLE events every loop; sensor checks use millis(), not delay().
  HCI_Process();
  if (set_connectable) {
    setConnectable();
    set_connectable = 0;
  }

  // Read serial input every loop to keep the face status current.
  pollHostLink();

  // Handle buttons every loop for responsive input.
  handleOwnerCaptureButtons();

  static uint32_t lastSensorCheckTime = 0;
  uint32_t now = millis();
  if (now - lastSensorCheckTime < SENSOR_POLL_INTERVAL_MS) {
    return; // Wait for the next sensor poll.
  }
  lastSensorCheckTime = now;

  SerialUSB.println("DBG: before isThermalPresenceDetected()");
  bool personPresent     = isThermalPresenceDetected();
  lastPersonPresent      = personPresent;
  SerialUSB.println("DBG: after isThermalPresenceDetected(), before isPairedPhoneNearby()");
  bool phoneNearby       = isPairedPhoneNearby();
  SerialUSB.println("DBG: after isPairedPhoneNearby()");

  // Treat missing or stale face messages as owner absent (fail-secure).
  bool piConnected       = (now - lastPcMessageTime) < PC_SIGNAL_TIMEOUT_MS;
  bool facePresent       = piConnected && pcFacePresent;

  // Rearm after the owner is recognized continuously for the configured time.
  if (currentlyLocked) {
    if (!facePresent) {
      faceSeenWhileLocked = false;
    } else if (!faceSeenWhileLocked) {
      faceSeenWhileLocked = true;
      faceSeenSince = now;
    } else if (asmTimeoutElapsed(now - faceSeenSince, REARM_FACE_CONFIRM_MS)) {
      currentlyLocked        = false;
      lockDeliveryPending    = false;
      lockWarningSent        = false;
      faceSeenWhileLocked    = false;
      lockCountdownRunning   = false;
      currentReason          = REASON_OWNER_PRESENT;
      SerialUSB.println("MCU:STATE=UNLOCKED");
    }
  } else {
    faceSeenWhileLocked = false;
  }

  // The owner's face controls the lock flow; phone and heat select the
  // countdown once the owner is absent. Never triggers while LOCKED.
  bool     countdownActive = false;
  uint32_t remainingMs     = 0;

  if (!currentlyLocked) {
    if (facePresent) {
      if (currentReason != REASON_OWNER_PRESENT) {
        // Cancel the countdown; close the privacy image if it was requested.
        bool wasPrivacy      = (currentReason == REASON_PRIVACY_MODE);
        currentReason        = REASON_OWNER_PRESENT;
        lockCountdownRunning = false;
        lockWarningSent      = false;
        if (wasPrivacy) {
          SerialUSB.println("MCU:STATE=UNLOCKED");
        }
      }
    } else {
      if (currentReason == REASON_OWNER_PRESENT) {
        lockWarningSent      = false;
        privacyImageAcked    = false;
        lockCountdownRunning = false;
        if (!phoneNearby) {
          // Start the no-privacy countdown.
          currentReason        = REASON_SILENT_LOCK;
          lockCountdownRunning = true;
          lockCountdownStart   = now;
        } else {
          currentReason       = REASON_PRIVACY_MODE;
          privacyStartTime    = now;
          lastPrivacySendTime = now;
          // Ask the Jetson to open the privacy image.
          SerialUSB.println("MCU:PRIVACY_MODE");
        }
      } else if (currentReason == REASON_PRIVACY_MODE) {
        // Retry until the Jetson confirms the image is open.
        if (!privacyImageAcked &&
            asmTimeoutElapsed(now - lastPrivacySendTime, PRIVACY_RESEND_INTERVAL_MS)) {
          lastPrivacySendTime = now;
          SerialUSB.println("MCU:PRIVACY_MODE");
        }
        // Start anyway after the fallback timeout if no confirmation arrives.
        if (!lockCountdownRunning &&
            asmTimeoutElapsed(now - privacyStartTime, PRIVACY_OPEN_FALLBACK_MS)) {
          lockCountdownRunning = true;
          lockCountdownStart   = now;
          countdownHeatPresent = personPresent;
        }
      }

      if (lockCountdownRunning) {
        uint32_t lockWait;
        if (currentReason == REASON_SILENT_LOCK) {
          lockWait = SILENT_LOCK_MS;
        } else if (!phoneNearby) {
          lockWait = SILENT_LOCK_MS; // Phone disconnected during privacy mode.
        } else {
          lockWait = countdownHeatPresent ? PRIVACY_LOCK_HEAT_MS : PRIVACY_LOCK_NO_HEAT_MS;
        }
        uint32_t elapsed  = now - lockCountdownStart;
        countdownActive   = true;
        remainingMs       = (elapsed < lockWait) ? (lockWait - elapsed) : 0;

        if (!lockWarningSent && lockWait > LOCK_WARNING_LEAD_MS &&
            asmTimeoutElapsed(elapsed, lockWait - LOCK_WARNING_LEAD_MS)) {
          SerialUSB.println("MCU:WARN"); // Warn before locking.
          lockWarningSent = true;
        }
        if (asmTimeoutElapsed(elapsed, lockWait)) {
          // The Jetson handles the lock request by sending Win+L.
          currentlyLocked     = true;
          lockDeliveryPending = true;
          lastLockSendTime    = now;
          faceSeenWhileLocked = false;
          countdownActive     = false;
          remainingMs         = 0;
          SerialUSB.println("MCU:STATE=LOCKED");
        }
      }
    }
  }

  // Retry until the Jetson confirms it sent Win+L.
  if (currentlyLocked && lockDeliveryPending &&
      asmTimeoutElapsed(now - lastLockSendTime, LOCK_RESEND_INTERVAL_MS)) {
    lastLockSendTime = now;
    SerialUSB.println("MCU:STATE=LOCKED");
  }

  // Send the status line for Jetson-side logging. REMAIN_MS is meaningful
  // only while ACTIVE=1. See README.md for the field reference.
  SerialUSB.print("MCU:STATUS=HEAT=");
  SerialUSB.print(personPresent ? 1 : 0);
  SerialUSB.print(",PHONE=");
  SerialUSB.print(phoneNearby ? 1 : 0);
  SerialUSB.print(",BLE=");
  SerialUSB.print(ownerPhoneConnected ? 1 : 0);
  SerialUSB.print(",FACE=");
  SerialUSB.print(facePresent ? 1 : 0);
  SerialUSB.print(",LOCKED=");
  SerialUSB.print(currentlyLocked ? 1 : 0);
  SerialUSB.print(",ACTIVE=");
  SerialUSB.print(countdownActive ? 1 : 0);
  SerialUSB.print(",PRIVACY=");
  SerialUSB.print(currentReason == REASON_PRIVACY_MODE ? 1 : 0);
  SerialUSB.print(",REMAIN_MS=");
  SerialUSB.println(remainingMs);

  // Preserve temporary button messages until their display interval ends.
  if (statusOverrideActive && asmTimeoutElapsed(now - statusOverrideSetAt, STATUS_OVERRIDE_DURATION_MS)) {
    statusOverrideActive = false;
  }
  if (!statusOverrideActive && !previewMode) {
    showStatus(currentlyLocked ? "LOCKED" : "UNLOCKED");
  }
}

// ---------------------------------------------------------------------------
// Read face status and acknowledgements from the Jetson without blocking.
// ---------------------------------------------------------------------------
void pollHostLink() {
  static char    lineBuf[32];
  static uint8_t lineLen = 0;

  // Reset the parser if a frame was only partially received.
  if (rxState == RX_FRAME && (millis() - frameRxStart) > FRAME_RX_TIMEOUT_MS) {
    rxState = RX_TEXT;
  }

  while (SerialUSB.available()) {
    uint8_t c = SerialUSB.read();

    if (rxState == RX_FRAME) {
      frameBuf[frameLen++] = c;
      if (frameLen >= PREVIEW_FRAME_BYTES) {
        rxState = RX_TEXT;
        // Consume every frame to stay in sync; draw only when appropriate.
        if (previewMode && !statusOverrideActive) {
          drawPreviewFrame();
        }
      }
    } else if (rxState == RX_MAGIC2) {
      if (c == FRAME_MAGIC_2) {
        rxState      = RX_FRAME;
        frameLen     = 0;
        frameRxStart = millis();
      } else {
        rxState = RX_TEXT;
      }
    } else if (c == FRAME_MAGIC_1) {
      rxState = RX_MAGIC2;
      lineLen = 0; // drop any partial text line
    } else if (c == '\n' || c == '\r') {
      if (lineLen > 0) {
        lineBuf[lineLen] = '\0';
        if (strncmp(lineBuf, "PC:FACE=", 8) == 0) {
          pcFacePresent     = (lineBuf[8] == '1');
          lastPcMessageTime = millis();
        } else if (strncmp(lineBuf, "PC:ACK=", 7) == 0) {
          handleOwnerCaptureAck(lineBuf + 7);
        }
        lineLen = 0;
      }
    } else if (lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = (char)c;
    }
  }
}

// Draw the most recently received preview frame across the whole OLED.
void drawPreviewFrame() {
  display.goTo(0, 0);
  display.startData();
  display.writeBuffer(frameBuf, PREVIEW_FRAME_BYTES);
  display.endTransfer();
  lastFrameDrawTime = millis();
  noVideoShown      = false;
}

// Toggle the OLED live preview and tell the Jetson whether to stream.
void setPreviewMode(bool on) {
  previewMode       = on;
  noVideoShown      = false;
  lastFrameDrawTime = millis();
  if (on) {
    display.clearScreen();
    lastPreviewPing = millis();
    SerialUSB.println("MCU:CMD=PREVIEW");
  } else {
    SerialUSB.println("MCU:CMD=PREVIEW_OFF");
    showStatus(currentlyLocked ? "LOCKED" : "UNLOCKED");
  }
}

// ---------------------------------------------------------------------------
// Owner photo buttons and the capture countdown
// ---------------------------------------------------------------------------
void handleOwnerCaptureButtons() {
  uint32_t now     = millis();
  uint8_t  buttons = display.getButtons();

  // Show preview while the lower-left button is held; debounce its release.
  // Holding it during power-on still enters the bootloader (checked before
  // this sketch runs).
  bool lowerLeftDown = (buttons & TSButtonLowerLeft) != 0;
  if (lowerLeftDown) {
    lastLowerLeftDownTime = now;
    if (!previewMode && !captureCountdownActive) {
      setPreviewMode(true);
    }
  } else if (previewMode &&
             asmTimeoutElapsed(now - lastLowerLeftDownTime, PREVIEW_RELEASE_DEBOUNCE_MS)) {
    setPreviewMode(false);
  }

  if (previewMode) {
    // Keep the Jetson stream active.
    if (asmTimeoutElapsed(now - lastPreviewPing, PREVIEW_PING_INTERVAL_MS)) {
      lastPreviewPing = now;
      SerialUSB.println("MCU:CMD=PREVIEW");
    }
    // Do not count temporary messages as missing video.
    if (statusOverrideActive) {
      lastFrameDrawTime = now;
    } else if (!noVideoShown &&
               asmTimeoutElapsed(now - lastFrameDrawTime, NO_VIDEO_AFTER_MS)) {
      showStatus("NO VIDEO");
      noVideoShown = true;
    }
  }

  // Ignore photo buttons while a capture countdown is active.
  if (!captureCountdownActive) {
    if ((buttons & TSButtonUpperLeft) &&
        asmTimeoutElapsed(now - lastUpperLeftPressTime, BUTTON_DEBOUNCE_MS)) {
      lastUpperLeftPressTime = now;
      SerialUSB.println("MCU:CMD=DELETE_REF_PHOTO");
      showTransientStatus("DELETING");
    }
    if ((buttons & TSButtonUpperRight) &&
        asmTimeoutElapsed(now - lastUpperRightPressTime, BUTTON_DEBOUNCE_MS)) {
      lastUpperRightPressTime = now;
      captureCountdownActive  = true;
      captureCountdownStart   = now;
      lastShownCountdownSec   = 255; // Force the first countdown display.
    }
    if ((buttons & TSButtonLowerRight) &&
        asmTimeoutElapsed(now - lastLowerRightPressTime, BUTTON_DEBOUNCE_MS)) {
      lastLowerRightPressTime = now;
      SerialUSB.println("MCU:CMD=TRAIN_REF_PHOTO");
      showTransientStatus("TRAINING");
    }
  }

  if (captureCountdownActive) {
    uint32_t elapsedMs = now - captureCountdownStart;
    uint32_t totalMs    = (uint32_t)CAPTURE_COUNTDOWN_SEC * 1000;
    if (elapsedMs >= totalMs) {
      captureCountdownActive = false;
      SerialUSB.println("MCU:CMD=CAPTURE_REF_PHOTO");
      showTransientStatus("CAPTURING");
    } else {
      // Round up so the display does not show zero early.
      uint8_t remainingSec = (uint8_t)((totalMs - elapsedMs + 999) / 1000);
      if (remainingSec != lastShownCountdownSec) {
        lastShownCountdownSec = remainingSec;
        showCaptureCountdown(remainingSec);
      }
    }
  }
}

// Show results returned by the Jetson on the OLED.
void handleOwnerCaptureAck(const char* ack) {
  if (strcmp(ack, "PHOTO_DELETED") == 0) {
    showTransientStatus("DELETED");
  } else if (strcmp(ack, "CAPTURE_OK") == 0) {
    showTransientStatus("CAPTURE OK");
  } else if (strcmp(ack, "CAPTURE_NO_FACE") == 0) {
    showTransientStatus("NO FACE");
  } else if (strcmp(ack, "TRAIN_OK") == 0) {
    showTransientStatus("TRAINED OK");
  } else if (strcmp(ack, "TRAIN_FAIL") == 0) {
    showTransientStatus("TRAIN FAIL");
  } else if (strcmp(ack, "LOCK_SENT") == 0) {
    lockDeliveryPending = false; // Stop retrying the lock request.
  } else if (strcmp(ack, "PRIVACY_OPEN") == 0) {
    // Start the countdown once the privacy image is open.
    privacyImageAcked = true;
    if (currentReason == REASON_PRIVACY_MODE && !currentlyLocked && !lockCountdownRunning) {
      lockCountdownRunning = true;
      lockCountdownStart   = millis();
      countdownHeatPresent = lastPersonPresent; // Heat is read once, here.
    }
  }
  // Ignore acknowledgements this sketch does not handle.
}

// Show a temporary message instead of the normal lock status.
void showTransientStatus(const char* msg) {
  showStatus(msg);
  statusOverrideActive = true;
  statusOverrideSetAt  = millis();
}

// Show the photo-capture countdown.
void showCaptureCountdown(uint8_t remainingSec) {
  display.clearScreen();
  display.setCursor(5, 10);
  display.print("LOOK AT");
  display.setCursor(5, 25);
  display.print("CAMERA");
  display.setCursor(40, 45);
  display.print(remainingSec);
  statusOverrideActive = true;
  statusOverrideSetAt  = millis();
}

// ---------------------------------------------------------------------------
// Thermal presence check (AMG8833)
// ---------------------------------------------------------------------------
bool isThermalPresenceDetected() {
  // Select the thermal sensor's port on the shared I2C bus.
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
// BLE setup (ST BlueNRG) - advertises as "LockGuard"
// ---------------------------------------------------------------------------
void initBlePairing() {
  HCI_Init();
  BNRG_SPI_Init();
  BlueNRG_RST();

  // Configure the device's BLE address.
  uint8_t bdaddr[] = {0x12, 0x34, 0x00, 0xE1, 0x80, 0x02};
  aci_hal_write_config_data(CONFIG_DATA_PUBADDR_OFFSET, CONFIG_DATA_PUBADDR_LEN, bdaddr);

  aci_gatt_init();

  uint16_t service_handle, dev_name_char_handle, appearance_char_handle;
  aci_gap_init_IDB05A1(GAP_PERIPHERAL_ROLE_IDB05A1, 0, 0x07, &service_handle,
                        &dev_name_char_handle, &appearance_char_handle);

  const char *name = "LockGuard";
  aci_gatt_update_char_value(service_handle, dev_name_char_handle, 0,
                              strlen(name), (uint8_t *)name);

  aci_hal_set_tx_power_level(1, 3); // +4 dBm

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
  // ownerPhoneConnected is updated by the BLE callbacks below.
  if (!ownerPhoneConnected) {
    return false;
  }

  // RSSI reads currently hang the board, so any active connection counts
  // as nearby.
  return true;

  /*
  int8_t rssi;
  int ret = hci_read_rssi(&connection_handle, &rssi);
  if (ret != 0) {
    return false; // RSSI read failed - treat as not nearby.
  }

  return (rssi > BLE_RSSI_THRESHOLD_DBM);
  */
}

// ---------------------------------------------------------------------------
// BLE event callbacks invoked via HCI_Process()
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

  // Accept any phone connection; Android randomizes its BLE address.
  connection_handle = handle;
  ownerPhoneConnected = true;
  SerialUSB.println("DBG: GAP_ConnectionComplete_CB() finished, handle set");
}

void GAP_DisconnectionComplete_CB(void) {
  ownerPhoneConnected = false;
  set_connectable = TRUE; // Make the device connectable again.
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

void showStatus(const char* status) {
  display.clearScreen();
  display.setCursor(5, 25);
  display.print(status);
}
