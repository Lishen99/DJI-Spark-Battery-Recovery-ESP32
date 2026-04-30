# DJI Spark Battery Recovery — ESP32

Recover over-discharged DJI Spark batteries stuck in **Permanent Fail (PF) mode** using an ESP32 — no CP2112 USB-to-SMBus bridge required.

Works by communicating directly with the BQ40Z307 battery management chip over I2C to unseal it, clear the PF flags, and reset it so it will accept a charge again.

> **Confirmed working** on DJI Spark batteries (chip reports device type `0xFA02`).

---

## The problem

DJI Spark batteries that sit unused for too long over-discharge past the BMS protection threshold. The BMS chip (BQ40Z307, branded "BQ9003" by DJI) latches a **Permanent Fail** flag and disables charging FETs. The battery appears completely dead — the charger won't touch it and the battery LEDs flash in a fault pattern. Plugging the battery into the drone does nothing.

The official recovery tool ("DJI Battery Killer") uses a CP2112 USB-to-SMBus bridge. This sketch replaces that with an ESP32.

---

## What you need

| Item | Notes |
|---|---|
| ESP32 dev board | Any variant with GPIO21/GPIO22 |
| 2× 4.7 kΩ resistors | Pull-ups for SDA and SCL |
| 9V battery + connector | To temporarily power the dead BMS |
| Jumper wires | To reach the 6-pin battery connector |

---

## Wiring

### DJI Spark battery connector

6-pin JST-style connector, **pins numbered left → right with the notch on top**:

```
 ┌─────────────────────────────┐
 │  1    2    3    4    5    6  │  ← notch on top
 └──┬────┬────┬────┬────┬──────┘
    SCL  GND  BAT+ BAT+ GND  SDA
```

| Battery pin | Signal | Connect to |
|---|---|---|
| Pin 1 | SCL | ESP32 GPIO22 |
| Pin 2 | GND | ESP32 GND |
| Pin 3 | VBAT+ | 9V battery + (dead-battery boost only) |
| Pin 4 | VBAT+ | (parallel with Pin 3) |
| Pin 5 | GND | (parallel with Pin 2) |
| Pin 6 | SDA | ESP32 GPIO21 |

### Pull-ups

Connect a 4.7 kΩ resistor from **GPIO21 → 3.3V** and another from **GPIO22 → 3.3V**. These are required — the ESP32's internal pull-ups are too weak for SMBus.

### 9V boost (dead battery only)

If the battery is completely dead, the BMS chip has no power and won't respond. Before running recovery:

1. Connect ESP32 SDA/SCL/GND to battery pins 6/1/2 as above
2. Touch **9V+** to Pin 3 (and/or 4), **9V−** to Pin 2
3. Hold it — two LEDs blinking on the battery = BMS is awake
4. Run the recovery sequence while holding the 9V supply
5. Release 9V after the Reset step completes

> **Note:** The voltage reading shown during recovery (~8.2V) is the 9V supply feeding through, not the actual cell voltage. Cell voltage is unknown until PF is cleared and the battery charges normally.

---

## Setup

1. Install the [Arduino IDE](https://www.arduino.cc/en/software) with ESP32 board support
2. Open `dji_battery_recovery/dji_battery_recovery.ino`
3. Select your ESP32 board and port
4. Upload
5. Open Serial Monitor at **115200 baud**

---

## Usage

On boot the sketch scans the I2C bus and prints a menu:

```
  1  Read battery status
  S  Scan I2C bus
  U  Unseal (Spark: 0xCCDF7EE0, fallback TI defaults)
  K  Unseal with custom key pair
  P  Clear PF  (Permanent Fail 1)
  2  Clear PF2 (Permanent Fail 2)
  R  Reset chip
  L  Seal (re-lock)
  A  Auto: full recovery sequence
```

**For most cases, just press `A`.** This runs the full sequence automatically:

1. Unseal the chip with the DJI Spark key
2. Send all three PF clear commands (`0x002A`, `0x002B`, `0x0029`)
3. Reset the chip
4. Re-seal

Then plug into the DJI charger. Alternating LEDs = charging normally.

### Expected serial output (successful recovery)

```
[*] Pack voltage before: 8222 mV
[U] Trying DJI Spark key (0x7EE0/0xCCDF = 0xCCDF7EE0, low word first)...
E (...) I2C hardware NACK detected   ← normal, first key write always NACKs
[OK] Unsealed with Spark key! State: Unsealed
[P] Sending PF clear commands (timeouts = chip processing, that's OK):
[P]   0x002A → reg 0x44 : ACK
[P]   0x002B → reg 0x44 : ACK
[P]   0x0029 → reg 0x00 : ACK
[R] Sending chip reset...
[OK] BMS restarted. Voltage: 8226 mV
[DONE] Now plug into the DJI charger.
```

### Troubleshooting

| Symptom | Likely cause |
|---|---|
| `Nothing found!` on bus scan | Wiring wrong, pull-ups missing, or battery needs 9V boost |
| `READ ERROR` on voltage | Same as above |
| All PF commands NACK | Chip not unsealed — run U first, then P |
| Battery still won't charge after A | Run A a second time, or hold 9V longer during the sequence |
| `Not sealed` at end | Harmless — chip reseals itself on reset; the seal command is a no-op |

---

## How it works

The BQ40Z307 uses the SMBus protocol (compatible with I2C at 100 kHz). It has three security levels:

- **Sealed** — read-only, most commands blocked
- **Unsealed** — PF clear commands accepted
- **Full Access** — full register access (not needed for recovery)

**Unseal sequence:** Write the 32-bit key `0xCCDF7EE0` as two 16-bit words to register `0x00` (ManufacturerAccess) — low word `0x7EE0` first, then high word `0xCCDF`. The first write always NACKs (a security feature of the chip). The second ACKs and the chip transitions to Unsealed.

**PF clear sequence (from Unsealed):**
- `0x002A` → reg `0x44`: PermanentFailClear
- `0x002B` → reg `0x44`: PermanentFailClear2  
- `0x0029` → reg `0x00`: PermanentFailDataReset (the critical one)

After a chip reset, the BMS comes back in normal operating mode with PF cleared, and the charging FETs re-enable.

---

## References

- [dji-firmware-tools](https://github.com/o-gs/dji-firmware-tools) by mefistotelis — BQ40Z307 register map and SMBus protocol implementation used to identify the correct key order and PF reset command (`0x0029`)
- [dji-firmware-tools issue #258](https://github.com/o-gs/dji-firmware-tools/issues/258) — community confirmation of the `0xCCDF7EE0` unseal key
- [Texas Instruments BQ40Z307 Technical Reference](https://www.ti.com/product/BQ40Z307)

---

## License

MIT — see [LICENSE](LICENSE).
