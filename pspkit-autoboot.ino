/*
 * pspkit-autoboot — ESP32-C3 battery emulator inside a PSP battery shell,
 * boots the PSP every time it gets power. PSP-1000 tested (key ID 0x00);
 * PSP-2000/3000 use the same handshake, untested. No PSP Go (opcode 0x90).
 *
 * Based on Baryon ESPer by Nyxef (GPL-3.0):
 *   https://github.com/NyxefTheRealOne/Baryon_ESPer/
 * a port of BaryonSweeper by khubik2: https://github.com/khubik2/pysweeper
 *
 * Syscon only boots after authenticating the battery on the DATA pin
 * (single-wire UART, 19200 baud). The battery's serial number picks the boot
 * path: 0xFFFFFFFF = service mode (IPL from the Memory Stick), 0x00000000 =
 * autoboot (NAND), anything else = normal battery.
 *
 * Wiring (ESP32-C3 SuperMini, see README):
 *   GPIO4 (TX) ──|◄── 1N4148 ──┬── GPIO5 (RX) ── DATA
 *   GPIO7 ────── 10 kΩ ────────┘
 *   GND ── "−",  5V ──►|──►| ── "+" (2,2 kΩ to "−")
 * The diode makes TX open-drain, so everything we send echoes back on RX.
 *
 * Boot sequence, switched together with the PSP's DC supply:
 *   1. DATA dead (no pull-up, UART off) for FIRST_INSERT_MS
 *   2. pull-up + UART on = "battery inserted", Syscon authenticates us
 *   3. Syscon silent for RETRY_SILENCE_MS before the boot → re-insert;
 *      after a completed authentication wait AUTH_WAIT_MS instead (the PSP
 *      needs more than 3 s from 0x81 to its first status poll)
 *   4. first status poll = PSP is running → only answer from now on;
 *      after BOOT_WINDOW_MS without a boot, stop re-inserting
 *
 * Long-term operation: WiFi/BT never started, nothing written to flash, no
 * heap in the loop, task watchdog reboots a hung loop, millis() differences
 * only (overflow safe).
 *
 * Arduino: board "ESP32C3 Dev Module", USB CDC On Boot enabled. Log over USB
 * at 115200 baud: "< " = from the PSP, "> " = our reply, [ms] = uptime.
 */

#include <Arduino.h>
#include <esp_task_wdt.h>
#include "mbedtls/aes.h"

// ─── Configuration ─────────────────────────────────────────────

#define SERVICE_MODE  0   // S/N 0xFFFFFFFF: IPL from the Memory Stick (DC-ARK)
#define AUTOBOOT      1   // S/N 0x00000000: boot from NAND
#define NORMAL_BOOT   2   // S/N 0x12345678: behaves like a normal battery

#define BOOT_MODE   AUTOBOOT        // ← change this line to switch modes

#define PSP_RX_PIN        5       // DATA
#define PSP_TX_PIN        4       // DATA through the diode
#define PULLUP_PIN        7       // 10k pull-up of the DATA line
#define FIRST_INSERT_MS   500     // after power-on: DATA dead, then battery "inserted"
#define RETRY_SILENCE_MS  3000    // re-insert when Syscon is silent this long before the first auth; measured: 3 s beats 6 s and 10 s
#define AUTH_WAIT_MS      15000   // after an auth the PSP continues on its own after 4.0 s or 10.0 s; re-insert only if it hangs
#define BOOT_WINDOW_MS    90000   // stop re-inserting this long after power-on
#define CPU_MHZ           80      // plenty for 19200 baud + AES
#define WDT_TIMEOUT_MS    10000   // reboot if the main loop hangs
#ifndef DIAG
#define DIAG              0       // 1 = keep a timeline of the last boots in flash (see below)
#endif

#if BOOT_MODE == SERVICE_MODE
  static const uint8_t SERIAL_NUMBER[4] = {0xFF, 0xFF, 0xFF, 0xFF};
#elif BOOT_MODE == AUTOBOOT
  static const uint8_t SERIAL_NUMBER[4] = {0x00, 0x00, 0x00, 0x00};
#else
  static const uint8_t SERIAL_NUMBER[4] = {0x12, 0x34, 0x56, 0x78};
#endif

// ─── Keys ──────────────────────────────────────────────────────
// Opcode 0x80 carries Syscon's key ID; each ID has an AES-128 key and two
// 8-byte challenge secrets. All known IDs are kept, the log shows which one
// this PSP uses.

struct KeyEntry    { uint8_t id; uint8_t key[16]; };
struct SecretEntry { uint8_t id; uint8_t secret[8]; };

static const KeyEntry KEYSTORE[] = {
  {0x00,{0x5C,0x52,0xD9,0x1C,0xF3,0x82,0xAC,0xA4,0x89,0xD8,0x81,0x78,0xEC,0x16,0x29,0x7B}},
  {0x01,{0x9D,0x4F,0x50,0xFC,0xE1,0xB6,0x8E,0x12,0x09,0x30,0x7D,0xDB,0xA6,0xA5,0xB5,0xAA}},
  {0x02,{0x09,0x75,0x98,0x88,0x64,0xAC,0xF7,0x62,0x1B,0xC0,0x90,0x9D,0xF0,0xFC,0xAB,0xFF}},
  {0x03,{0xC9,0x11,0x5C,0xE2,0x06,0x4A,0x26,0x86,0xD8,0xD6,0xD9,0xD0,0x8C,0xDE,0x30,0x59}},
  {0x04,{0x66,0x75,0x39,0xD2,0xFB,0x42,0x73,0xB2,0x90,0x3F,0xD7,0xA3,0x9E,0xD2,0xC6,0x0C}},
  {0x05,{0xF4,0xFA,0xEF,0x20,0xF4,0xDB,0xAB,0x31,0xD1,0x86,0x74,0xFD,0x8F,0x99,0x05,0x66}},
  {0x06,{0xEA,0x0C,0x81,0x13,0x63,0xD7,0xE9,0x30,0xF9,0x61,0x13,0x5A,0x4F,0x35,0x2D,0xDC}},
  {0x08,{0x0A,0x2E,0x73,0x30,0x5C,0x38,0x2D,0x4F,0x31,0x0D,0x0A,0xED,0x84,0xA4,0x18,0x00}},
  {0x09,{0xD2,0x04,0x74,0x30,0x8F,0xE2,0x69,0x04,0x6E,0xD7,0xBB,0x07,0xCF,0x1C,0xFF,0x43}},
  {0x0A,{0xAC,0x00,0xC0,0xE3,0xE8,0x0A,0xF0,0x68,0x3F,0xDD,0x17,0x45,0x19,0x45,0x43,0xBD}},
  {0x0B,{0x01,0x77,0xD7,0x50,0xBD,0xFD,0x2B,0xC1,0xA0,0x49,0x3A,0x13,0x4A,0x4C,0x6A,0xCF}},
  {0x0C,{0x05,0x34,0x91,0x70,0x93,0x93,0x45,0xEE,0x95,0x1A,0x14,0x84,0x33,0x34,0xA0,0xDE}},
  {0x0D,{0xDF,0xF3,0xFC,0xD6,0x08,0xB0,0x55,0x97,0xCF,0x09,0xA2,0x3B,0xD1,0x7D,0x3F,0xD2}},
  {0x2F,{0x4A,0xA7,0xC7,0xB0,0x11,0x34,0x46,0x6F,0xAC,0x82,0x16,0x3E,0x4B,0xB5,0x1B,0xF9}},
  {0x97,{0xCA,0xC8,0xB8,0x7A,0xCD,0x9E,0xC4,0x96,0x90,0xAB,0xE0,0x81,0x39,0x20,0xB1,0x10}},
  {0xB3,{0x03,0xBE,0xB6,0x54,0x99,0x14,0x04,0x83,0xBA,0x18,0x7A,0x64,0xEF,0x90,0x26,0x1D}},
  {0xD9,{0xC7,0xAC,0x13,0x06,0xDE,0xFE,0x39,0xEC,0x83,0xA1,0x48,0x3B,0x0E,0xE2,0xEC,0x89}},
  {0xEB,{0x41,0x84,0x99,0xBE,0x9D,0x35,0xA3,0xB9,0xFC,0x6A,0xD0,0xD6,0xF0,0x41,0xBB,0x26}},
};

static const SecretEntry CHALLENGE1_SECRET[] = {
  {0x00,{0xD2,0x07,0x22,0x53,0xA4,0xF2,0x74,0x68}},
  {0x01,{0xB3,0x7A,0x16,0xEF,0x55,0x7B,0xD0,0x89}},
  {0x02,{0xA0,0x4E,0x32,0xBB,0xA7,0x13,0x9E,0x46}},
  {0x03,{0xB0,0xB8,0x09,0x83,0x39,0x89,0xFA,0xE2}},
  {0x04,{0xFE,0x7D,0x78,0x99,0xBF,0xEC,0x47,0xC5}},
  {0x05,{0x30,0x6F,0x3A,0x03,0xD8,0x6C,0xBE,0xE4}},
  {0x06,{0x84,0x22,0xDF,0xEA,0xE2,0x1B,0x63,0xC2}},
  {0x08,{0xAD,0x40,0x43,0xB2,0x56,0xEB,0x45,0x8B}},
  {0x0A,{0xC2,0x37,0x7E,0x8A,0x74,0x09,0x6C,0x5F}},
  {0x0D,{0x58,0x1C,0x7F,0x19,0x44,0xF9,0x62,0x62}},
  {0x2F,{0xF1,0xBC,0x56,0x2B,0xD5,0x5B,0xB0,0x77}},
  {0x97,{0xAF,0x60,0x10,0xA8,0x46,0xF7,0x41,0xF3}},
  {0xB3,{0xDB,0xD3,0xAE,0xA4,0xDB,0x04,0x64,0x10}},
  {0xD9,{0x90,0xE1,0xF0,0xC0,0x01,0x78,0xE3,0xFF}},
  {0xEB,{0x0B,0xD9,0x02,0x7E,0x85,0x1F,0xA1,0x23}},
};

static const SecretEntry CHALLENGE2_SECRET[] = {
  {0x00,{0xF5,0xD7,0xD4,0xB5,0x75,0xF0,0x8E,0x4E}},
  {0x01,{0xCC,0x69,0x95,0x81,0xFD,0x89,0x12,0x6C}},
  {0x02,{0x49,0x5E,0x03,0x47,0x94,0x93,0x1D,0x7B}},
  {0x03,{0xF4,0xE0,0x43,0x13,0xAD,0x2E,0xB4,0xDB}},
  {0x04,{0x86,0x5E,0x3E,0xEF,0x9D,0xFB,0xB1,0xFD}},
  {0x05,{0xFF,0x72,0xBD,0x2B,0x83,0xB8,0x9D,0x2F}},
  {0x06,{0x58,0xB9,0x5A,0xAE,0xF3,0x99,0xDB,0xD0}},
  {0x08,{0x67,0xC0,0x72,0x15,0xD9,0x6B,0x39,0xA1}},
  {0x0A,{0x09,0x3E,0xC5,0x19,0xAF,0x0F,0x50,0x2D}},
  {0x0D,{0x31,0x80,0x53,0x87,0x5C,0x20,0x3E,0x24}},
  {0x2F,{0x1B,0xDF,0x24,0x33,0xEB,0x29,0x15,0x5B}},
  {0x97,{0x9D,0xEE,0xC0,0x11,0x44,0xB6,0x6F,0x41}},
  {0xB3,{0xE3,0x2B,0x8F,0x56,0xB2,0x64,0x12,0x98}},
  {0xD9,{0xC3,0x4A,0x6A,0x7B,0x20,0x5F,0xE8,0xF9}},
  {0xEB,{0xF7,0x91,0xED,0x0B,0x3F,0x49,0xA4,0x48}},
};

#define LEN(a) (sizeof(a) / sizeof((a)[0]))

static const uint8_t* findKey(uint8_t id) {
  for (size_t i = 0; i < LEN(KEYSTORE); i++) if (KEYSTORE[i].id == id) return KEYSTORE[i].key;
  return nullptr;
}
static const uint8_t* findSecret(const SecretEntry* t, size_t n, uint8_t id) {
  for (size_t i = 0; i < n; i++) if (t[i].id == id) return t[i].secret;
  return nullptr;
}

// ─── Crypto ────────────────────────────────────────────────────

static void aesEcbEncrypt(const uint8_t* key, const uint8_t* in, uint8_t* out) {
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx, key, 128);
  mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, in, out);
  mbedtls_aes_free(&ctx);
}

// Transpose a 4×4 byte matrix.
static void matrixSwap(const uint8_t* in, uint8_t* out) {
  for (int i = 0; i < 16; i++) out[i] = in[(i % 4) * 4 + i / 4];
}

// Interleave two 8-byte halves into the 16-byte AES input: `a` fills rows
// 0–1 of each column, `b` rows 2–3.
static void mix(const uint8_t* a, const uint8_t* b, uint8_t* out) {
  for (int c = 0; c < 4; c++) {
    out[4*c + 0] = a[c];
    out[4*c + 1] = a[c + 4];
    out[4*c + 2] = b[c];
    out[4*c + 3] = b[c + 4];
  }
}

// ─── Bus I/O ───────────────────────────────────────────────────

static uint8_t  g_version = 0xFF;   // key ID of the current session (from 0x80)
static uint8_t  g_chall1b[16];      // second half of our 0x80 reply, input to 0x81
static uint32_t g_lastRx = 0;       // millis() of the last PSP packet (or insert)
static bool     g_authed = false;   // 0x81 answered since the last insert

// Discard our own bytes echoing back through the diode.
static void drainEcho(int n) {
  Serial1.flush();
  int got = 0;
  uint32_t t = millis();
  while (got < n && millis() - t <= 100) {
    if (Serial1.available()) { Serial1.read(); got++; t = millis(); }
  }
  if (got != n) Serial.printf("[echo] wanted %d, got %d\n", n, got);
}

// Send a complete packet as is (static replies carry their checksum).
static void sendRaw(const uint8_t* p, int len) {
  Serial.print("> ");
  for (int i = 0; i < len; i++) Serial.printf("%02X ", p[i]);
  Serial.println();
  Serial1.write(p, len);
  drainEcho(len);
}

// Send A5 <len> 06 <payload> <checksum>; checksum = 0xFF - sum of all bytes.
static void sendReply(const uint8_t* payload, int n) {
  uint8_t p[3 + 16 + 1] = {0xA5, (uint8_t)(n + 2), 0x06};
  memcpy(p + 3, payload, n);
  uint8_t sum = 0;
  for (int i = 0; i < 3 + n; i++) sum += p[i];
  p[3 + n] = 0xFF - sum;
  sendRaw(p, 4 + n);
}

bool retryDue();

// Read one PSP packet: 5A <len> <opcode> <len-2 data bytes> <checksum>.
// False on a 3 s byte timeout, a bad length, or when a re-insert is due.
static bool readPacket(uint8_t& opcode, uint8_t* mesg, int& mesgLen) {
  uint32_t t = millis();
  auto waitByte = [&]() -> int {
    while (!Serial1.available()) {
      if (millis() - t > 3000 || retryDue()) return -1;
      esp_task_wdt_reset();
      delay(1);
    }
    t = millis();
    return Serial1.read();
  };

  int b;
  while ((b = waitByte()) != 0x5A) {           // sync on the header
    if (b < 0) return false;
    Serial.printf("[sync] skip %02X\n", b);
  }
  int len = waitByte(); if (len < 0) return false;
  int op  = waitByte(); if (op  < 0) return false;
  mesgLen = len - 2;
  if (mesgLen < 0 || mesgLen > 126) {
    Serial.printf("[!] bad length 0x%02X\n", len);
    return false;
  }
  for (int i = 0; i < mesgLen; i++) {
    if ((b = waitByte()) < 0) return false;
    mesg[i] = b;
  }
  waitByte();                                  // checksum, not checked
  opcode   = op;
  g_lastRx = millis();

  Serial.printf("[%lu] < 5A %02X %02X", (unsigned long)g_lastRx, len, op);
  for (int i = 0; i < mesgLen; i++) Serial.printf(" %02X", mesg[i]);
  Serial.println();
  return true;
}

// ─── Opcodes ───────────────────────────────────────────────────
// PSP-1000 sequence: 0x01 capacity, 0x0C serial number, 0x80/0x81 challenge,
// then status polls. Unknown opcodes get no reply.

static void handleOpcode(uint8_t opcode, const uint8_t* mesg, int mesgLen) {
  switch (opcode) {
    case 0x01: { static const uint8_t r[] = {0xA5,0x05,0x06,0x10,0xC3,0x06,0x76}; sendRaw(r, sizeof(r)); break; }

    case 0x0C:
      sendReply(SERIAL_NUMBER, 4);
      break;

    case 0x80: {  // challenge 1: <key ID> <8-byte nonce>
      if (mesgLen < 9) { Serial.printf("[!] 0x80 short (%d)\n", mesgLen); break; }
      g_version = mesg[0];
      const uint8_t* key = findKey(g_version);
      const uint8_t* s1  = findSecret(CHALLENGE1_SECRET, LEN(CHALLENGE1_SECRET), g_version);
      Serial.printf("key ID 0x%02X\n", g_version);
      if (!key || !s1) {
        Serial.printf("[!] key ID 0x%02X unknown\n", g_version);
        static const uint8_t ff[16] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
                                       0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
        sendReply(ff, 16);
        break;
      }
      uint8_t m[16], sw[16], a[16], b[16], r[16];
      mix(s1, mesg + 1, m);
      matrixSwap(m, sw);
      aesEcbEncrypt(key, sw, a);
      aesEcbEncrypt(key, a, b);
      matrixSwap(b, g_chall1b);
      memcpy(r, a, 8);
      memcpy(r + 8, g_chall1b, 8);
      sendReply(r, 16);
      break;
    }

    case 0x81: {  // challenge 2, derived from our 0x80 reply
      const uint8_t* key = findKey(g_version);
      const uint8_t* s2  = findSecret(CHALLENGE2_SECRET, LEN(CHALLENGE2_SECRET), g_version);
      if (!key || !s2) { Serial.println("[!] 0x81 without valid 0x80"); break; }
      uint8_t m[16], sw[16], c[16], r[16];
      mix(g_chall1b, s2, m);
      matrixSwap(m, sw);
      aesEcbEncrypt(key, sw, c);
      aesEcbEncrypt(key, c, r);
      sendReply(r, 16);
      if (g_version == 0xEB || g_version == 0xB3) {   // late boards want a nudge (Baryon ESPer)
        static const uint8_t nudge[] = {0x5A, 0x02, 0x01, 0xA2};
        sendRaw(nudge, sizeof(nudge));
      }
      g_authed = true;
      break;
    }

    // Status polls, static values of a healthy battery.
    case 0x02: { static const uint8_t r[] = {0xA5,0x03,0x06,0x1B,0x36};                    sendRaw(r, sizeof(r)); break; }
    case 0x03: { static const uint8_t r[] = {0xA5,0x04,0x06,0x36,0x10,0x0A};                sendRaw(r, sizeof(r)); break; }
    case 0x04: { static const uint8_t r[] = {0xA5,0x04,0x06,0x68,0x10,0xD8};                sendRaw(r, sizeof(r)); break; }
    case 0x07: { static const uint8_t r[] = {0xA5,0x04,0x06,0x08,0x07,0x41};                sendRaw(r, sizeof(r)); break; }
    case 0x08: { static const uint8_t r[] = {0xA5,0x04,0x06,0xE2,0x04,0x6A};                sendRaw(r, sizeof(r)); break; }
    case 0x09: { static const uint8_t r[] = {0xA5,0x04,0x06,0x01,0x04,0x4B};                sendRaw(r, sizeof(r)); break; }
    case 0x0B: { static const uint8_t r[] = {0xA5,0x04,0x06,0x0F,0x00,0x41};                sendRaw(r, sizeof(r)); break; }
    case 0x0D: { static const uint8_t r[] = {0xA5,0x07,0x06,0x9D,0x10,0x10,0x28,0x14,0x54}; sendRaw(r, sizeof(r)); break; }
    case 0x16: {  // "SonyEnergyDevices"
      static const uint8_t r[] = {0xA5,0x13,0x06,
        0x53,0x6F,0x6E,0x79,0x45,0x6E,0x65,0x72,0x67,0x79,0x44,0x65,0x76,0x69,0x63,0x65,0x73,0x6B};
      sendRaw(r, sizeof(r)); break;
    }

    default:
      Serial.printf("[?] opcode 0x%02X len=%d\n", opcode, mesgLen);
      break;
  }
}

// ─── Diagnostics (DIAG 1 only) ─────────────────────────────────
// Records what happened in each boot (inserts, PSP packets until the PSP
// runs) and saves it to NVS once, at "PSP running" or "giving up", so a
// power cut does not lose it. Slots for the last DIAG_SLOTS boots. Over USB:
// send 'd' to dump all boots, 'c' to clear. Diagnostics only: this writes to
// flash once per boot.

#if DIAG
#include <Preferences.h>
#define DIAG_SLOTS  30
#define DIAG_EVENTS 48
struct DiagEvent { uint32_t ms; char ev; uint8_t op; };
static DiagEvent g_diag[DIAG_EVENTS];
static uint8_t   g_diagN = 0;
static bool      g_diagSaved = false;
static uint32_t  g_bootNo = 0;
static Preferences g_prefs;

static void diag(char ev, uint8_t op = 0) {
  if (g_diagSaved || g_diagN >= DIAG_EVENTS) return;
  g_diag[g_diagN++] = {millis(), ev, op};
}
static void diagSave() {
  if (g_diagSaved) return;
  char key[8];
  snprintf(key, sizeof(key), "b%u", (unsigned)(g_bootNo % DIAG_SLOTS));
  g_prefs.putBytes(key, g_diag, g_diagN * sizeof(DiagEvent));
  g_diagSaved = true;
}
static void diagBegin() {
  g_prefs.begin("diag", false);
  g_bootNo = g_prefs.getUInt("n", 0);
  g_prefs.putUInt("n", g_bootNo + 1);
}
static void diagConsole() {
  int c = Serial.read();
  if (c == 'c') { g_prefs.clear(); g_prefs.putUInt("n", g_bootNo + 1); Serial.println("diag cleared"); }
  if (c != 'd') return;
  uint32_t n = g_prefs.getUInt("n", 0);
  uint32_t first = n > DIAG_SLOTS ? n - DIAG_SLOTS : 0;
  for (uint32_t b = first; b < n; b++) {
    char key[8];
    snprintf(key, sizeof(key), "b%u", (unsigned)(b % DIAG_SLOTS));
    DiagEvent ev[DIAG_EVENTS];
    size_t len = g_prefs.getBytes(key, ev, sizeof(ev));
    Serial.printf("boot %u:", (unsigned)b);
    for (size_t i = 0; i < len / sizeof(DiagEvent); i++)
      Serial.printf(" %c%02X@%u", ev[i].ev, ev[i].op, (unsigned)ev[i].ms);
    Serial.println();
    esp_task_wdt_reset();
  }
  Serial.println("diag end");
}
#else
static inline void diag(char, uint8_t = 0) {}
static inline void diagSave() {}
static inline void diagBegin() {}
static inline void diagConsole() {}
#endif

// ─── Insert / remove ───────────────────────────────────────────

static bool g_booted  = false;   // PSP ran past authentication (status polls)
static bool g_expired = false;   // BOOT_WINDOW_MS is over

// Battery "inserted": pull-up on, UART on. The PSP sends 8E2; 8E1 is what
// works on the ESP32-C3 (the second stop bit is just idle time).
static void batteryInsert() {
  pinMode(PULLUP_PIN, OUTPUT);
  digitalWrite(PULLUP_PIN, HIGH);
  Serial1.begin(19200, SERIAL_8E1, PSP_RX_PIN, PSP_TX_PIN);
  while (Serial1.available()) Serial1.read();
  g_version = 0xFF;
  g_authed  = false;
  g_lastRx  = millis();
  diag('I');
  Serial.printf("[%lu] battery inserted\n", millis());
}

// Battery "removed": UART off, all DATA pins high-impedance.
static void batteryRemove() {
  Serial1.end();
  pinMode(PSP_RX_PIN, INPUT);
  pinMode(PSP_TX_PIN, INPUT);
  pinMode(PULLUP_PIN, INPUT);
}

// True while the PSP has not booted, the window is open and Syscon has been
// silent since the last packet or insert for RETRY_SILENCE_MS, or for
// AUTH_WAIT_MS once authentication is through.
bool retryDue() {
  if (g_booted || g_expired) return false;
  if (millis() > BOOT_WINDOW_MS) {
    g_expired = true;
    diag('G');
    diagSave();
    Serial.printf("[%lu] no boot, giving up\n", millis());
    return false;
  }
  return millis() - g_lastRx > (g_authed ? AUTH_WAIT_MS : RETRY_SILENCE_MS);
}

// ─── Main ──────────────────────────────────────────────────────

void setup() {
  // DATA dead before anything else = no battery
  pinMode(PSP_RX_PIN, INPUT);
  pinMode(PSP_TX_PIN, INPUT);
  pinMode(PULLUP_PIN, INPUT);

  setCpuFrequencyMhz(CPU_MHZ);

  esp_task_wdt_config_t wdt = {
    .timeout_ms = WDT_TIMEOUT_MS,
    .idle_core_mask = 0,
    .trigger_panic = true,
  };
  esp_task_wdt_reconfigure(&wdt);
  esp_task_wdt_add(NULL);
  diagBegin();

  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);   // never block on the log without a USB host
  Serial.printf("[%lu] pspkit-autoboot: %s, S/N %02X%02X%02X%02X, reset reason %d\n",
    millis(),
    BOOT_MODE == SERVICE_MODE ? "SERVICE" : BOOT_MODE == AUTOBOOT ? "AUTOBOOT" : "NORMAL",
    SERIAL_NUMBER[0], SERIAL_NUMBER[1], SERIAL_NUMBER[2], SERIAL_NUMBER[3],
    (int)esp_reset_reason());

  while (millis() < FIRST_INSERT_MS) { esp_task_wdt_reset(); delay(5); }
  batteryInsert();
}

void loop() {
  esp_task_wdt_reset();
  if (Serial.available()) diagConsole();

  if (retryDue()) {
    Serial.printf("[%lu] Syscon silent, re-inserting\n", millis());
    batteryRemove();
    delay(300);
    batteryInsert();
    return;
  }

  uint8_t mesg[128];
  int     mesgLen = 0;
  uint8_t opcode  = 0;
  if (!readPacket(opcode, mesg, mesgLen)) {
    while (Serial1.available()) Serial1.read();   // drop a partial packet
    return;
  }

  diag('P', opcode);

  // Anything but capacity/serial/authentication = PSP is up and polling.
  bool running = false;
  if (!g_booted && opcode != 0x01 && opcode != 0x0C && opcode != 0x80 && opcode != 0x81) {
    g_booted = running = true;
    Serial.printf("[%lu] PSP running\n", millis());
  }

  handleOpcode(opcode, mesg, mesgLen);
  if (running) diagSave();   // after the reply, so the flash write never delays it
}
