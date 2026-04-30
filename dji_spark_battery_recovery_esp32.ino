/*
 * DJI Spark Battery Recovery — ESP32
 * Replaces the CP2112 USB-to-SMBus bridge with ESP32 I2C.
 *
 * Battery chip : BQ40Z307 (branded BQ9003 by DJI), SMBus addr 0x0B
 *
 * ── Wiring ────────────────────────────────────────────────────────────────
 *
 *  DJI Spark 6-pin connector (pins numbered left → right, notch on top):
 *
 *    Pin 1: SCL   → ESP32 GPIO22   (+ 4.7 kΩ pull-up to 3.3 V)
 *    Pin 2: GND   → ESP32 GND
 *    Pin 3: VBAT+ → 9 V battery +  (only when boosting a dead battery)
 *    Pin 4: VBAT+ → (same, parallel)
 *    Pin 5: GND   → (same as Pin 2)
 *    Pin 6: SDA   → ESP32 GPIO21   (+ 4.7 kΩ pull-up to 3.3 V)
 *
 *  Power the ESP32 from USB only.  Never feed 9 V into the ESP32.
 *  The 9 V temporary boost gives the dead BMS enough power to talk.
 *
 * ── 9 V boost procedure ───────────────────────────────────────────────────
 *  1. Connect ESP32 SDA/SCL/GND to battery pins 6/1/2.
 *  2. Touch 9 V+ to Pin 3 (and/or 4), 9 V- to Pin 2 (and/or 5).
 *     Hold it – two LEDs flashing on the battery = BMS awake.
 *  3. Immediately run option A (auto recover) or step through U/P/2/R/L.
 *  4. Keep the 9 V held until the Reset step completes.
 *
 * ── Recovery steps (mirrors DJI Battery Killer software) ─────────────────
 *  U → Unseal
 *  P → Clear PF  (Permanent Fail flag 1)
 *  2 → Clear PF2 (Permanent Fail flag 2)
 *  R → Reset chip
 *  L → Seal (re-lock)
 *  A → All of the above automatically
 */

#include <Wire.h>

#define BATT_ADDR  0x0B   // Standard SBS/SMBus battery address
#define SDA_PIN    21
#define SCL_PIN    22

// ── Standard SBS registers ────────────────────────────────────────────────
#define R_TEMP      0x08
#define R_VOLTAGE   0x09
#define R_CURRENT   0x0A
#define R_RSOC      0x0D   // Relative State of Charge (%)
#define R_STATUS    0x19   // BatteryStatus flags
#define R_MAC       0x44   // ManufacturerBlockAccess (BQ40Z307 gateway)

// ── BQ40Z307 MAC subcommands ──────────────────────────────────────────────
#define MAC_DEV_TYPE       0x0001
#define MAC_FW_VER         0x0002
#define MAC_SAFETY_ALERT   0x0050
#define MAC_SAFETY_STATUS  0x0051
#define MAC_PF_ALERT       0x0052
#define MAC_PF_STATUS      0x0053
#define MAC_OP_STATUS      0x0054
#define MAC_PF_CLEAR       0x002A   // Clear PF flag 1
#define MAC_PF2_CLEAR      0x002B   // Clear PF flag 2
#define MAC_RESET          0x0041   // Soft reset
#define MAC_SEAL           0x0030   // Re-seal device

// DJI Spark confirmed key: 0xCCDF7EE0 — LOW word written first, then HIGH word.
// comm_sbs_bqctrl.py: sec_key_w0 = (i32key) & 0xffff = 0x7EE0 (first),
//                     sec_key_w1 = (i32key >> 16) & 0xffff = 0xCCDF (second).
// First key write always NACKs — that is a security feature of the chip, not an error.
#define KEY_SPARK_1        0x7EE0   // low  16 bits of 0xCCDF7EE0 — first write
#define KEY_SPARK_2        0xCCDF   // high 16 bits of 0xCCDF7EE0 — second write

// Fallback: default TI keys (tried if Spark key fails)
#define KEY_UNSEAL_1       0x0414
#define KEY_UNSEAL_2       0x3672
#define KEY_FULL_1         0xFFFF
#define KEY_FULL_2         0xFFFF

// PermanentFailDataReset: clears ALL PF flags — goes to reg 0x00, not 0x44
// Confirmed: BQ40z307.py line 73 — PermanentFailDataReset = 0x29
#define MAC_PF_DATA_RESET  0x0029

// ── Low-level helpers ─────────────────────────────────────────────────────

bool writeWord(uint8_t reg, uint16_t val) {
    Wire.beginTransmission(BATT_ADDR);
    Wire.write(reg);
    Wire.write((uint8_t)(val & 0xFF));
    Wire.write((uint8_t)(val >> 8));
    return Wire.endTransmission() == 0;
}

int32_t readWord(uint8_t reg) {
    Wire.beginTransmission(BATT_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return -1;
    if (Wire.requestFrom((uint8_t)BATT_ADDR, (uint8_t)2) != 2) return -1;
    uint16_t lo = Wire.read();
    uint16_t hi = Wire.read();
    return (int32_t)((hi << 8) | lo);
}

// Write 2-byte subcommand to register 0x00 (ManufacturerAccess)
// Used for: unseal keys, full-access keys, reset, seal — these work even while sealed
bool mac00Write(uint16_t subcmd) {
    Wire.beginTransmission(BATT_ADDR);
    Wire.write(0x00);
    Wire.write((uint8_t)(subcmd & 0xFF));
    Wire.write((uint8_t)(subcmd >> 8));
    return Wire.endTransmission() == 0;
}

// Write 2-byte MAC subcommand to register 0x44 (ManufacturerBlockAccess)
// Used for: PF clear, status reads — only accessible after unsealing
bool macSend(uint16_t subcmd) {
    Wire.beginTransmission(BATT_ADDR);
    Wire.write(R_MAC);
    Wire.write((uint8_t)(subcmd & 0xFF));
    Wire.write((uint8_t)(subcmd >> 8));
    return Wire.endTransmission() == 0;
}

// Block-read response from register 0x44 (first byte = byte count)
// Uses stop+start instead of repeated-start — some BMS chips need this
uint8_t macRead(uint8_t* buf, uint8_t maxLen) {
    Wire.beginTransmission(BATT_ADDR);
    Wire.write(R_MAC);
    if (Wire.endTransmission(true) != 0) return 0;  // STOP, then new START below
    delay(5);
    Wire.requestFrom((uint8_t)BATT_ADDR, (uint8_t)(maxLen + 1));
    if (!Wire.available()) return 0;
    uint8_t len = Wire.read();               // byte-count prefix
    len = min(len, maxLen);
    for (uint8_t i = 0; i < len && Wire.available(); i++) buf[i] = Wire.read();
    while (Wire.available()) Wire.read();    // flush remainder
    return len;
}

// Send command, wait, read response
uint8_t macCmd(uint16_t subcmd, uint8_t* buf, uint8_t maxLen) {
    if (!macSend(subcmd)) return 0;
    delay(20);
    return macRead(buf, maxLen);
}

// ── Status display ────────────────────────────────────────────────────────

void printStatus() {
    Serial.println(F("\n──── Battery Status ─────────────────"));

    int32_t v = readWord(R_VOLTAGE);
    if (v >= 0) {
        Serial.printf("Pack voltage : %d mV", v);
        if (v > 0) Serial.printf("  (~%.0f mV/cell)", v / 2.0f);
        Serial.println();
    } else {
        Serial.println(F("Pack voltage : READ ERROR  ← check wiring & pull-ups"));
    }

    int32_t t = readWord(R_TEMP);
    if (t >= 0) Serial.printf("Temperature  : %.1f C\n", t / 10.0f - 273.15f);

    int32_t c = readWord(R_CURRENT);
    if (c >= 0) Serial.printf("Current      : %d mA\n", (int16_t)c);

    int32_t soc = readWord(R_RSOC);
    if (soc >= 0) Serial.printf("State of chg : %d%%\n", (int)soc);

    int32_t bs = readWord(R_STATUS);
    if (bs >= 0) {
        uint8_t errCode = ((uint16_t)bs >> 12) & 0x0F;
        Serial.printf("Batt flags   : 0x%04X", (uint16_t)bs);
        if (errCode) Serial.printf("  ERR=0x%X", errCode);
        if (bs & 0x4000) Serial.print(F("  TCA"));
        if (bs & 0x2000) Serial.print(F("  OTA"));
        if (bs & 0x0800) Serial.print(F("  TDA"));
        if (bs & 0x0040) Serial.print(F("  FC=FullyCharged"));
        if (bs & 0x0020) Serial.print(F("  FD=FullyDischarged"));
        if (bs & 0x0010) Serial.print(F("  CF=ConditionFlag(PF?)"));
        Serial.println();
    }

    uint8_t buf[36] = {0};

    // Device type
    uint8_t n = macCmd(MAC_DEV_TYPE, buf, sizeof(buf));
    if (n >= 4) {
        uint16_t id = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
        const char* name = (id == 0x4307 || id == 0xFA02) ? "  (BQ9003/BQ40Z307 - OK)" : "";
        Serial.printf("Device type  : 0x%04X%s\n", id, name);
    }

    // Seal state from OperationStatus
    n = macCmd(MAC_OP_STATUS, buf, sizeof(buf));
    if (n >= 1) {
        uint8_t sec = (buf[0] >> 1) & 0x03;
        const char* sl[] = {"Full Access", "Unsealed", "?", "Sealed"};
        Serial.printf("Seal state   : %s\n", sl[sec]);
        if (buf[0] & 0x08) Serial.println(F("  CHG FET ON"));
        if (buf[0] & 0x10) Serial.println(F("  DSG FET ON"));
    }

    // Safety status (latched faults)
    n = macCmd(MAC_SAFETY_STATUS, buf, sizeof(buf));
    if (n >= 2) {
        uint16_t ss = buf[0] | ((uint16_t)buf[1] << 8);
        Serial.printf("Safety status: 0x%04X%s\n", ss, ss ? "  ← FAULTS LATCHED" : "  OK");
        if (ss & 0x0001) Serial.println(F("  → CUV: cell under-voltage latched"));
        if (ss & 0x0008) Serial.println(F("  → COV: cell over-voltage latched"));
        if (ss & 0x0010) Serial.println(F("  → OCC: overcurrent charge"));
        if (ss & 0x0020) Serial.println(F("  → OCD: overcurrent discharge"));
    }

    // Permanent Fail status
    n = macCmd(MAC_PF_STATUS, buf, sizeof(buf));
    if (n >= 4) {
        uint32_t pf = (uint32_t)buf[0] | ((uint32_t)buf[1]<<8)
                    | ((uint32_t)buf[2]<<16) | ((uint32_t)buf[3]<<24);
        Serial.printf("PF status    : 0x%08X%s\n", pf,
                      pf ? "  ← PERMANENT FAIL (needs clearing)" : "  OK");
    }

    Serial.println(F("─────────────────────────────────────\n"));
}

// ── Recovery operations ───────────────────────────────────────────────────

// Check seal state by probing 0x44 — sealed chips NACK writes to 0x44
// sec: 3 = sealed, 1 = unsealed, 0 = full access (we treat 0 and 1 the same here)
bool readSealState(uint8_t& sec) {
    Wire.beginTransmission(BATT_ADDR);
    Wire.write(0x44);
    Wire.write(0x01); Wire.write(0x00);   // MAC_DEV_TYPE probe
    bool acked = (Wire.endTransmission() == 0);
    delay(10);
    if (!acked) { sec = 3; return true; } // NACK = still sealed

    // ACK means 0x44 is accessible — try reading OperationStatus for detail
    uint8_t buf[8] = {0};
    uint8_t n = macCmd(MAC_OP_STATUS, buf, sizeof(buf));
    sec = (n >= 1) ? ((buf[0] >> 1) & 0x03) : 1;
    return true;
}

void doUnseal() {
    // DJI Spark confirmed unseal key: 0xCCDF7EE0, low word first
    // Source: dji-firmware-tools GitHub issue #258 (multiple independent confirmations)
    Serial.println(F("[U] Trying DJI Spark key (0x7EE0/0xCCDF = 0xCCDF7EE0, low word first)..."));
    mac00Write(KEY_SPARK_1); delay(15);
    mac00Write(KEY_SPARK_2); delay(150);

    uint8_t sec = 3;
    readSealState(sec);
    if (sec != 3) {
        Serial.printf("[OK] Unsealed with Spark key! State: %s\n",
                      sec == 0 ? "Full Access" : "Unsealed");
        if (sec != 0) {
            // Write Spark key pair a second time to escalate to Full Access.
            // DJI Spark appears to use the same key for both Unseal and Full Access.
            Serial.println(F("[U] Re-applying Spark key for Full Access..."));
            mac00Write(KEY_SPARK_1); delay(15);
            mac00Write(KEY_SPARK_2); delay(150);
            readSealState(sec);
            Serial.printf("[%s] State: %s\n",
                          sec == 0 ? "OK" : "!",
                          sec == 0 ? "Full Access" : "Unsealed only (PF clear will proceed; may need FA)");
        }
        return;
    }

    // Fallback: standard TI defaults
    Serial.println(F("[U] Spark key failed — trying TI defaults (0x0414/0x3672)..."));
    mac00Write(KEY_UNSEAL_1); delay(15);
    mac00Write(KEY_UNSEAL_2); delay(150);
    readSealState(sec);
    if (sec == 3) {
        Serial.println(F("[!] Still sealed after both key attempts."));
        Serial.println(F("    Use option K to enter a custom key pair."));
        return;
    }
    Serial.printf("[OK] Unsealed with TI keys! State: %s\n",
                  sec == 0 ? "Full Access" : "Unsealed");

    if (sec != 0) {
        // Try FA escalation with TI defaults
        Serial.println(F("[U] Trying Full Access escalation (0xFFFF/0xFFFF)..."));
        mac00Write(KEY_FULL_1); delay(15);
        mac00Write(KEY_FULL_2); delay(150);
        readSealState(sec);
        Serial.printf("[%s] State now: %s\n",
                      sec == 0 ? "OK" : "!",
                      sec == 0 ? "Full Access" : "Unsealed (no FA key worked)");
    }
}

void doCustomUnseal() {
    Serial.println(F("[K] Enter custom key 1 (hex, e.g. 0414): "));
    while (!Serial.available());
    String s1 = Serial.readStringUntil('\n');
    s1.trim();
    uint16_t k1 = (uint16_t)strtoul(s1.c_str(), nullptr, 16);

    Serial.println(F("Enter custom key 2: "));
    while (!Serial.available());
    String s2 = Serial.readStringUntil('\n');
    s2.trim();
    uint16_t k2 = (uint16_t)strtoul(s2.c_str(), nullptr, 16);

    Serial.printf("[K] Trying 0x%04X / 0x%04X ...\n", k1, k2);
    mac00Write(k1); delay(15);
    mac00Write(k2); delay(150);

    uint8_t sec = 3;
    if (readSealState(sec))
        Serial.printf("[%s] Seal state: %d\n", sec < 3 ? "OK" : "!", sec);
}

// Try all three PF-clear approaches and report exactly which ACK/NACK.
// DJI Battery Killer works in Unsealed mode, so 0x002A/0x002B via 0x44 are the most likely path.
void doClearPF() {
    // Timeouts (not NACKs) on these commands mean the chip accepted and is clock-stretching
    // during a flash write — that is expected and means the command ran.
    Serial.println(F("[P] Sending PF clear commands (timeouts = chip processing, that's OK):"));

    bool ok1 = macSend(MAC_PF_CLEAR);            // 0x002A → reg 0x44
    Serial.printf("[P]   0x002A → reg 0x44 : %s\n", ok1 ? "ACK" : "NACK");
    delay(300);

    bool ok2 = macSend(MAC_PF2_CLEAR);           // 0x002B → reg 0x44
    Serial.printf("[P]   0x002B → reg 0x44 : %s\n", ok2 ? "ACK" : "NACK");
    delay(300);

    bool ok3 = mac00Write(MAC_PF_DATA_RESET);    // 0x0029 → reg 0x00 (needs FA but try anyway)
    Serial.printf("[P]   0x0029 → reg 0x00 : %s\n", ok3 ? "ACK" : "NACK");
    delay(1000);

    uint8_t buf[8] = {0};
    uint8_t n = macCmd(MAC_PF_STATUS, buf, sizeof(buf));
    if (n < 2) {
        Serial.println(F("[?] PF status unreadable after commands."));
        Serial.println(F("    Run R, then U, then 1 to re-check PF status after reset."));
        return;
    }
    uint32_t pf = (uint32_t)buf[0] | ((uint32_t)buf[1]<<8)
                | ((uint32_t)buf[2]<<16) | ((uint32_t)buf[3]<<24);
    if (!pf) Serial.println(F("[OK] All PF flags cleared!"));
    else     Serial.printf("[?] PF still: 0x%08X\n", pf);
}

void doClearPF2() {
    Serial.println(F("[2] Running same PF clear sequence (clears all PF bits)..."));
    doClearPF();
}

void doReset() {
    Serial.println(F("[R] Sending chip reset..."));
    mac00Write(MAC_RESET);
    delay(2500);
    int32_t v = readWord(R_VOLTAGE);
    if (v >= 0) Serial.printf("[OK] BMS restarted. Voltage: %d mV\n", v);
    else         Serial.println(F("[!] No reply after reset — normal if very low voltage."));
}

void doSeal() {
    Serial.println(F("[L] Sealing battery..."));
    // First attempt — may NACK if chip is guarding while PF is active; retry once
    mac00Write(MAC_SEAL);
    delay(500);
    uint8_t sec = 3;
    readSealState(sec);
    if (sec != 3) {
        Serial.println(F("[L] First seal attempt pending — retrying..."));
        mac00Write(MAC_SEAL);
        delay(500);
        readSealState(sec);
    }
    Serial.printf("[%s] Seal state: %s\n",
                  sec == 3 ? "OK" : "!",
                  sec == 3 ? "Sealed" : "Not sealed — chip may need Reset first");
}

void scanBus() {
    Serial.println(F("[S] Scanning I2C (1–126)..."));
    bool found = false;
    for (uint8_t a = 1; a < 127; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            Serial.printf("  0x%02X", a);
            if (a == BATT_ADDR) Serial.print(F(" ← DJI BMS"));
            Serial.println();
            found = true;
        }
    }
    if (!found) {
        Serial.println(F("  Nothing found!"));
        Serial.println(F("  Check: SDA→Pin6, SCL→Pin1, GND→Pin2, pull-ups fitted?"));
        Serial.println(F("  If battery is dead, apply 9V boost to Pin3(+) / Pin2(−)."));
    }
}

void autoRecover() {
    Serial.println(F("\n[A] AUTO RECOVERY — mirrors DJI Battery Killer sequence"));
    Serial.println(F("    Unseal → Clear PF → Clear PF2 → Reset → Seal\n"));

    int32_t v = readWord(R_VOLTAGE);
    if (v < 0) {
        Serial.println(F("[!] No I2C response. Apply 9V boost first:"));
        Serial.println(F("    Touch 9V+ to Pin 3, 9V− to Pin 2, hold while running this."));
        return;
    }
    Serial.printf("[*] Pack voltage before: %d mV\n", v);

    doUnseal();   delay(400);
    doClearPF();  delay(400);
    doClearPF2(); delay(400);
    doReset();    delay(2500);
    doSeal();

    v = readWord(R_VOLTAGE);
    Serial.printf("[*] Pack voltage after:  %d mV\n\n", v);
    Serial.println(F("[DONE] Now plug into the DJI charger."));
    Serial.println(F("       Normal charging = alternating LEDs."));
    Serial.println(F("       Still flashing 1+2 = try Auto again, or hold 9V longer."));
}

// ── Arduino entry points ──────────────────────────────────────────────────

void printMenu() {
    Serial.println(F("\n═══════════════════════════════════════"));
    Serial.println(F("  DJI Spark Battery Recovery — ESP32"));
    Serial.println(F("═══════════════════════════════════════"));
    Serial.println(F("  1  Read battery status"));
    Serial.println(F("  S  Scan I2C bus"));
    Serial.println(F("  U  Unseal (Spark: 0xCCDF7EE0, fallback TI defaults)"));
    Serial.println(F("  K  Unseal with custom key pair"));
    Serial.println(F("  P  Clear PF  (Permanent Fail 1)"));
    Serial.println(F("  2  Clear PF2 (Permanent Fail 2)"));
    Serial.println(F("  R  Reset chip"));
    Serial.println(F("  L  Seal (re-lock)"));
    Serial.println(F("  A  Auto: full recovery sequence"));
    Serial.println(F("───────────────────────────────────────"));
    Serial.print(F("Choice: "));
}

void setup() {
    Serial.begin(115200);
    delay(1200);

    Serial.println(F("\n╔═══════════════════════════════════════╗"));
    Serial.println(F("║  DJI Spark Battery Recovery — ESP32   ║"));
    Serial.println(F("║  BQ40Z307 via I2C  (no CP2112 needed) ║"));
    Serial.println(F("╚═══════════════════════════════════════╝"));
    Serial.println(F("  SDA → GPIO21,  SCL → GPIO22,  GND → GND"));
    Serial.println(F("  4.7 kΩ pull-ups on SDA & SCL to 3.3 V"));
    Serial.println(F("  9V boost → Battery Pin3(+) / Pin2(−)\n"));

    Wire.begin(SDA_PIN, SCL_PIN, 100000); // 100 kHz = SMBus speed
    Wire.setTimeOut(6000);               // allow up to 6s clock-stretch (PF flash write)

    scanBus();
    printMenu();
}

void loop() {
    if (!Serial.available()) return;

    char c = toupper((char)Serial.read());
    while (Serial.available()) Serial.read(); // flush rest of line

    Serial.println(c);
    switch (c) {
        case '1': printStatus();    break;
        case 'S': scanBus();        break;
        case 'U': doUnseal();       break;
        case 'K': doCustomUnseal(); break;
        case 'P': doClearPF();      break;
        case '2': doClearPF2();     break;
        case 'R': doReset();        break;
        case 'L': doSeal();         break;
        case 'A': autoRecover();    break;
        default:  break;
    }
    printMenu();
}
