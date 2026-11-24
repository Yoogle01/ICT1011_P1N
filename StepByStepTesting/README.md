# Step-by-Step Bring-Up & Test Plan

This folder verifies every subsystem of the Smart Lock-Screen project on
real hardware **one at a time**, instead of uploading the full
`SmartLockScreen.ino` and guessing which subsystem broke. Each step is a
**strict superset** of the previous one, so by the final step you have
rebuilt the full sketch from verified pieces.

## Order and why

```
Step 1:  OLED only
Step 2:  + Thermal camera
Step 3:  + TOF distance sensor
Step 4b: BLE alone (no Wireling Adapter stacked)
Step 5b: Combined, BLE initialized first
Final:   full SmartLockScreen.ino
```

Step 4 needs a phone: connect with a BLE scanner app (e.g. "nRF Connect"
on Android/iOS) to the advertised `LockGuard` device.

Steps 4b/5b exist because the original combined sketch hard-hung at
"BLE: SPI INIT": the BLE TinyShield's interrupt line defaults to pin 2,
and initializing BLE **after** the Wireling Adapter touched the bus
caused the hang. Step 4b isolates BLE with nothing else stacked; Step 5b
tests the full stack with BLE initialized first (the order the final
sketch now uses).

## How to run each step

1. Open the step's `.ino` in Arduino IDE (each step is its own sketch
   folder, since Arduino requires the folder name to match the `.ino`).
2. Tools → Board: TinyScreen+. Tools → Port: the kit's COM port.
3. Upload, then open the Serial Monitor at **115200 baud**.
4. Compare what you see with "Expected results" below before moving on —
   debug a failing step first rather than adding more code on top.

## Expected results

| Step | OLED | Notes |
|---|---|---|
| 1 | `OLED OK` within ~1s | Serial prints `Step 1: OLED OK` once. Black screen → check Board/Port and that the upload succeeded; otherwise hardware/seating |
| 2 | `THERMAL OK`, then a live 8x8 heatmap + `WARM PX: n` | Wave a hand: a red/yellow blob appears, warm count jumps, serial flips PRESENT/ABSENT. `THERMAL ERR` → check Port 1 seating. Mirrored image → flip `THERMAL_FLIP_X/Y` |
| 3 | `T:x D:xmm` cycling every second | Hand close to the TOF → distance drops while thermal keeps working (proves the shared I2C mux works). `TOF ERR` → check Port 0 and that the VL53L0X library is Pololu's |
| 4b | `BLE: WAITING` → `BLE: CONNECTED` → `RSSI: n` | Must get past `BLE: SPI INIT` with only the BLE shield stacked. If it still hangs, check the shield's INT resistor position (silkscreen near "INT") |
| 5b | `THERMAL OK`/`TOF OK`/combined status | BLE-first init should avoid the Step-5 hang; serial stays connected throughout |

