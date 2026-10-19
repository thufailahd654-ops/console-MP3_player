/*
  RETRO CONSOLE + MP3 PLAYER WITH COVER ART   (v5)
  Board : ESP32 DevKit V1 (original ESP32, dual core)
  Screen: 1.8" ST7735 128x160 (used in landscape 160x128)
  SD    : separate micro-SD card module (SPI)
  Audio : MAX98357A I2S amp (speaker)  -- or PCM5102A DAC for earphones (same pins)

  LIBRARIES (Library Manager)
    Adafruit GFX Library, Adafruit ST7735 and ST7789 Library, Adafruit BusIO
    ESP8266Audio (1.9.7 for ESP32 core 2.0.x)
    JPEGDEC  (by Larry Bank)  <-- NEW, needed for cover art

  WIRING
    TFT   : CS->5  DC(A0)->16  RST->17  SCK->18  SDA(MOSI)->23  LED->3V3  VCC->3V3  GND
    SD module: CS->13  SCK->27  MOSI->25  MISO->26  VCC->5V (or 3V3 if the module says 3.3V)  GND
    MAX98357A: BCLK->21  LRC->22  DIN->4  VIN->5V  GND  (speaker on its +/- output)
    Buzzer (passive): + -> GPIO2, - -> GND
    Joystick: VRX->34  VRY->35  +5V->3V3  GND   (SW not used)
    Buttons (other leg to GND): A->32  B->33  START->14  SELECT->15

  CONTROLS
    Joystick = D-pad, A / B buttons, START = pause
    HOLD SELECT (0.7 s) anywhere = switch Games <-> MP3 player
    MP3 mode: Up/Down = volume, Left = previous, Right = next, A = play/pause
    Music keeps playing in the background while you game.
*/

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include "AudioFileSourceSD.h"
#include "AudioFileSourceBuffer.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"
#include <JPEGDEC.h>
#include <esp_system.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ---------------- PINS ----------------
#define TFT_CS 5
#define TFT_DC 16
#define TFT_RST 17
#define SD_SCK_PIN 27
#define SD_MISO_PIN 26
#define SD_MOSI_PIN 25
#define SD_CS_PIN 13
#define I2S_BCLK 21
#define I2S_LRC 22
#define I2S_DIN 4
#define BUZZER_PIN 2
#define JOY_X 34
#define JOY_Y 35
#define PIN_A 32
#define PIN_B 33
#define PIN_START 14
#define PIN_SELECT 15

// ---------------- TWEAKS ----------------
#define JOY_THRESH 900     // how far to push the stick before it counts
#define JOY_INVERT_X 1     // these three are set for YOUR joystick orientation
#define JOY_INVERT_Y 0
#define JOY_SWAP_XY 1
#define MAX_GAIN 1.0f      // loudness at volume 10 (keep <= 1.5)
#define AUDIO_MONO 1       // 1 = mono for one speaker, 0 = stereo for earphones
#define SELECT_HOLD_MS 700
#define DISABLE_BROWNOUT 0 // 1 = switch off the brown-out reset (stop-gap only, fix your power instead)
#define MAX_TRACKS 100

#define SCRW 160
#define SCRH 128

#define C_BLACK 0x0000
#define C_WHITE 0xFFFF
#define C_RED 0xF800
#define C_GREEN 0x07E0
#define C_BLUE 0x001F
#define C_CYAN 0x07FF
#define C_YELLOW 0xFFE0
#define C_MAGENTA 0xF81F
#define C_ORANGE 0xFD20
#define C_GRAY 0x7BEF
#define C_DGRAY 0x39E7
#define C_LGRAY 0xBDF7

// ---------------- GLOBALS ----------------
Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_RST);
GFXcanvas16 cv(SCRW, SCRH);   // full-screen frame buffer (flicker-free)
GFXcanvas16 mq(150, 16);      // marquee buffer for song title
SPIClass sdSPI(HSPI);
Preferences prefs;

enum { K_UP, K_DOWN, K_LEFT, K_RIGHT, K_A, K_B, K_START, K_SELECT, K_COUNT };
bool held[K_COUNT], pressedEv[K_COUNT], repeatEv[K_COUNT];
uint32_t holdStart[K_COUNT], lastRep[K_COUNT];
int joyCx = 2048, joyCy = 2048;
int joyRawX = 0, joyRawY = 0;
bool joyCal = false;              // true once the calibration wizard has been completed
float jvx[4], jvy[4], jmag[4];    // raw stick vectors for UP, DOWN, LEFT, RIGHT
int safeLevel = 0;                // 0 normal, 1 cover art off, 2 SD/music off (after repeated crashes)
bool artEnabled = true;
String lastRst = "";

#define M_MENU 0
#define M_GAME 1
#define M_MP3 2
#define M_SETTINGS 3
int mode = M_MENU, prevMode = M_MENU;

#define NGAMES 9
#define MENU_MUSIC NGAMES
#define MENU_SET (NGAMES + 1)
#define MENU_N (NGAMES + 2)
const char* ITEM_NAME[MENU_N] = {"SNAKE", "TETRIS", "BREAKOUT", "FLAPPY", "DINO RUN", "PONG", "INVADERS", "2048", "RACER", "MUSIC", "SETTINGS"};
const uint16_t ITEM_COL[MENU_N] = {C_GREEN, C_CYAN, C_ORANGE, C_YELLOW, C_LGRAY, C_MAGENTA, 0x87F0, 0xFD20, C_RED, 0xF81F, C_GRAY};
int menuSel = 0;

int hs[NGAMES];
bool sfxOn = true;
int vol = 5;  // 0..10

int curGame = 0, score = 0, pauseSel = 0, setSel = 0;
bool paused = false, g_over = false, newHigh = false;
const char* overMsg = "";

// ---------------- SMALL HELPERS ----------------
int iMin(int a, int b) { return a < b ? a : b; }
int iMax(int a, int b) { return a > b ? a : b; }
int iClamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
// blend colour c towards t by amt (0..255)
uint16_t mix565(uint16_t c, uint16_t t, int amt) {
  int r1 = (c >> 11) & 31, g1 = (c >> 5) & 63, b1 = c & 31;
  int r2 = (t >> 11) & 31, g2 = (t >> 5) & 63, b2 = t & 31;
  int r = (r1 * (255 - amt) + r2 * amt) / 255;
  int g = (g1 * (255 - amt) + g2 * amt) / 255;
  int b = (b1 * (255 - amt) + b2 * amt) / 255;
  return (r << 11) | (g << 5) | b;
}
void txt(int x, int y, const String& s, uint16_t c, int sz) {
  cv.setTextSize(sz);
  cv.setTextColor(c);
  cv.setCursor(x, y);
  cv.print(s);
}
void txtC(int cx, int y, const String& s, uint16_t c, int sz) {
  txt(cx - (int)s.length() * 3 * sz, y, s, c, sz);
}
void txtR(int rx, int y, const String& s, uint16_t c, int sz) {
  txt(rx - (int)s.length() * 6 * sz, y, s, c, sz);
}
void drawSprite8(int x, int y, const uint8_t* s, int rows, uint16_t col) {
  for (int r = 0; r < rows; r++) {
    uint8_t b = s[r];
    for (int c = 0; c < 8; c++)
      if (b & (0x80 >> c)) cv.drawPixel(x + c, y + r, col);
  }
}

// ---------------- PARTICLES + POPUP TEXT (shared by games) ----------------
#define NPART 28
float ptx[NPART], pty[NPART], ptvx[NPART], ptvy[NPART];
int ptlife[NPART];
uint16_t ptcol[NPART];
void partClear() { for (int i = 0; i < NPART; i++) ptlife[i] = 0; }
void spawnParts(float x, float y, uint16_t col, int n) {
  for (int k = 0; k < n; k++) {
    for (int i = 0; i < NPART; i++) {
      if (ptlife[i] <= 0) {
        ptx[i] = x; pty[i] = y;
        ptvx[i] = random(-20, 21) / 10.0f;
        ptvy[i] = random(-25, 6) / 10.0f;
        ptlife[i] = random(10, 22);
        ptcol[i] = col;
        break;
      }
    }
  }
}
void partStep() {
  for (int i = 0; i < NPART; i++)
    if (ptlife[i] > 0) { ptx[i] += ptvx[i]; pty[i] += ptvy[i]; ptvy[i] += 0.15f; ptlife[i]--; }
}
void partDraw() {
  for (int i = 0; i < NPART; i++)
    if (ptlife[i] > 0) cv.fillRect((int)ptx[i], (int)pty[i], 2, 2, ptcol[i]);
}
String popS;
int popX, popY, popLife = 0;
uint16_t popCol;
void popup(int x, int y, const String& s, uint16_t c) {
  popS = s; popX = iClamp(x, 20, 140); popY = y; popLife = 34; popCol = c;
}
void popStep() { if (popLife > 0) { popLife--; if ((popLife & 1) == 0) popY--; } }
void popDraw() {
  if (popLife > 0 && !(popLife < 10 && (popLife & 1))) txtC(popX, popY, popS, popCol, 1);
}

// ---------------- SOUND (buzzer, non-blocking) ----------------
uint32_t buzzOff = 0;
void buzzInit() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(BUZZER_PIN, 2000, 8);
#else
  ledcSetup(0, 2000, 8);
  ledcAttachPin(BUZZER_PIN, 0);
#endif
}
void buzzTone(int f) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWriteTone(BUZZER_PIN, f);
#else
  ledcWriteTone(0, f);
#endif
}
void beep(int f, int ms) {
  if (!sfxOn) return;
  buzzTone(f);
  buzzOff = millis() + ms;
}

// ---------------- INPUT ----------------
void readInputs() {
  bool now[K_COUNT];
  joyRawX = analogRead(JOY_X);
  joyRawY = analogRead(JOY_Y);
  int dx = joyRawX - joyCx, dy = joyRawY - joyCy;
  if (joyCal) {
    // calibrated: how far along each recorded direction is the stick pushed? (1.0 = full throw)
    for (int d = 0; d < 4; d++) {
      float sc = ((float)dx * jvx[d] + (float)dy * jvy[d]) / (jmag[d] * jmag[d]);
      now[d] = sc > 0.45f;
    }
  } else {
    if (JOY_SWAP_XY) { int t = dx; dx = dy; dy = t; }
    if (JOY_INVERT_X) dx = -dx;
    if (JOY_INVERT_Y) dy = -dy;
    now[K_LEFT] = dx < -JOY_THRESH;
    now[K_RIGHT] = dx > JOY_THRESH;
    now[K_UP] = dy < -JOY_THRESH;
    now[K_DOWN] = dy > JOY_THRESH;
  }
  now[K_A] = !digitalRead(PIN_A);
  now[K_B] = !digitalRead(PIN_B);
  now[K_START] = !digitalRead(PIN_START);
  now[K_SELECT] = !digitalRead(PIN_SELECT);
  uint32_t t = millis();
  for (int i = 0; i < K_COUNT; i++) {
    pressedEv[i] = now[i] && !held[i];
    repeatEv[i] = pressedEv[i];
    if (pressedEv[i]) {
      holdStart[i] = t;
      lastRep[i] = t;
    } else if (now[i] && i < 4) {
      if (t - holdStart[i] > 260 && t - lastRep[i] > 70) {
        repeatEv[i] = true;
        lastRep[i] = t;
      }
    }
    held[i] = now[i];
  }
}

// ---------------- SETTINGS STORAGE ----------------
void loadSettings() {
  prefs.begin("retro", false);
  for (int i = 0; i < NGAMES; i++) {
    char k[8];
    snprintf(k, sizeof(k), "hs%d", i);
    hs[i] = prefs.getInt(k, 0);
  }
  sfxOn = prefs.getBool("sfx", true);
  vol = prefs.getInt("vol", 5);
  if (vol < 0) vol = 0;
  if (vol > 10) vol = 10;
}
void saveSettings() {
  prefs.putBool("sfx", sfxOn);
  prefs.putInt("vol", vol);
}

void loadJoyCal() {
  joyCal = prefs.getBool("jcal", false);
  if (!joyCal) return;
  for (int d = 0; d < 4; d++) {
    char kx[6], ky[6];
    snprintf(kx, sizeof(kx), "jx%d", d);
    snprintf(ky, sizeof(ky), "jy%d", d);
    jvx[d] = prefs.getInt(kx, 0);
    jvy[d] = prefs.getInt(ky, 0);
    jmag[d] = sqrtf(jvx[d] * jvx[d] + jvy[d] * jvy[d]);
    if (jmag[d] < 400) joyCal = false;
  }
}
void saveJoyCal() {
  for (int d = 0; d < 4; d++) {
    char kx[6], ky[6];
    snprintf(kx, sizeof(kx), "jx%d", d);
    snprintf(ky, sizeof(ky), "jy%d", d);
    prefs.putInt(kx, (int)jvx[d]);
    prefs.putInt(ky, (int)jvy[d]);
  }
  prefs.putBool("jcal", true);
}
const char* rstName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "POWER ON";
    case ESP_RST_SW: return "SOFTWARE";
    case ESP_RST_PANIC: return "CRASH (PANIC)";
    case ESP_RST_INT_WDT: return "WATCHDOG (INT)";
    case ESP_RST_TASK_WDT: return "WATCHDOG (TASK)";
    case ESP_RST_WDT: return "WATCHDOG";
    case ESP_RST_BROWNOUT: return "BROWNOUT (POWER)";
    case ESP_RST_EXT: return "RESET BUTTON";
    default: return "OTHER";
  }
}
bool rstAbnormal() {
  esp_reset_reason_t r = esp_reset_reason();
  return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT || r == ESP_RST_BROWNOUT;
}

// ---------------- AUDIO ENGINE (runs on core 0) ----------------
AudioFileSourceSD* aFile = nullptr;
AudioFileSourceBuffer* aBuf = nullptr;
AudioGeneratorMP3* aMp3 = nullptr;
AudioOutputI2S* aOut = nullptr;

String tracks[MAX_TRACKS];
int nTracks = 0;
bool sdOK = false;
bool audioStarted = false;

volatile int reqTrack = -1;
volatile bool wantPlay = false;
volatile int curTrack = 0;
volatile uint32_t trackSerial = 0;
volatile uint32_t aPos = 0, aSize = 1;
uint32_t aBase = 0;
int failCount = 0;

void applyVol() {
  if (aOut) aOut->SetGain(vol * MAX_GAIN / 10.0f);
}

// ---------- cover art (ID3v2 APIC -> JPEGDEC -> 64x64 RGB565) ----------
#define ART_D 64
#define MAX_ART_BYTES 1500000UL
uint16_t* artBuf[2] = {nullptr, nullptr};
volatile int artCur = 0;
volatile bool artValid = false;
File artFile;
uint32_t artOff = 0, artLen = 0, artPos = 0;
uint16_t* artDst = nullptr;
int artSw = 1, artSh = 1;
float artF = 1.0f, artOX = 0, artOY = 0;
int artReadCount = 0;

uint32_t syncsafe(const uint8_t* b) {
  return ((uint32_t)(b[0] & 0x7F) << 21) | ((uint32_t)(b[1] & 0x7F) << 14) | ((uint32_t)(b[2] & 0x7F) << 7) | (b[3] & 0x7F);
}
uint32_t be32(const uint8_t* b) {
  return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

// finds the picture inside the ID3v2 tag. tagEnd = byte where the real audio starts
bool findArt(File& f, uint32_t& tagEnd, uint32_t& off, uint32_t& len) {
  tagEnd = 0; off = 0; len = 0;
  uint8_t h[10];
  f.seek(0);
  if (f.read(h, 10) != 10) return false;
  if (h[0] != 'I' || h[1] != 'D' || h[2] != '3') return false;
  int ver = h[3];
  uint8_t flags = h[5];
  uint32_t tagSize = syncsafe(h + 6);
  tagEnd = 10 + tagSize;
  if (ver == 4 && (flags & 0x10)) tagEnd += 10;
  if (ver < 3 || ver > 4) return false;
  if (flags & 0x80) return false;  // whole-tag unsynchronisation: skip art
  uint32_t pos = 10;
  if (flags & 0x40) {  // extended header
    uint8_t e[4];
    f.seek(pos);
    if (f.read(e, 4) != 4) return false;
    uint32_t es = (ver == 4) ? syncsafe(e) : be32(e) + 4;
    pos += es;
  }
  uint32_t end = 10 + tagSize;
  while (pos + 10 <= end) {
    uint8_t fh[10];
    f.seek(pos);
    if (f.read(fh, 10) != 10) return false;
    if (fh[0] == 0) break;  // padding
    uint32_t fs = (ver == 4) ? syncsafe(fh + 4) : be32(fh + 4);
    if (fs == 0 || pos + 10 + fs > end) break;
    if (fh[0] == 'A' && fh[1] == 'P' && fh[2] == 'I' && fh[3] == 'C') {
      uint32_t dpos = pos + 10, dlen = fs;
      if (ver == 3 && (fh[9] & 0xC0)) return false;
      if (ver == 4) {
        if (fh[9] & 0x4E) return false;  // grouping / compression / encryption / unsync
        if (fh[9] & 0x01) { dpos += 4; dlen -= 4; }
      }
      uint8_t b[256];
      uint32_t n = dlen < 256 ? dlen : 256;
      f.seek(dpos);
      if (f.read(b, n) != (int)n) return false;
      uint32_t i = 1;                 // skip text-encoding byte
      int enc = b[0];
      while (i < n && b[i]) i++;      // MIME type
      i++;
      i++;                            // picture type
      if (enc == 0 || enc == 3) {     // description (1-byte terminator)
        while (i < n && b[i]) i++;
        i++;
      } else {                        // UTF-16 description (2-byte terminator)
        while (i + 1 < n && !(b[i] == 0 && b[i + 1] == 0)) i += 2;
        i += 2;
      }
      if (i >= n) return false;
      off = dpos + i;
      len = dlen - i;
      return len > 0;
    }
    pos += 10 + fs;
  }
  return false;
}

void* artOpenCb(const char* fn, int32_t* sz) {
  *sz = (int32_t)artLen;
  return (void*)&artFile;
}
void artCloseCb(void* h) {}
int32_t artReadCb(JPEGFILE* h, uint8_t* buf, int32_t len) {
  int32_t rem = (int32_t)(artLen - artPos);
  if (len > rem) len = rem;
  if (len <= 0) return 0;
  int32_t n = (int32_t)artFile.read(buf, len);
  if (n < 0) n = 0;
  artPos += n;
  if ((++artReadCount & 7) == 0) vTaskDelay(1);  // let the idle task breathe
  return n;
}
int32_t artSeekCb(JPEGFILE* h, int32_t pos) {
  if (pos < 0) pos = 0;
  if (pos > (int32_t)artLen) pos = (int32_t)artLen;
  artPos = pos;
  artFile.seek(artOff + pos);
  return pos;
}
int artDrawCb(JPEGDRAW* p) {
  if (!artDst) return 0;
  int bx = p->x, by = p->y, bw = p->iWidth, bh = p->iHeight;
  int vw = iMin(bw, artSw - bx), vh = iMin(bh, artSh - by);
  if (vw <= 0 || vh <= 0) return 1;
  int dx0 = (int)ceilf(artOX + bx * artF), dx1 = (int)ceilf(artOX + (bx + vw) * artF);
  int dy0 = (int)ceilf(artOY + by * artF), dy1 = (int)ceilf(artOY + (by + vh) * artF);
  dx0 = iMax(dx0, 0); dy0 = iMax(dy0, 0);
  dx1 = iMin(dx1, ART_D); dy1 = iMin(dy1, ART_D);
  for (int dy = dy0; dy < dy1; dy++) {
    int sy = iClamp((int)((dy - artOY) / artF), by, by + vh - 1);
    for (int dx = dx0; dx < dx1; dx++) {
      int sx = iClamp((int)((dx - artOX) / artF), bx, bx + vw - 1);
      artDst[dy * ART_D + dx] = p->pPixels[(sy - by) * bw + (sx - bx)];
    }
  }
  return 1;
}
bool artDecode(bool forceEighth) {
  JPEGDEC* jd = new JPEGDEC();
  if (!jd) return false;
  for (int i = 0; i < ART_D * ART_D; i++) artDst[i] = 0x0841;
  artFile.seek(artOff);
  artPos = 0;
  bool ok = false;
  if (jd->open("art", artOpenCb, artCloseCb, artReadCb, artSeekCb, artDrawCb)) {
    int w = jd->getWidth(), h = jd->getHeight();
    if (w > 0 && h > 0) {
      int mx = iMax(w, h), s = 1;
      if (forceEighth) s = 8;
      else if (mx / 8 >= ART_D) s = 8;
      else if (mx / 4 >= ART_D) s = 4;
      else if (mx / 2 >= ART_D) s = 2;
      artSw = (w + s - 1) / s;
      artSh = (h + s - 1) / s;
      artF = (float)ART_D / (float)iMax(artSw, artSh);
      artOX = (ART_D - artSw * artF) / 2.0f;
      artOY = (ART_D - artSh * artF) / 2.0f;
      int opt = (s == 8) ? JPEG_SCALE_EIGHTH : (s == 4) ? JPEG_SCALE_QUARTER : (s == 2) ? JPEG_SCALE_HALF : 0;
      jd->setPixelType(RGB565_LITTLE_ENDIAN);
      ok = jd->decode(0, 0, opt) != 0;
    }
    jd->close();
  }
  delete jd;
  return ok;
}
void loadArt(const String& path, uint32_t& id3End) {
  id3End = 0;
  artValid = false;
  artFile = SD.open(path.c_str());
  if (!artFile) return;
  uint32_t off = 0, len = 0;
  bool found = findArt(artFile, id3End, off, len);
  Serial.printf("art: tag end=%lu found=%d off=%lu len=%lu enabled=%d heap=%lu\n", (unsigned long)id3End, found, (unsigned long)off, (unsigned long)len, artEnabled, (unsigned long)ESP.getMaxAllocHeap());
  if (found && artEnabled && len > 16 && len <= MAX_ART_BYTES && ESP.getMaxAllocHeap() > 45000) {
    uint8_t m[3];
    artFile.seek(off);
    if (artFile.read(m, 3) == 3 && m[0] == 0xFF && m[1] == 0xD8 && m[2] == 0xFF) {  // JPEG only
      artOff = off;
      artLen = len;
      int w = 1 - artCur;
      artDst = artBuf[w];
      bool ok = false;
      if (artDst) {
        ok = artDecode(false);
        if (!ok) ok = artDecode(true);  // progressive JPEGs: DC-only thumbnail
      }
      if (ok) { artCur = w; artValid = true; }
      Serial.printf("art: decode %s\n", ok ? "OK" : "FAILED");
    }
  }
  artFile.close();
}

// ---------- track control ----------
void closeTrack() {
  if (aMp3) {
    if (aMp3->isRunning()) aMp3->stop();
    delete aMp3;
    aMp3 = nullptr;
  }
  if (aBuf) { delete aBuf; aBuf = nullptr; }
  if (aFile) { delete aFile; aFile = nullptr; }
}

void openTrack(int i) {
  closeTrack();
  String p = "/" + tracks[i];
  Serial.printf("open track %d: %s\n", i, p.c_str());
  uint32_t id3End = 0;
  loadArt(p, id3End);
  aFile = new AudioFileSourceSD(p.c_str());
  uint32_t sz = aFile->getSize();
  if (id3End > 0 && id3End < sz) aFile->seek((int32_t)id3End, SEEK_SET);  // skip the tag (and the picture)
  else id3End = 0;
  aBuf = new AudioFileSourceBuffer(aFile, 8192);
  aMp3 = new AudioGeneratorMP3();
  curTrack = i;
  aBase = id3End;
  aPos = 0;
  aSize = (sz > id3End) ? (sz - id3End) : 1;
  trackSerial++;
  bool beginOk = aMp3->begin(aBuf, aOut);
  Serial.printf("mp3 begin %s, heap=%lu\n", beginOk ? "OK" : "FAILED", (unsigned long)ESP.getFreeHeap());
  if (!beginOk) {
    closeTrack();
    failCount++;
    if (failCount < nTracks) reqTrack = (i + 1) % nTracks;
    else wantPlay = false;
  } else {
    failCount = 0;
  }
}

void audioTask(void* pv) {
  static uint32_t lastPos = 0xFFFFFFFF;
  static int stallN = 0;
  for (;;) {
    int r = reqTrack;
    if (r >= 0) {
      reqTrack = -1;
      openTrack(r);
      lastPos = 0xFFFFFFFF;
      stallN = 0;
    }
    if (aMp3 && aMp3->isRunning() && wantPlay) {
      if (!aMp3->loop()) {
        aMp3->stop();
        reqTrack = (curTrack + 1) % nTracks;  // auto-next
      } else if (aFile) {
        uint32_t p = aFile->getPos();
        aPos = (p > aBase) ? (p - aBase) : 0;
        // stall guard: a bad frame can keep the decoder spinning without reading the file
        if (p == lastPos) {
          if (++stallN > 2000) {
            Serial.println("audio stall: skipping track");
            stallN = 0;
            aMp3->stop();
            reqTrack = (curTrack + 1) % nTracks;
          }
        } else {
          lastPos = p;
          stallN = 0;
        }
      }
    } else if (aMp3 && !aMp3->isRunning() && wantPlay) {
      reqTrack = (curTrack + 1) % nTracks;
      vTaskDelay(5);
    } else {
      vTaskDelay(5);
    }
    vTaskDelay(1);   // always give the idle task time to run (prevents watchdog resets)
  }
}

String cleanName(const String& f) {
  String s = f;
  String l = s;
  l.toLowerCase();
  if (l.endsWith(".mp3")) s.remove(s.length() - 4);
  return s;
}

void scanTracks() {
  nTracks = 0;
  File root = SD.open("/");
  if (!root) return;
  File f = root.openNextFile();
  while (f && nTracks < MAX_TRACKS) {
    if (!f.isDirectory()) {
      String n = f.name();
      int sl = n.lastIndexOf('/');
      if (sl >= 0) n = n.substring(sl + 1);
      String l = n;
      l.toLowerCase();
      if (l.endsWith(".mp3") && !n.startsWith(".")) tracks[nTracks++] = n;
    }
    f.close();
    f = root.openNextFile();
  }
  root.close();
  for (int i = 0; i < nTracks - 1; i++)
    for (int j = 0; j < nTracks - 1 - i; j++) {
      String a = tracks[j], b = tracks[j + 1];
      a.toLowerCase();
      b.toLowerCase();
      if (a > b) {
        String t = tracks[j];
        tracks[j] = tracks[j + 1];
        tracks[j + 1] = t;
      }
    }
}

// tries the card at 16 MHz, then slower (long wires / cheap modules)
bool mountSD() {
  const uint32_t fr[3] = {16000000, 8000000, 4000000};
  for (int i = 0; i < 3; i++) {
    SD.end();
    if (SD.begin(SD_CS_PIN, sdSPI, fr[i]) && SD.cardType() != CARD_NONE) {
      Serial.printf("SD OK at %lu Hz, %lu MB\n", (unsigned long)fr[i], (unsigned long)(SD.cardSize() / (1024 * 1024)));
      return true;
    }
    delay(60);
  }
  Serial.println("SD mount failed");
  return false;
}
void startAudioIfReady() {
  if (nTracks > 0 && !audioStarted) {
    audioStarted = true;
    reqTrack = 0;  // pre-load first track, paused
    Serial.println("audio task start");
    xTaskCreatePinnedToCore(audioTask, "audio", 20480, NULL, 2, NULL, 0);
  }
}
void retrySD() {
  if (audioStarted || safeLevel >= 2) return;
  sdOK = mountSD();
  if (sdOK) {
    scanTracks();
    startAudioIfReady();
  }
}

// elapsed-time bookkeeping (runs every frame in every mode)
uint32_t lastBk = 0, elapsedMs = 0, seenSerial = 0;
int mScroll = 0;
void musicBookkeeping() {
  uint32_t n = millis();
  if (seenSerial != trackSerial) {
    seenSerial = trackSerial;
    elapsedMs = 0;
    mScroll = 0;
  } else if (wantPlay) {
    elapsedMs += n - lastBk;
  }
  lastBk = n;
}

// ---------------- GAME FRAMEWORK ----------------
void endGame(const char* m) {
  g_over = true;
  overMsg = m;
  newHigh = false;
  if (score > hs[curGame]) {
    hs[curGame] = score;
    newHigh = true;
    char k[8];
    snprintf(k, sizeof(k), "hs%d", curGame);
    prefs.putInt(k, score);
  }
  beep(200, 300);
}

// =====================================================================
// 0: SNAKE  (classic wrap-around, random food, timed big bonus food)
// =====================================================================
#define SN_COLS 20
#define SN_ROWS 15
#define SN_Y0 8
int8_t snx[300], sny[300];
int snLen, snDir, snNext, snDelay, snFoods, snLevel, snGrow;
int8_t fx, fy;                 // normal food
int8_t bfx, bfy;               // bonus food (2x2 cells)
bool bfOn;
uint32_t bfEnd, bfNext, snT;
const int8_t DX[4] = {1, 0, -1, 0};
const int8_t DY[4] = {0, 1, 0, -1};

bool snFree(int x, int y) {
  for (int i = 0; i < snLen; i++)
    if (snx[i] == x && sny[i] == y) return false;
  return true;
}
void snPlaceFood() {
  int guard = 0;
  do {
    fx = random(SN_COLS);
    fy = random(SN_ROWS);
    guard++;
  } while ((!snFree(fx, fy) || (bfOn && fx >= bfx && fx < bfx + 2 && fy >= bfy && fy < bfy + 2)) && guard < 500);
}
void snPlaceBonus() {
  for (int tries = 0; tries < 60; tries++) {
    int x = random(SN_COLS - 1), y = random(SN_ROWS - 1);
    bool ok = true;
    for (int a = 0; a < 2 && ok; a++)
      for (int b = 0; b < 2; b++)
        if (!snFree(x + a, y + b) || (x + a == fx && y + b == fy)) { ok = false; break; }
    if (ok) { bfx = x; bfy = y; bfOn = true; bfEnd = millis() + 6000; return; }
  }
  bfNext = millis() + 3000;
}
void snakeInit() {
  snLen = 3;
  for (int i = 0; i < 3; i++) { snx[i] = 10 - i; sny[i] = 7; }
  snDir = snNext = 0;
  snDelay = 150;
  snFoods = 0; snLevel = 1; snGrow = 0;
  bfOn = false;
  bfNext = millis() + random(7000, 12000);
  snT = millis();
  snPlaceFood();
}
void snakeUpdate() {
  popStep();
  partStep();
  if (held[K_UP] && snDir != 1) snNext = 3;
  else if (held[K_DOWN] && snDir != 3) snNext = 1;
  else if (held[K_LEFT] && snDir != 0) snNext = 2;
  else if (held[K_RIGHT] && snDir != 2) snNext = 0;
  uint32_t now = millis();
  if (!bfOn && now >= bfNext) snPlaceBonus();
  if (bfOn && now >= bfEnd) { bfOn = false; bfNext = now + random(8000, 14000); }
  if (now - snT < (uint32_t)snDelay) return;
  snT = now;
  snDir = snNext;
  // classic: no walls, the snake wraps around to the opposite side
  int nx = (snx[0] + DX[snDir] + SN_COLS) % SN_COLS;
  int ny = (sny[0] + DY[snDir] + SN_ROWS) % SN_ROWS;
  bool eat = (nx == fx && ny == fy);
  bool eatB = bfOn && nx >= bfx && nx < bfx + 2 && ny >= bfy && ny < bfy + 2;
  bool growNow = eat || eatB || snGrow > 0;
  if (snGrow > 0) snGrow--;
  int lim = growNow ? snLen : snLen - 1;
  for (int i = 0; i < lim; i++)
    if (snx[i] == nx && sny[i] == ny) { endGame("CRASH!"); return; }
  if (growNow && snLen < 299) snLen++;
  for (int i = snLen - 1; i > 0; i--) { snx[i] = snx[i - 1]; sny[i] = sny[i - 1]; }
  snx[0] = nx;
  sny[0] = ny;
  if (eat) {
    score += 10;
    snFoods++;
    beep(1200, 40);
    spawnParts(nx * 8 + 4, SN_Y0 + ny * 8 + 4, C_RED, 6);
    if (snFoods % 5 == 0) { snLevel++; if (snDelay > 55) snDelay -= 10; }
    else if (snDelay > 55) snDelay -= 2;
    snPlaceFood();
  }
  if (eatB) {
    int pts = 50 + (int)((bfEnd - now) / 60);
    score += pts;
    snGrow += 2;
    bfOn = false;
    bfNext = now + random(8000, 14000);
    beep(2000, 120);
    spawnParts(nx * 8 + 4, SN_Y0 + ny * 8 + 4, C_YELLOW, 14);
    popup(nx * 8, SN_Y0 + ny * 8 - 6, "+" + String(pts), C_YELLOW);
  }
}
void snakeDraw() {
  uint16_t t1 = rgb(14, 44, 22), t2 = rgb(10, 36, 18);
  cv.fillScreen(C_BLACK);
  for (int r = 0; r < SN_ROWS; r++)
    for (int c = 0; c < SN_COLS; c++)
      cv.fillRect(c * 8, SN_Y0 + r * 8, 8, 8, ((r + c) & 1) ? t1 : t2);
  txt(2, 0, "SCORE " + String(score), C_WHITE, 1);
  txtR(158, 0, "LV" + String(snLevel), C_YELLOW, 1);
  // normal food: apple
  int ax = fx * 8, ay = SN_Y0 + fy * 8;
  cv.fillCircle(ax + 4, ay + 5, 3, C_RED);
  cv.drawPixel(ax + 3, ay + 4, C_WHITE);
  cv.fillRect(ax + 4, ay + 1, 1, 2, rgb(120, 80, 30));
  cv.fillRect(ax + 5, ay, 2, 2, C_GREEN);
  // bonus food: big golden fruit with timer bar
  if (bfOn) {
    uint32_t now = millis();
    int remain = (int)(bfEnd - now);
    bool blink = remain < 2000 && ((now / 120) & 1);
    if (!blink) {
      int bx = bfx * 8, by = SN_Y0 + bfy * 8;
      cv.fillCircle(bx + 8, by + 8, 7, C_ORANGE);
      cv.fillCircle(bx + 8, by + 8, 5, C_YELLOW);
      cv.fillCircle(bx + 6, by + 6, 1, C_WHITE);
      cv.fillRect(bx + 7, by - 1, 2, 3, C_GREEN);
    }
    int w = (36 * remain) / 6000;
    if (w < 0) w = 0;
    cv.drawRect(60, 1, 38, 6, C_GRAY);
    cv.fillRect(61, 2, w, 4, remain > 2000 ? C_YELLOW : C_RED);
    cv.fillRect(54, 1, 4, 6, C_YELLOW);
  }
  // snake body (fades toward the tail) + head with eyes
  for (int i = snLen - 1; i >= 0; i--) {
    int x = snx[i] * 8, y = SN_Y0 + sny[i] * 8;
    int g = 215 - (i * 110) / (snLen + 1);
    uint16_t col = (i == 0) ? rgb(190, 255, 80) : rgb(30, g, 60);
    cv.fillRoundRect(x, y, 8, 8, 2, col);
    if (i > 0 && (i & 3) == 0) cv.drawPixel(x + 4, y + 4, mix565(col, C_BLACK, 90));
  }
  {
    int x = snx[0] * 8, y = SN_Y0 + sny[0] * 8;
    int e1x, e1y, e2x, e2y;
    if (snDir == 0) { e1x = x + 5; e1y = y + 1; e2x = x + 5; e2y = y + 5; }
    else if (snDir == 2) { e1x = x + 1; e1y = y + 1; e2x = x + 1; e2y = y + 5; }
    else if (snDir == 3) { e1x = x + 1; e1y = y + 1; e2x = x + 5; e2y = y + 1; }
    else { e1x = x + 1; e1y = y + 5; e2x = x + 5; e2y = y + 5; }
    cv.fillRect(e1x, e1y, 2, 2, C_WHITE);
    cv.fillRect(e2x, e2y, 2, 2, C_WHITE);
    cv.drawPixel(e1x + (snDir == 0 ? 1 : 0), e1y + (snDir == 1 ? 1 : 0), C_BLACK);
    cv.drawPixel(e2x + (snDir == 0 ? 1 : 0), e2y + (snDir == 1 ? 1 : 0), C_BLACK);
  }
  partDraw();
  popDraw();
}

// =====================================================================
// 1: TETRIS  (ghost piece, line-clear flash, bevelled blocks)
// =====================================================================
const uint16_t TPIECES[7][4] = {
  {0x0F00, 0x2222, 0x00F0, 0x4444},  // I
  {0x8E00, 0x6440, 0x0E20, 0x44C0},  // J
  {0x2E00, 0x4460, 0x0E80, 0xC440},  // L
  {0xCC00, 0xCC00, 0xCC00, 0xCC00},  // O
  {0x6C00, 0x4620, 0x06C0, 0x8C40},  // S
  {0x4E00, 0x4640, 0x0E40, 0x4C40},  // T
  {0xC600, 0x2640, 0x0C60, 0x4C80}   // Z
};
const uint16_t TCOL[7] = {C_CYAN, 0x35BF, C_ORANGE, C_YELLOW, C_GREEN, C_MAGENTA, C_RED};
uint8_t tb[20][10];
int tType, tRot, tX, tY, tNext, tLines, tLevel;
int tFlash[4], tFlashN;
uint32_t tDrop, tFlashEnd;

bool tValid(int ty, int r, int x, int y) {
  uint16_t m = TPIECES[ty][r];
  for (int i = 0; i < 16; i++) {
    if (m & (0x8000 >> i)) {
      int cx = x + (i & 3), cy = y + (i >> 2);
      if (cx < 0 || cx >= 10 || cy >= 20) return false;
      if (cy >= 0 && tb[cy][cx]) return false;
    }
  }
  return true;
}
void tSpawn() {
  tType = tNext;
  tNext = random(7);
  tRot = 0;
  tX = 3;
  tY = -1;
  tDrop = millis();
  if (!tValid(tType, tRot, tX, tY)) endGame("GAME OVER");
}
void tetrisInit() {
  memset(tb, 0, sizeof(tb));
  tLines = 0;
  tLevel = 1;
  tFlashN = 0;
  tNext = random(7);
  tSpawn();
}
void tLock() {
  uint16_t m = TPIECES[tType][tRot];
  for (int i = 0; i < 16; i++) {
    if (m & (0x8000 >> i)) {
      int cx = tX + (i & 3), cy = tY + (i >> 2);
      if (cy < 0) { endGame("GAME OVER"); return; }
      tb[cy][cx] = tType + 1;
    }
  }
  tFlashN = 0;
  for (int r = 0; r < 20; r++) {
    bool full = true;
    for (int c = 0; c < 10; c++)
      if (!tb[r][c]) { full = false; break; }
    if (full && tFlashN < 4) tFlash[tFlashN++] = r;
  }
  if (tFlashN > 0) {
    tFlashEnd = millis() + 260;
    beep(1000 + tFlashN * 200, 90);
    return;  // piece spawns after the flash animation
  }
  beep(300, 20);
  tSpawn();
}
void tetrisUpdate() {
  if (tFlashN > 0) {
    if (millis() >= tFlashEnd) {
      for (int k = 0; k < tFlashN; k++) {
        int r = tFlash[k];
        for (int rr = r; rr > 0; rr--) memcpy(tb[rr], tb[rr - 1], 10);
        memset(tb[0], 0, 10);
      }
      const int pts[5] = {0, 100, 300, 500, 800};
      score += pts[tFlashN] * tLevel;
      tLines += tFlashN;
      tLevel = tLines / 10 + 1;
      tFlashN = 0;
      tSpawn();
    }
    return;
  }
  if (repeatEv[K_LEFT] && tValid(tType, tRot, tX - 1, tY)) tX--;
  if (repeatEv[K_RIGHT] && tValid(tType, tRot, tX + 1, tY)) tX++;
  if (pressedEv[K_A] || pressedEv[K_UP]) {
    int nr = (tRot + 1) & 3;
    if (tValid(tType, nr, tX, tY)) tRot = nr;
    else if (tValid(tType, nr, tX - 1, tY)) { tX--; tRot = nr; }
    else if (tValid(tType, nr, tX + 1, tY)) { tX++; tRot = nr; }
  }
  if (pressedEv[K_B]) {  // hard drop
    while (tValid(tType, tRot, tX, tY + 1)) { tY++; score += 2; }
    tLock();
    return;
  }
  int interval = 650 - (tLevel - 1) * 55;
  if (interval < 70) interval = 70;
  bool soft = repeatEv[K_DOWN];
  if (soft || millis() - tDrop >= (uint32_t)interval) {
    tDrop = millis();
    if (tValid(tType, tRot, tX, tY + 1)) {
      tY++;
      if (soft) score += 1;
    } else {
      tLock();
    }
  }
}
void tCell(int c, int r, uint16_t col) {
  int x = 50 + c * 6, y = 4 + r * 6;
  cv.fillRect(x, y, 5, 5, col);
  uint16_t hi = mix565(col, C_WHITE, 140), lo = mix565(col, C_BLACK, 110);
  cv.drawFastHLine(x, y, 5, hi);
  cv.drawFastVLine(x, y, 5, hi);
  cv.drawFastHLine(x + 1, y + 4, 4, lo);
  cv.drawFastVLine(x + 4, y + 1, 4, lo);
}
void tetrisDraw() {
  cv.fillScreen(rgb(6, 8, 18));
  cv.drawRect(49, 3, 62, 122, C_GRAY);
  for (int r = 0; r < 20; r++)
    for (int c = 0; c < 10; c++)
      if ((r + c) & 1) cv.drawPixel(50 + c * 6 + 5, 4 + r * 6 + 5, 0x1082);
  for (int r = 0; r < 20; r++) {
    bool fl = false;
    for (int k = 0; k < tFlashN; k++)
      if (tFlash[k] == r) fl = true;
    for (int c = 0; c < 10; c++) {
      if (!tb[r][c]) continue;
      if (fl) tCell(c, r, ((millis() / 60) & 1) ? C_WHITE : TCOL[tb[r][c] - 1]);
      else tCell(c, r, TCOL[tb[r][c] - 1]);
    }
  }
  if (!g_over && tFlashN == 0) {
    uint16_t m = TPIECES[tType][tRot];
    int gy = tY;
    while (tValid(tType, tRot, tX, gy + 1)) gy++;
    if (gy != tY) {
      for (int i = 0; i < 16; i++)
        if (m & (0x8000 >> i)) {
          int cy = gy + (i >> 2);
          if (cy >= 0) cv.drawRect(50 + (tX + (i & 3)) * 6, 4 + cy * 6, 5, 5, mix565(TCOL[tType], C_BLACK, 130));
        }
    }
    for (int i = 0; i < 16; i++)
      if (m & (0x8000 >> i)) {
        int cy = tY + (i >> 2);
        if (cy >= 0) tCell(tX + (i & 3), cy, TCOL[tType]);
      }
  }
  txt(2, 6, "SCORE", C_GRAY, 1);
  txt(2, 16, String(score), C_WHITE, 1);
  txt(2, 34, "LINES", C_GRAY, 1);
  txt(2, 44, String(tLines), C_WHITE, 1);
  txt(2, 62, "LEVEL", C_GRAY, 1);
  txt(2, 72, String(tLevel), C_WHITE, 1);
  txt(2, 90, "BEST", C_GRAY, 1);
  txt(2, 100, String(hs[1]), C_YELLOW, 1);
  txt(116, 6, "NEXT", C_GRAY, 1);
  cv.drawRect(114, 16, 30, 28, C_DGRAY);
  uint16_t m = TPIECES[tNext][0];
  for (int i = 0; i < 16; i++)
    if (m & (0x8000 >> i)) cv.fillRect(118 + (i & 3) * 5, 21 + (i >> 2) * 5, 4, 4, TCOL[tNext]);
}

// =====================================================================
// 2: BREAKOUT  (power-ups, multi-ball, shield, tough bricks, levels)
// =====================================================================
#define BO_COLS 10
#define BO_ROWS 6
#define NBALL 3
#define NCAP 6
#define PADY 114
#define PADH 4
#define SHIELD_Y 122
uint8_t boHp[BO_ROWS][BO_COLS], boPw[BO_ROWS][BO_COLS];
float bbx[NBALL], bby[NBALL], bbvx[NBALL], bbvy[NBALL];
bool bbOn[NBALL];
float padX, bSpeed;
int padW, lives, bLeft, boLevel;
bool bStuck, boShield;
uint32_t tWide, tFire, tSlow;
float capX[NCAP], capY[NCAP];
int capT[NCAP];
bool capOn[NCAP];
const uint16_t BROW_COL[BO_ROWS] = {C_RED, C_ORANGE, C_YELLOW, C_GREEN, C_CYAN, C_MAGENTA};
const char CAP_CH[6] = {'S', 'M', 'W', 'F', 'L', 'T'};
const uint16_t CAP_COL[6] = {C_CYAN, C_MAGENTA, C_GREEN, C_ORANGE, C_RED, 0x5D3F};
const char* CAP_NAME[6] = {"SHIELD", "MULTI-BALL", "WIDE PADDLE", "FIREBALL", "EXTRA LIFE", "SLOW BALL"};

void boReset() {
  bStuck = true;
  for (int i = 0; i < NBALL; i++) bbOn[i] = false;
  bbOn[0] = true;
  bbvx[0] = 0; bbvy[0] = 0;
  for (int i = 0; i < NCAP; i++) capOn[i] = false;
  tWide = tFire = tSlow = 0;
  padW = 28;
  boShield = false;
}
void boLevelInit() {
  memset(boHp, 0, sizeof(boHp));
  memset(boPw, 0, sizeof(boPw));
  int pat = (boLevel - 1) % 4;
  int rowsN = (pat == 0 || pat == 3) ? 5 : 6;
  bLeft = 0;
  for (int r = 0; r < rowsN; r++)
    for (int c = 0; c < BO_COLS; c++) {
      bool put = true;
      if (pat == 2) put = ((r + c) & 1) == 0;
      if (pat == 3) put = (c >= r && c < BO_COLS - r);
      if (!put) continue;
      int hp = 1;
      if (r == 0 && boLevel > 1) hp = 2;
      if (r == 1 && boLevel > 4) hp = 2;
      boHp[r][c] = hp;
      bLeft++;
    }
  int placed = 0, guard = 0;
  while (placed < 8 && guard < 200) {
    guard++;
    int r = random(rowsN), c = random(BO_COLS);
    if (boHp[r][c] && !boPw[r][c]) {
      int t = random(10);
      int type = t < 2 ? 0 : t < 4 ? 1 : t < 6 ? 2 : t < 8 ? 3 : t == 8 ? 5 : 4;
      boPw[r][c] = type + 1;
      placed++;
    }
  }
}
void breakoutInit() {
  lives = 3;
  boLevel = 1;
  bSpeed = 2.2f;
  padX = (SCRW - 28) / 2.0f;
  boLevelInit();
  boReset();
}
void boSpawnCap(int r, int c, int type) {
  for (int i = 0; i < NCAP; i++)
    if (!capOn[i]) {
      capOn[i] = true;
      capX[i] = 5 + c * 15;
      capY[i] = 16 + r * 6;
      capT[i] = type;
      return;
    }
}
bool boBrickHit(float px, float py, bool fire) {
  if (px < 5 || py < 16) return false;
  int c = (int)((px - 5) / 15), r = (int)((py - 16) / 6);
  if (c < 0 || c >= BO_COLS || r < 0 || r >= BO_ROWS) return false;
  if (boHp[r][c] == 0) return false;
  if (fire || boHp[r][c] == 1) {
    boHp[r][c] = 0;
    bLeft--;
    score += 10 + (BO_ROWS - r) * 2;
    spawnParts(5 + c * 15 + 7, 16 + r * 6 + 2, BROW_COL[r], 5);
    if (boPw[r][c]) { boSpawnCap(r, c, boPw[r][c] - 1); boPw[r][c] = 0; }
    beep(700 + (BO_ROWS - r) * 120, 20);
  } else {
    boHp[r][c]--;
    score += 5;
    beep(300, 15);
  }
  return true;
}
void boApplyPower(int t) {
  beep(1600, 70);
  popup(80, 90, CAP_NAME[t], CAP_COL[t]);
  switch (t) {
    case 0: boShield = true; break;
    case 1: {
      int src = -1;
      for (int i = 0; i < NBALL; i++)
        if (bbOn[i] && !(i == 0 && bStuck)) { src = i; break; }
      if (src < 0) break;
      float spd = bSpeed;
      for (int k = 0; k < 2; k++) {
        for (int i = 0; i < NBALL; i++)
          if (!bbOn[i]) {
            bbOn[i] = true;
            bbx[i] = bbx[src]; bby[i] = bby[src];
            float a = (k == 0) ? -0.6f : 0.6f;
            bbvx[i] = spd * sinf(a);
            bbvy[i] = -spd * cosf(a);
            break;
          }
      }
    } break;
    case 2:
      padX -= (44 - padW) / 2.0f;
      padW = 44;
      tWide = millis() + 12000;
      break;
    case 3: tFire = millis() + 8000; break;
    case 4: if (lives < 5) lives++; break;
    case 5: tSlow = millis() + 10000; break;
  }
}
void breakoutUpdate() {
  partStep();
  popStep();
  uint32_t now = millis();
  if (padW > 28 && now >= tWide) { padX += (padW - 28) / 2.0f; padW = 28; }
  if (held[K_LEFT]) padX -= 3.5f;
  if (held[K_RIGHT]) padX += 3.5f;
  if (padX < 0) padX = 0;
  if (padX > SCRW - padW) padX = SCRW - padW;

  // falling power-up capsules
  for (int i = 0; i < NCAP; i++) {
    if (!capOn[i]) continue;
    capY[i] += 1.3f;
    if (capY[i] + 9 >= PADY && capY[i] <= PADY + PADH && capX[i] + 14 >= padX && capX[i] <= padX + padW) {
      capOn[i] = false;
      boApplyPower(capT[i]);
    } else if (capY[i] > SCRH) {
      capOn[i] = false;
    }
  }

  if (bStuck) {
    bbx[0] = padX + padW / 2.0f;
    bby[0] = PADY - 3;
    if (pressedEv[K_A] || pressedEv[K_B] || pressedEv[K_UP]) {
      bStuck = false;
      bbvx[0] = bSpeed * 0.5f * (random(2) ? 1 : -1);
      bbvy[0] = -sqrtf(bSpeed * bSpeed - bbvx[0] * bbvx[0]);
    }
    return;
  }
  float spd = bSpeed * (now < tSlow ? 0.7f : 1.0f);
  bool fire = now < tFire;
  int alive = 0;
  for (int i = 0; i < NBALL; i++) {
    if (!bbOn[i]) continue;
    float m = sqrtf(bbvx[i] * bbvx[i] + bbvy[i] * bbvy[i]);
    if (m > 0.01f) { bbvx[i] *= spd / m; bbvy[i] *= spd / m; }
    bbx[i] += bbvx[i];
    if (bbx[i] < 3) { bbx[i] = 3; bbvx[i] = -bbvx[i]; beep(400, 10); }
    if (bbx[i] > SCRW - 3) { bbx[i] = SCRW - 3; bbvx[i] = -bbvx[i]; beep(400, 10); }
    bool h = false;
    h |= boBrickHit(bbx[i] - 2, bby[i] - 2, fire);
    h |= boBrickHit(bbx[i] + 2, bby[i] - 2, fire);
    h |= boBrickHit(bbx[i] - 2, bby[i] + 2, fire);
    h |= boBrickHit(bbx[i] + 2, bby[i] + 2, fire);
    if (h && !fire) bbvx[i] = -bbvx[i];
    bby[i] += bbvy[i];
    if (bby[i] < 3) { bby[i] = 3; bbvy[i] = fabsf(bbvy[i]); beep(400, 10); }
    h = false;
    h |= boBrickHit(bbx[i] - 2, bby[i] - 2, fire);
    h |= boBrickHit(bbx[i] + 2, bby[i] - 2, fire);
    h |= boBrickHit(bbx[i] - 2, bby[i] + 2, fire);
    h |= boBrickHit(bbx[i] + 2, bby[i] + 2, fire);
    if (h && !fire) bbvy[i] = -bbvy[i];
    // paddle
    if (bbvy[i] > 0 && bby[i] + 2 >= PADY && bby[i] - 2 <= PADY + PADH && bbx[i] >= padX - 2 && bbx[i] <= padX + padW + 2) {
      float rel = (bbx[i] - (padX + padW / 2.0f)) / (padW / 2.0f);
      bbvx[i] = spd * rel * 0.9f;
      bbvy[i] = -sqrtf(fmaxf(spd * spd - bbvx[i] * bbvx[i], 1.0f));
      bby[i] = PADY - 3;
      beep(600, 15);
    }
    // shield
    if (boShield && bbvy[i] > 0 && bby[i] + 2 >= SHIELD_Y) {
      bbvy[i] = -fabsf(bbvy[i]);
      bby[i] = SHIELD_Y - 3;
      boShield = false;
      beep(1100, 60);
      spawnParts(bbx[i], SHIELD_Y, C_CYAN, 8);
    }
    if (bby[i] > SCRH + 4) bbOn[i] = false;
    else alive++;
  }
  if (alive == 0) {
    lives--;
    if (lives <= 0) endGame("GAME OVER");
    else { boReset(); beep(150, 200); }
    return;
  }
  if (bLeft <= 0) {
    boLevel++;
    bSpeed = fminf(2.2f + 0.25f * (boLevel - 1), 4.0f);
    boLevelInit();
    boReset();
    popup(80, 90, "LEVEL " + String(boLevel), C_WHITE);
    beep(1500, 200);
  }
}
void breakoutDraw() {
  uint32_t now = millis();
  cv.fillScreen(rgb(6, 6, 20));
  txt(2, 0, "SC " + String(score), C_WHITE, 1);
  txt(54, 0, "LV" + String(boLevel), C_GRAY, 1);
  String hearts = "";
  for (int i = 0; i < lives; i++) hearts += (char)3;
  txtR(158, 0, hearts, C_RED, 1);
  // active power timers
  int bx0 = 84;
  if (now < tWide) { cv.fillRect(bx0, 1, (12 * (int)(tWide - now)) / 12000 + 1, 5, C_GREEN); bx0 += 14; }
  if (now < tFire) { cv.fillRect(bx0, 1, (12 * (int)(tFire - now)) / 8000 + 1, 5, C_ORANGE); bx0 += 14; }
  if (now < tSlow) { cv.fillRect(bx0, 1, (12 * (int)(tSlow - now)) / 10000 + 1, 5, 0x5D3F); bx0 += 14; }
  if (boShield) cv.fillRect(bx0, 1, 12, 5, C_CYAN);
  // bricks
  for (int r = 0; r < BO_ROWS; r++)
    for (int c = 0; c < BO_COLS; c++) {
      if (!boHp[r][c]) continue;
      int x = 5 + c * 15, y = 16 + r * 6;
      uint16_t col = BROW_COL[r];
      if (boHp[r][c] == 2) {
        cv.fillRect(x, y, 14, 5, mix565(col, C_LGRAY, 150));
        cv.drawRect(x, y, 14, 5, C_WHITE);
      } else {
        cv.fillRect(x, y, 14, 5, col);
        cv.drawFastHLine(x, y, 14, mix565(col, C_WHITE, 140));
        cv.drawFastHLine(x, y + 4, 14, mix565(col, C_BLACK, 120));
      }
      if (boPw[r][c] && ((now / 250) & 1)) cv.fillRect(x + 6, y + 1, 2, 3, C_WHITE);
    }
  // capsules
  for (int i = 0; i < NCAP; i++) {
    if (!capOn[i]) continue;
    cv.fillRoundRect((int)capX[i], (int)capY[i], 14, 9, 3, CAP_COL[capT[i]]);
    cv.drawRoundRect((int)capX[i], (int)capY[i], 14, 9, 3, C_WHITE);
    txt((int)capX[i] + 4, (int)capY[i] + 1, String(CAP_CH[capT[i]]), C_BLACK, 1);
  }
  if (boShield) {
    uint16_t sc = ((now / 120) & 1) ? C_CYAN : C_WHITE;
    cv.drawFastHLine(0, SHIELD_Y, SCRW, sc);
    cv.drawFastHLine(0, SHIELD_Y + 1, SCRW, mix565(sc, C_BLACK, 120));
  }
  // paddle
  uint16_t pc = (now < tFire) ? C_ORANGE : C_WHITE;
  cv.fillRoundRect((int)padX, PADY, padW, PADH, 2, pc);
  cv.drawFastHLine((int)padX + 2, PADY, padW - 4, mix565(pc, C_BLACK, 90));
  // balls
  bool fire = now < tFire;
  for (int i = 0; i < NBALL; i++) {
    if (!bbOn[i]) continue;
    int x = (int)bbx[i], y = (int)bby[i];
    if (fire) {
      cv.fillCircle(x, y, 3, C_ORANGE);
      cv.fillCircle(x, y, 1, C_YELLOW);
      cv.fillRect(x - 1, y - (int)(bbvy[i] * 2) - 1, 2, 2, mix565(C_ORANGE, C_BLACK, 120));
    } else {
      cv.fillRect(x - 2, y - 2, 4, 4, C_WHITE);
      cv.drawPixel(x - 2, y - 2, C_LGRAY);
    }
  }
  if (bStuck) txtC(80, 96, "PRESS A TO LAUNCH", C_GRAY, 1);
  partDraw();
  popDraw();
}

// =====================================================================
// 3: FLAPPY  (day/night, clouds, scrolling ground, medals)
// =====================================================================
#define F_PW 22
#define F_GROUND 116
#define F_BX 40
#define F_GAP_BASE 44
float fyb, fvy;
int pipeX[3], pipeGap[3], pipeG[3];
bool pipeScored[3], fStarted;
int fFrame, fGroundOff;
float cloudX[4];
int cloudY[4], starX[14], starY[14];

int fGapFor() {
  int g = F_GAP_BASE - score / 4;
  return g < 34 ? 34 : g;
}
void flappyInit() {
  fyb = 60; fvy = 0; fStarted = false; fFrame = 0; fGroundOff = 0;
  for (int i = 0; i < 3; i++) {
    pipeX[i] = SCRW + 30 + i * 80;
    pipeG[i] = F_GAP_BASE;
    pipeGap[i] = random(14, F_GROUND - pipeG[i] - 14);
    pipeScored[i] = false;
  }
  for (int i = 0; i < 4; i++) { cloudX[i] = i * 45 + random(20); cloudY[i] = random(10, 60); }
  for (int i = 0; i < 14; i++) { starX[i] = random(SCRW); starY[i] = random(4, 80); }
}
void flappyUpdate() {
  fFrame++;
  for (int i = 0; i < 4; i++) { cloudX[i] -= 0.4f; if (cloudX[i] < -30) cloudX[i] = SCRW + random(30); }
  if (pressedEv[K_A] || pressedEv[K_UP] || pressedEv[K_B]) {
    fvy = -3.6f;
    fStarted = true;
    beep(900, 25);
  }
  if (!fStarted) { fyb = 60 + sinf(millis() / 200.0f) * 4; return; }
  fGroundOff = (fGroundOff + 2) % 16;
  fvy += 0.38f;
  if (fvy > 6) fvy = 6;
  fyb += fvy;
  if (fyb < 4) { fyb = 4; fvy = 0; }
  for (int i = 0; i < 3; i++) {
    pipeX[i] -= 2;
    if (pipeX[i] < -F_PW) {
      pipeX[i] += 240;
      pipeG[i] = fGapFor();
      pipeGap[i] = random(14, F_GROUND - pipeG[i] - 14);
      pipeScored[i] = false;
    }
    if (!pipeScored[i] && pipeX[i] + F_PW < F_BX - 4) {
      pipeScored[i] = true;
      score++;
      beep(1500, 30);
      if (score % 10 == 0) popup(80, 40, "NICE!", C_YELLOW);
    }
    if (F_BX + 4 > pipeX[i] && F_BX - 4 < pipeX[i] + F_PW) {
      if (fyb - 4 < pipeGap[i] || fyb + 4 > pipeGap[i] + pipeG[i]) { endGame("OUCH!"); return; }
    }
  }
  popStep();
  if (fyb + 4 >= F_GROUND) endGame("OUCH!");
}
void flappyDraw() {
  bool night = ((score / 10) & 1) == 1;
  uint16_t sky = night ? rgb(10, 14, 44) : rgb(80, 190, 225);
  cv.fillScreen(sky);
  if (night) {
    for (int i = 0; i < 14; i++) cv.drawPixel(starX[i], starY[i], ((i + fFrame / 15) & 3) ? C_WHITE : C_GRAY);
    cv.fillCircle(132, 20, 8, C_LGRAY);
    cv.fillCircle(135, 18, 7, sky);
  } else {
    uint16_t cc = rgb(240, 248, 255);
    for (int i = 0; i < 4; i++) {
      int cx = (int)cloudX[i], cy = cloudY[i];
      cv.fillRoundRect(cx, cy, 26, 8, 4, cc);
      cv.fillRoundRect(cx + 6, cy - 4, 14, 8, 4, cc);
    }
  }
  uint16_t pg = night ? rgb(30, 110, 50) : rgb(60, 170, 60);
  uint16_t pd = night ? rgb(15, 70, 30) : rgb(30, 120, 30);
  uint16_t ph = night ? rgb(60, 150, 80) : rgb(130, 220, 100);
  for (int i = 0; i < 3; i++) {
    int g = pipeGap[i], gh = pipeG[i];
    cv.fillRect(pipeX[i], 0, F_PW, g, pg);
    cv.fillRect(pipeX[i] + 3, 0, 3, g, ph);
    cv.fillRect(pipeX[i] - 2, g - 6, F_PW + 4, 6, pd);
    cv.fillRect(pipeX[i] - 1, g - 5, 3, 4, ph);
    cv.fillRect(pipeX[i], g + gh, F_PW, F_GROUND - g - gh, pg);
    cv.fillRect(pipeX[i] + 3, g + gh, 3, F_GROUND - g - gh, ph);
    cv.fillRect(pipeX[i] - 2, g + gh, F_PW + 4, 6, pd);
    cv.fillRect(pipeX[i] - 1, g + gh + 1, 3, 4, ph);
  }
  uint16_t gd = night ? rgb(120, 100, 50) : rgb(222, 184, 90);
  cv.fillRect(0, F_GROUND, SCRW, SCRH - F_GROUND, gd);
  cv.fillRect(0, F_GROUND, SCRW, 3, night ? rgb(40, 120, 40) : rgb(90, 190, 60));
  for (int x = -fGroundOff; x < SCRW; x += 16) cv.fillRect(x, F_GROUND + 4, 8, 3, night ? rgb(90, 70, 30) : rgb(190, 150, 60));
  // bird with flapping wing
  int by_ = (int)fyb;
  cv.fillCircle(F_BX, by_, 5, C_YELLOW);
  cv.fillCircle(F_BX - 1, by_ + 2, 3, rgb(255, 235, 140));
  bool up = fvy < 0 || (!fStarted && ((fFrame / 6) & 1));
  cv.fillRect(F_BX - 5, up ? by_ - 2 : by_ + 1, 5, 3, C_ORANGE);
  cv.fillCircle(F_BX + 2, by_ - 2, 2, C_WHITE);
  cv.drawPixel(F_BX + 3, by_ - 2, C_BLACK);
  cv.fillRect(F_BX + 5, by_, 4, 3, C_RED);
  txtC(80, 6, String(score), C_WHITE, 2);
  if (!fStarted) txtC(80, 90, "PRESS A TO FLAP", C_WHITE, 1);
  popDraw();
}

// =====================================================================
// 4: DINO RUN  (day/night, clouds, dust, flashing milestones)
// =====================================================================
#define D_GROUND 100
#define D_X 18
float dY, dVy, dSpeed, dDist;
bool dDuck, dWasAir;
float obXf[3], dcX[3];
int obY[3], obW[3], obH[3], obT[3], dcY[3];
bool obOn[3];
int dNextGap, dFrame, dFlash, dLastHundred, dStarX[10], dStarY[10];

void dinoInit() {
  dY = 0; dVy = 0; dSpeed = 3; dDist = 0; dDuck = false; dFrame = 0; dWasAir = false;
  dFlash = 0; dLastHundred = 0;
  for (int i = 0; i < 3; i++) { obOn[i] = false; dcX[i] = i * 60 + random(30); dcY[i] = random(12, 55); }
  for (int i = 0; i < 10; i++) { dStarX[i] = random(SCRW); dStarY[i] = random(10, 70); }
  dNextGap = 60;
}
void dSpawn() {
  for (int i = 0; i < 3; i++) {
    if (!obOn[i]) {
      obOn[i] = true;
      obXf[i] = SCRW + 4;
      int r = random(10);
      if (score > 100 && r < 3) {
        obT[i] = 2; obW[i] = 16; obH[i] = 10;
        obY[i] = D_GROUND - (random(2) ? 24 : 46);
      } else if (r < 6) {
        obT[i] = 0; obW[i] = 6; obH[i] = 14; obY[i] = D_GROUND - 14;
      } else if (r < 9) {
        obT[i] = 1; obW[i] = 9; obH[i] = 22; obY[i] = D_GROUND - 22;
      } else {
        obT[i] = 3; obW[i] = 18; obH[i] = 14; obY[i] = D_GROUND - 14;
      }
      return;
    }
  }
}
void dinoUpdate() {
  partStep();
  for (int i = 0; i < 3; i++) { dcX[i] -= 0.5f; if (dcX[i] < -26) { dcX[i] = SCRW + random(40); dcY[i] = random(12, 55); } }
  bool air = dY > 0;
  if (!air && (pressedEv[K_A] || pressedEv[K_UP] || pressedEv[K_B])) {
    dVy = 6.8f;
    air = true;
    beep(700, 30);
  }
  dDuck = held[K_DOWN] && !air;
  if (air) {
    if (held[K_DOWN]) dVy -= 0.9f;
    dVy -= 0.6f;
    dY += dVy;
    if (dY <= 0) { dY = 0; dVy = 0; }
  }
  if (dWasAir && dY <= 0) spawnParts(D_X + 8, D_GROUND - 1, C_GRAY, 5);
  dWasAir = dY > 0;
  dDist += dSpeed;
  score = (int)(dDist / 8);
  dSpeed = fminf(7.0f, 3.0f + score * 0.004f);
  dFrame++;
  if (score / 100 > dLastHundred) { dLastHundred = score / 100; dFlash = 24; beep(1000, 60); }
  if (dFlash > 0) dFlash--;
  float lastEdge = -999;
  bool any = false;
  for (int i = 0; i < 3; i++) {
    if (!obOn[i]) continue;
    obXf[i] -= dSpeed;
    if (obXf[i] + obW[i] < 0) { obOn[i] = false; continue; }
    any = true;
    float e = obXf[i] + obW[i];
    if (e > lastEdge) lastEdge = e;
  }
  if (!any || lastEdge < SCRW - dNextGap) {
    dSpawn();
    dNextGap = random(70, 130) + (int)(dSpeed * 18);
  }
  int dw = dDuck ? 18 : 14, dh = dDuck ? 9 : 16;
  int top = D_GROUND - dh - (int)dY;
  for (int i = 0; i < 3; i++) {
    if (!obOn[i]) continue;
    if (D_X + 2 < obXf[i] + obW[i] - 2 && D_X + dw - 2 > obXf[i] + 2 &&
        top + 2 < obY[i] + obH[i] - 2 && top + dh - 1 > obY[i] + 2) {
      spawnParts(D_X + 8, top + 6, C_GRAY, 10);
      endGame("OUCH!");
      return;
    }
  }
}
void dinoDraw() {
  bool nt = ((score / 400) & 1) == 1;
  uint16_t bg = nt ? rgb(18, 20, 32) : rgb(247, 247, 247);
  uint16_t fg = nt ? rgb(215, 215, 215) : rgb(83, 83, 83);
  uint16_t soft = mix565(bg, fg, 70);
  cv.fillScreen(bg);
  if (nt) {
    for (int i = 0; i < 10; i++) cv.drawPixel(dStarX[i], dStarY[i], soft);
    cv.fillCircle(132, 24, 8, fg);
    cv.fillCircle(136, 22, 7, bg);
  }
  for (int i = 0; i < 3; i++) {
    int cx = (int)dcX[i], cy = dcY[i];
    cv.fillRoundRect(cx, cy, 24, 6, 3, soft);
    cv.fillRoundRect(cx + 6, cy - 3, 12, 5, 2, soft);
  }
  txt(2, 2, "HI " + String(hs[4]), soft, 1);
  if (!(dFlash > 0 && (dFlash & 4))) txtR(158, 2, String(score), fg, 1);
  cv.drawFastHLine(0, D_GROUND, SCRW, fg);
  int off = (int)dDist;
  for (int x = 0; x < SCRW; x += 4) {
    int p = (x + off) % 24;
    if (p < 2) cv.drawFastHLine(x, D_GROUND + 4, 3, soft);
    if (p == 10) cv.drawFastHLine(x, D_GROUND + 9, 2, soft);
    if (p == 18) cv.drawPixel(x, D_GROUND - 1, fg);
  }
  int dh = dDuck ? 9 : 16;
  int top = D_GROUND - dh - (int)dY;
  if (dDuck) {
    cv.fillRect(D_X, top + 2, 18, 7, fg);
    cv.fillRect(D_X + 12, top, 6, 5, fg);
    cv.fillRect(D_X + 15, top + 1, 1, 1, bg);
    cv.fillRect(D_X + 17, top + 4, 3, 1, fg);
    int lg = (dFrame / 3) & 1;
    cv.fillRect(D_X + 4, top + 9 - lg, 2, 1 + lg, fg);
    cv.fillRect(D_X + 11, top + 8 + lg - 1, 2, 2 - lg, fg);
  } else {
    cv.fillRect(D_X + 3, top + 5, 9, 8, fg);
    cv.fillRect(D_X + 6, top, 9, 7, fg);
    cv.fillRect(D_X + 11, top + 2, 2, 2, bg);
    cv.fillRect(D_X + 13, top + 5, 3, 2, fg);
    cv.fillRect(D_X, top + 7, 3, 3, fg);
    cv.fillRect(D_X + 1, top + 5, 2, 2, fg);
    cv.fillRect(D_X + 11, top + 8, 3, 2, fg);
    int a = (dY > 0) ? 0 : ((dFrame / 3) & 1);
    cv.fillRect(D_X + 4, top + 13, 2, 3 - a, fg);
    cv.fillRect(D_X + 9, top + 13, 2, 2 + a, fg);
  }
  for (int i = 0; i < 3; i++) {
    if (!obOn[i]) continue;
    int x = (int)obXf[i], y = obY[i];
    if (obT[i] == 2) {
      cv.fillRect(x, y + 4, 14, 4, fg);
      cv.fillRect(x + 12, y + 2, 4, 3, fg);
      cv.drawPixel(x + 14, y + 3, bg);
      if ((dFrame / 4) & 1) cv.fillRect(x + 3, y, 6, 4, fg);
      else cv.fillRect(x + 3, y + 8, 6, 3, fg);
    } else if (obT[i] == 3) {
      cv.fillRect(x + 2, y, 4, obH[i], fg);
      cv.fillRect(x + 11, y + 2, 4, obH[i] - 2, fg);
      cv.fillRect(x, y + 5, 3, 2, fg);
      cv.fillRect(x + 15, y + 6, 3, 2, fg);
    } else {
      cv.fillRect(x + obW[i] / 2 - 1, y, 3, obH[i], fg);
      if (obT[i] == 1) {
        cv.fillRect(x, y + 6, 3, 2, fg);
        cv.fillRect(x, y + 3, 2, 4, fg);
        cv.fillRect(x + obW[i] - 3, y + 9, 3, 2, fg);
        cv.fillRect(x + obW[i] - 2, y + 6, 2, 4, fg);
      }
    }
  }
  partDraw();
}

// =====================================================================
// 5: PONG  (difficulty select, ball trail, sparks, rally counter)
// =====================================================================
#define PONG_PW 4
#define PONG_PH 24
float pbx, pby, pvx, pvy, pSpd, pY, cY, cMaxSp, cErr;
float pTrX[6], pTrY[6];
int pPts, cPts, pDiff, pState, pRally, pHitP, pHitC;

void pongSetDiff() {
  const float ms[3] = {1.8f, 2.4f, 3.2f};
  cMaxSp = ms[pDiff];
}
void pongServe(int dir) {
  pbx = SCRW / 2;
  pby = SCRH / 2;
  pSpd = 2.6f;
  pRally = 0;
  float ang = random(-30, 31) * 0.01745f;
  pvx = dir * pSpd * cosf(ang);
  pvy = pSpd * sinf(ang);
  cErr = random(-8, 9);
  for (int i = 0; i < 6; i++) { pTrX[i] = pbx; pTrY[i] = pby; }
}
void pongInit() {
  pY = cY = (SCRH - PONG_PH) / 2.0f;
  pPts = cPts = 0;
  pDiff = 1; pState = 0; pHitP = pHitC = 0;
  pongSetDiff();
  pongServe(1);
}
void pongUpdate() {
  partStep();
  if (pState == 0) {
    if (pressedEv[K_LEFT] && pDiff > 0) pDiff--;
    if (pressedEv[K_RIGHT] && pDiff < 2) pDiff++;
    if (pressedEv[K_A]) { pongSetDiff(); pState = 1; beep(1200, 40); }
    return;
  }
  if (pHitP > 0) pHitP--;
  if (pHitC > 0) pHitC--;
  if (held[K_UP]) pY -= 3.2f;
  if (held[K_DOWN]) pY += 3.2f;
  if (pY < 0) pY = 0;
  if (pY > SCRH - PONG_PH) pY = SCRH - PONG_PH;
  float target = (pvx > 0) ? (pby - PONG_PH / 2.0f + cErr) : ((SCRH - PONG_PH) / 2.0f);
  float diff = target - cY;
  float ms = (pvx > 0) ? cMaxSp : 1.0f;
  if (diff > ms) diff = ms;
  if (diff < -ms) diff = -ms;
  cY += diff;
  if (cY < 0) cY = 0;
  if (cY > SCRH - PONG_PH) cY = SCRH - PONG_PH;
  for (int i = 5; i > 0; i--) { pTrX[i] = pTrX[i - 1]; pTrY[i] = pTrY[i - 1]; }
  pTrX[0] = pbx; pTrY[0] = pby;
  pbx += pvx;
  pby += pvy;
  if (pby < 2) { pby = 2; pvy = -pvy; beep(400, 10); }
  if (pby > SCRH - 2) { pby = SCRH - 2; pvy = -pvy; beep(400, 10); }
  if (pvx < 0 && pbx - 2 <= 10 && pbx - 2 >= 2 && pby >= pY - 2 && pby <= pY + PONG_PH + 2) {
    float rel = (pby - (pY + PONG_PH / 2.0f)) / (PONG_PH / 2.0f);
    pSpd = fminf(pSpd + 0.25f, 6.0f);
    float a = rel * 0.9f;
    pvx = pSpd * cosf(a);
    pvy = pSpd * sinf(a);
    pbx = 13;
    pRally++;
    pHitP = 6;
    spawnParts(pbx, pby, C_CYAN, 6);
    beep(600, 15);
  }
  if (pvx > 0 && pbx + 2 >= SCRW - 10 && pbx + 2 <= SCRW - 2 && pby >= cY - 2 && pby <= cY + PONG_PH + 2) {
    float rel = (pby - (cY + PONG_PH / 2.0f)) / (PONG_PH / 2.0f);
    pSpd = fminf(pSpd + 0.25f, 6.0f);
    float a = rel * 0.9f;
    pvx = -pSpd * cosf(a);
    pvy = pSpd * sinf(a);
    pbx = SCRW - 13;
    pRally++;
    pHitC = 6;
    int e = 10 - pDiff * 3;
    cErr = random(-e, e + 1);
    spawnParts(pbx, pby, C_MAGENTA, 6);
    beep(500, 15);
  }
  if (pbx < -4) {
    cPts++;
    beep(180, 150);
    if (cPts >= 7) endGame("CPU WINS");
    else pongServe(-1);
  }
  if (pbx > SCRW + 4) {
    pPts++;
    score += 10 * (pDiff + 1);
    beep(1300, 100);
    if (pPts >= 7) { score += 50 * (pDiff + 1); endGame("YOU WIN!"); }
    else pongServe(1);
  }
}
void pongDraw() {
  cv.fillScreen(rgb(4, 6, 16));
  cv.drawFastHLine(0, 0, SCRW, C_DGRAY);
  cv.drawFastHLine(0, SCRH - 1, SCRW, C_DGRAY);
  for (int y = 0; y < SCRH; y += 8) cv.fillRect(SCRW / 2 - 1, y, 2, 4, C_DGRAY);
  txt(SCRW / 2 - 30, 4, String(pPts), C_WHITE, 2);
  txt(SCRW / 2 + 18, 4, String(cPts), C_WHITE, 2);
  if (pRally > 2) txtC(80, 24, "RALLY " + String(pRally), C_GRAY, 1);
  cv.fillRoundRect(6, (int)pY, PONG_PW, PONG_PH, 2, pHitP ? C_CYAN : C_WHITE);
  cv.fillRoundRect(SCRW - 10, (int)cY, PONG_PW, PONG_PH, 2, pHitC ? C_MAGENTA : C_WHITE);
  if (pState == 1) {
    for (int i = 5; i >= 1; i--) {
      int s = 4 - i / 2;
      cv.fillRect((int)pTrX[i] - s / 2, (int)pTrY[i] - s / 2, s, s, mix565(C_YELLOW, C_BLACK, 40 + i * 35));
    }
    cv.fillRect((int)(pbx - 2), (int)(pby - 2), 4, 4, C_YELLOW);
  }
  partDraw();
  if (pState == 0) {
    cv.fillRect(26, 34, 108, 62, C_BLACK);
    cv.drawRect(26, 34, 108, 62, C_WHITE);
    txtC(80, 40, "PONG", C_CYAN, 2);
    const char* dn[3] = {"EASY", "NORMAL", "HARD"};
    for (int i = 0; i < 3; i++) {
      uint16_t c = (i == pDiff) ? C_GREEN : C_DGRAY;
      txt(34 + i * 34, 64, dn[i], c, 1);
    }
    txtC(80, 80, "< > CHOOSE   A: START", C_GRAY, 1);
  }
}

// =====================================================================
// 6: SPACE INVADERS  (bunkers, UFO, waves, 3 alien types)
// =====================================================================
#define INV_COLS 8
#define INV_ROWS 4
#define INV_SX 15
#define INV_SY 11
#define BUNK_Y 98
const uint8_t SPR_INV[3][2][6] = {
  {{0x18, 0x3C, 0x7E, 0xDB, 0xFF, 0x24}, {0x18, 0x3C, 0x7E, 0xDB, 0xFF, 0x5A}},
  {{0x24, 0x18, 0x7E, 0xDB, 0xFF, 0xA5}, {0x24, 0x99, 0xFF, 0xDB, 0x7E, 0x42}},
  {{0x3C, 0x7E, 0xFF, 0xA5, 0xFF, 0x5A}, {0x3C, 0x7E, 0xFF, 0xA5, 0xFF, 0xA5}}
};
const uint8_t INV_TYPE[INV_ROWS] = {0, 1, 1, 2};
const int INV_PTS[3] = {30, 20, 10};
const uint16_t INV_COL[3] = {C_MAGENTA, C_CYAN, C_GREEN};
const uint16_t BUNK_PAT[8] = {0x0FF0, 0x3FFC, 0x7FFE, 0xFFFF, 0xFFFF, 0xFFFF, 0xFC3F, 0xF00F};
bool invAlive[INV_ROWS][INV_COLS];
int invX, invY, invDir, invFrame, invCnt, invWave, invLives, invFlash, plX, pbX, pbY, ufoX, ufoDir, ufoPts;
int abX[3], abY[3];
bool pbOn, abOn[3], ufoOn;
uint16_t bunk[4][8];
uint32_t invT, invNextUfo;

void bunkReset() {
  for (int i = 0; i < 4; i++)
    for (int r = 0; r < 8; r++) bunk[i][r] = BUNK_PAT[r];
}
bool bunkHit(int x, int y) {
  for (int i = 0; i < 4; i++) {
    int bx = 19 + i * 35;
    if (x >= bx && x < bx + 16 && y >= BUNK_Y && y < BUNK_Y + 8) {
      int px = x - bx, py = y - BUNK_Y;
      if (bunk[i][py] & (0x8000 >> px)) {
        for (int dy = -1; dy <= 1; dy++)
          for (int dx = -1; dx <= 1; dx++) {
            int qx = px + dx, qy = py + dy;
            if (qx >= 0 && qx < 16 && qy >= 0 && qy < 8) bunk[i][qy] &= ~(0x8000 >> qx);
          }
        int ex = px + random(-2, 3), ey = py + random(-2, 3);
        if (ex >= 0 && ex < 16 && ey >= 0 && ey < 8) bunk[i][ey] &= ~(0x8000 >> ex);
        return true;
      }
    }
  }
  return false;
}
void invSetupWave() {
  for (int r = 0; r < INV_ROWS; r++)
    for (int c = 0; c < INV_COLS; c++) invAlive[r][c] = true;
  invCnt = INV_ROWS * INV_COLS;
  invX = 20;
  invY = 20 + (invWave > 3 ? 3 : invWave) * 4;
  invDir = 1;
  invFrame = 0;
  invT = millis();
  pbOn = false;
  for (int i = 0; i < 3; i++) abOn[i] = false;
  if (invWave % 3 == 0) bunkReset();
  ufoOn = false;
  invNextUfo = millis() + random(12000, 20000);
}
void invadersInit() {
  invWave = 0; invLives = 3; plX = 80; invFlash = 0;
  bunkReset();
  invSetupWave();
}
void invadersUpdate() {
  partStep();
  popStep();
  if (invFlash > 0) invFlash--;
  if (held[K_LEFT]) plX -= 3;
  if (held[K_RIGHT]) plX += 3;
  plX = iClamp(plX, 8, 152);
  if ((held[K_A] || held[K_B] || pressedEv[K_UP]) && !pbOn) {
    pbOn = true; pbX = plX; pbY = 112;
    beep(1800, 20);
  }
  // player bullet
  if (pbOn) {
    pbY -= 5;
    if (pbY < 0) pbOn = false;
    else if (bunkHit(pbX, pbY) || bunkHit(pbX, pbY + 3)) pbOn = false;
    else {
      for (int r = 0; r < INV_ROWS && pbOn; r++)
        for (int c = 0; c < INV_COLS; c++) {
          if (!invAlive[r][c]) continue;
          int ax = invX + c * INV_SX, ay = invY + r * INV_SY;
          if (pbX >= ax && pbX < ax + 8 && pbY + 3 >= ay && pbY < ay + 6) {
            invAlive[r][c] = false;
            invCnt--;
            pbOn = false;
            int t = INV_TYPE[r];
            score += INV_PTS[t];
            spawnParts(ax + 4, ay + 3, INV_COL[t], 8);
            beep(250, 40);
            break;
          }
        }
      if (pbOn && ufoOn && pbX >= ufoX && pbX < ufoX + 16 && pbY >= 8 && pbY < 18) {
        score += ufoPts;
        popup(ufoX + 8, 20, "+" + String(ufoPts), C_RED);
        spawnParts(ufoX + 8, 14, C_RED, 14);
        ufoOn = false;
        pbOn = false;
        invNextUfo = millis() + random(15000, 25000);
        beep(2200, 120);
      }
    }
  }
  // formation movement (speeds up as aliens die)
  int interval = 40 + invCnt * 13 - invWave * 12;
  if (interval < 30) interval = 30;
  if (millis() - invT >= (uint32_t)interval) {
    invT = millis();
    invFrame ^= 1;
    int minC = INV_COLS, maxC = -1;
    for (int r = 0; r < INV_ROWS; r++)
      for (int c = 0; c < INV_COLS; c++)
        if (invAlive[r][c]) { if (c < minC) minC = c; if (c > maxC) maxC = c; }
    if (maxC >= 0) {
      int dx = invDir * 3;
      int left = invX + minC * INV_SX + dx, right = invX + maxC * INV_SX + 8 + dx;
      if (left < 2 || right > 158) { invY += 6; invDir = -invDir; }
      else invX += dx;
      beep(invFrame ? 110 : 90, 25);
      for (int r = 0; r < INV_ROWS; r++)
        for (int c = 0; c < INV_COLS; c++)
          if (invAlive[r][c] && invY + r * INV_SY + 6 >= 110) { endGame("INVADED!"); return; }
    }
  }
  // alien fire
  int maxB = 2 + (invWave >= 2 ? 1 : 0);
  int active = 0;
  for (int i = 0; i < 3; i++) if (abOn[i]) active++;
  int chance = 28 - invWave * 3;
  if (chance < 10) chance = 10;
  if (active < maxB && invCnt > 0 && random(chance) == 0) {
    int c = random(INV_COLS);
    for (int k = 0; k < INV_COLS; k++) {
      int cc = (c + k) % INV_COLS;
      int lowR = -1;
      for (int r = INV_ROWS - 1; r >= 0; r--) if (invAlive[r][cc]) { lowR = r; break; }
      if (lowR >= 0) {
        for (int i = 0; i < 3; i++)
          if (!abOn[i]) { abOn[i] = true; abX[i] = invX + cc * INV_SX + 4; abY[i] = invY + lowR * INV_SY + 6; break; }
        break;
      }
    }
  }
  int bspd = 2 + (invWave >= 3 ? 1 : 0);
  for (int i = 0; i < 3; i++) {
    if (!abOn[i]) continue;
    abY[i] += bspd;
    if (abY[i] >= 126) { abOn[i] = false; continue; }
    if (bunkHit(abX[i], abY[i] + 3) || bunkHit(abX[i], abY[i] + 1)) { abOn[i] = false; continue; }
    if (invFlash == 0 && abX[i] >= plX - 6 && abX[i] <= plX + 6 && abY[i] + 3 >= 114 && abY[i] < 122) {
      abOn[i] = false;
      invLives--;
      spawnParts(plX, 118, C_GREEN, 16);
      beep(150, 300);
      invFlash = 60;
      for (int k = 0; k < 3; k++) abOn[k] = false;
      if (invLives <= 0) { endGame("INVADED!"); return; }
    }
  }
  // UFO
  if (!ufoOn && millis() >= invNextUfo) {
    ufoOn = true;
    ufoDir = random(2) ? 1 : -1;
    ufoX = ufoDir > 0 ? -16 : 160;
    ufoPts = (random(4) + 1) * 50;
  }
  if (ufoOn) {
    ufoX += ufoDir * 2;
    if (ufoX < -18 || ufoX > 162) { ufoOn = false; invNextUfo = millis() + random(15000, 25000); }
  }
  if (invCnt <= 0) {
    invWave++;
    score += 100;
    invSetupWave();
    popup(80, 60, "WAVE " + String(invWave + 1), C_WHITE);
    beep(1500, 200);
  }
}
void invadersDraw() {
  cv.fillScreen(C_BLACK);
  for (int k = 0; k < 20; k++) cv.drawPixel((k * 37) % 160, (k * 53) % 80 + 14, 0x2104);
  txt(2, 0, "SC " + String(score), C_WHITE, 1);
  txtC(88, 0, "W" + String(invWave + 1), C_GRAY, 1);
  String hearts = "";
  for (int i = 0; i < invLives; i++) hearts += (char)3;
  txtR(158, 0, hearts, C_RED, 1);
  for (int r = 0; r < INV_ROWS; r++)
    for (int c = 0; c < INV_COLS; c++)
      if (invAlive[r][c]) {
        int t = INV_TYPE[r];
        drawSprite8(invX + c * INV_SX, invY + r * INV_SY, SPR_INV[t][invFrame], 6, INV_COL[t]);
      }
  for (int i = 0; i < 4; i++) {
    int bx = 19 + i * 35;
    for (int r = 0; r < 8; r++)
      for (int c = 0; c < 16; c++)
        if (bunk[i][r] & (0x8000 >> c)) cv.drawPixel(bx + c, BUNK_Y + r, C_GREEN);
  }
  if (!(invFlash > 0 && (invFlash & 4))) {
    cv.fillRect(plX - 6, 118, 13, 4, C_GREEN);
    cv.fillRect(plX - 4, 115, 9, 3, C_GREEN);
    cv.fillRect(plX - 1, 112, 3, 3, C_GREEN);
  }
  if (pbOn) cv.fillRect(pbX, pbY, 1, 4, C_WHITE);
  for (int i = 0; i < 3; i++)
    if (abOn[i]) {
      cv.fillRect(abX[i], abY[i], 2, 4, C_YELLOW);
      cv.drawPixel(abX[i] + ((abY[i] >> 2) & 1), abY[i] + 4, C_ORANGE);
    }
  if (ufoOn) {
    cv.fillRect(ufoX + 5, 10, 6, 3, C_RED);
    cv.fillRect(ufoX + 2, 13, 12, 3, C_RED);
    cv.fillRect(ufoX, 16, 16, 2, C_RED);
    cv.drawPixel(ufoX + 4, 14, C_YELLOW);
    cv.drawPixel(ufoX + 8, 14, C_YELLOW);
    cv.drawPixel(ufoX + 12, 14, C_YELLOW);
  }
  cv.drawFastHLine(0, 124, SCRW, C_GREEN);
  partDraw();
  popDraw();
}

// =====================================================================
// 7: 2048
// =====================================================================
uint8_t g48[4][4];
int n48r, n48c, n48a;
bool won48;
const uint16_t TILE48[12] = {0, 0xEF3B, 0xEEF9, 0xF58F, 0xF4AC, 0xF3EB, 0xF2E7, 0xEE6E, 0xEE2C, 0xEE0A, 0xEDE8, 0xEDC6};

uint8_t* cell48(int dir, int i, int k) {
  switch (dir) {
    case 0: return &g48[k][i];
    case 1: return &g48[3 - k][i];
    case 2: return &g48[i][k];
    default: return &g48[i][3 - k];
  }
}
bool slide48(int dir) {
  bool moved = false;
  for (int i = 0; i < 4; i++) {
    uint8_t line[4], out[4] = {0, 0, 0, 0};
    bool mg[4] = {false, false, false, false};
    for (int k = 0; k < 4; k++) line[k] = *cell48(dir, i, k);
    int pos = 0;
    for (int k = 0; k < 4; k++) {
      uint8_t v = line[k];
      if (!v) continue;
      if (pos > 0 && out[pos - 1] == v && !mg[pos - 1]) {
        out[pos - 1] = v + 1;
        mg[pos - 1] = true;
        score += (1 << out[pos - 1]);
      } else {
        out[pos++] = v;
      }
    }
    for (int k = 0; k < 4; k++) {
      uint8_t* p = cell48(dir, i, k);
      if (*p != out[k]) { moved = true; *p = out[k]; }
    }
  }
  return moved;
}
void spawn48() {
  int er[16], ec[16], n = 0;
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++)
      if (!g48[r][c]) { er[n] = r; ec[n] = c; n++; }
  if (n == 0) return;
  int k = random(n);
  g48[er[k]][ec[k]] = (random(10) < 9) ? 1 : 2;
  n48r = er[k]; n48c = ec[k]; n48a = 5;
}
bool canMove48() {
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++) {
      if (!g48[r][c]) return true;
      if (c < 3 && g48[r][c] == g48[r][c + 1]) return true;
      if (r < 3 && g48[r][c] == g48[r + 1][c]) return true;
    }
  return false;
}
void g2048Init() {
  memset(g48, 0, sizeof(g48));
  won48 = false;
  n48a = 0;
  spawn48();
  spawn48();
}
void g2048Update() {
  popStep();
  if (n48a > 0) n48a--;
  int d = -1;
  if (pressedEv[K_UP]) d = 0;
  else if (pressedEv[K_DOWN]) d = 1;
  else if (pressedEv[K_LEFT]) d = 2;
  else if (pressedEv[K_RIGHT]) d = 3;
  if (d < 0) return;
  int before = score;
  if (slide48(d)) {
    if (score > before) beep(900, 40);
    else beep(450, 15);
    spawn48();
    if (!won48) {
      for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
          if (g48[r][c] >= 11) {
            won48 = true;
            popup(60, 60, "2048!", C_RED);
            beep(2000, 300);
          }
    }
    if (!canMove48()) endGame("NO MOVES");
  }
}
void g2048Draw() {
  cv.fillScreen(rgb(250, 248, 239));
  cv.fillRoundRect(3, 6, 114, 114, 4, rgb(187, 173, 160));
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++) {
      int x = 5 + c * 28, y = 8 + r * 28;
      int n = g48[r][c];
      if (!n) { cv.fillRoundRect(x, y, 26, 26, 3, rgb(205, 193, 180)); continue; }
      int inset = (n48a > 0 && r == n48r && c == n48c) ? n48a : 0;
      uint16_t col = (n < 12) ? TILE48[n] : rgb(60, 58, 50);
      cv.fillRoundRect(x + inset, y + inset, 26 - 2 * inset, 26 - 2 * inset, 3, col);
      if (inset > 2) continue;
      int val = 1 << n;
      String s = String(val);
      int sz = (val < 100) ? 2 : 1;
      int w = (int)s.length() * 6 * sz, h = 8 * sz;
      txt(x + 13 - w / 2, y + 13 - h / 2, s, n <= 2 ? rgb(119, 110, 101) : C_WHITE, sz);
    }
  txt(122, 8, "SCORE", rgb(119, 110, 101), 1);
  txt(122, 18, String(score), rgb(60, 55, 50), 1);
  txt(122, 40, "BEST", rgb(119, 110, 101), 1);
  txt(122, 50, String(hs[7]), rgb(60, 55, 50), 1);
  if (won48) txt(122, 76, "2048!", C_RED, 1);
  popDraw();
}

// =====================================================================
// 8: RACER  (traffic, trucks, coins, nitro, scrolling scenery)
// =====================================================================
#define R_X0 30
#define R_X1 130
#define NENEM 5
#define R_CARW 12
#define R_CARH 22
#define R_PY 98
float rpx, rSpeed, rDist, rOff, rNitro, rSpawnAcc, rSpawnGap;
bool rNitroOn;
int rCoinScore, rFrame;
float enX[NENEM], enY[NENEM], enOwn[NENEM], coX[3], coY[3];
int enW[NENEM], enH[NENEM];
uint16_t enCol[NENEM];
bool enOn[NENEM], enTruck[NENEM], coOn[3];

int laneCx(int l) { return R_X0 + 12 + l * 25; }
void racerInit() {
  rpx = 80 - R_CARW / 2;
  rSpeed = 3; rDist = 0; rOff = 0; rNitro = 100; rNitroOn = false; rCoinScore = 0; rFrame = 0;
  rSpawnAcc = 0; rSpawnGap = 60;
  for (int i = 0; i < NENEM; i++) enOn[i] = false;
  for (int i = 0; i < 3; i++) coOn[i] = false;
}
void rSpawn() {
  int lane = random(4);
  int nearCnt = 0;
  for (int i = 0; i < NENEM; i++) if (enOn[i] && enY[i] < 70) nearCnt++;
  bool coin = random(10) < 3;
  for (int k = 0; k < 4; k++) {
    int l = (lane + k) % 4;
    bool blocked = false;
    for (int i = 0; i < NENEM; i++)
      if (enOn[i] && enY[i] < 50 && abs((int)(enX[i] + enW[i] / 2) - laneCx(l)) < 20) blocked = true;
    if (blocked) continue;
    if (coin) {
      for (int i = 0; i < 3; i++)
        if (!coOn[i]) { coOn[i] = true; coX[i] = laneCx(l); coY[i] = -8; break; }
      return;
    }
    if (nearCnt >= 2) return;
    for (int i = 0; i < NENEM; i++)
      if (!enOn[i]) {
        bool truck = (rDist > 1500) && random(5) == 0;
        enOn[i] = true;
        enTruck[i] = truck;
        enW[i] = truck ? 14 : R_CARW;
        enH[i] = truck ? 34 : R_CARH;
        enX[i] = laneCx(l) - enW[i] / 2;
        enY[i] = -enH[i];
        enOwn[i] = truck ? random(8, 13) / 10.0f : random(10, 21) / 10.0f;
        const uint16_t pal[6] = {C_BLUE, C_YELLOW, C_GREEN, C_MAGENTA, C_ORANGE, C_CYAN};
        enCol[i] = truck ? rgb(200, 200, 205) : pal[random(6)];
        return;
      }
    return;
  }
}
void racerUpdate() {
  partStep();
  popStep();
  rFrame++;
  if (held[K_LEFT]) rpx -= 2.8f;
  if (held[K_RIGHT]) rpx += 2.8f;
  if (rpx < R_X0 + 3) rpx = R_X0 + 3;
  if (rpx > R_X1 - 3 - R_CARW) rpx = R_X1 - 3 - R_CARW;
  rNitroOn = held[K_A] && rNitro > 0;
  if (rNitroOn) rNitro -= 1.2f;
  else if (rNitro < 100) rNitro += 0.3f;
  if (rNitro < 0) rNitro = 0;
  float base = 3.0f + rDist / 2500.0f;
  if (base > 6.0f) base = 6.0f;
  rSpeed = base * (rNitroOn ? 1.5f : 1.0f);
  rDist += rSpeed;
  rOff += rSpeed;
  score = (int)(rDist / 6) + rCoinScore;
  rSpawnAcc += rSpeed;
  if (rSpawnAcc >= rSpawnGap) {
    rSpawnAcc = 0;
    rSpawnGap = random(55, 100);
    rSpawn();
  }
  int px0 = (int)rpx + 2, px1 = (int)rpx + R_CARW - 2, py0 = R_PY + 3, py1 = R_PY + R_CARH - 2;
  for (int i = 0; i < NENEM; i++) {
    if (!enOn[i]) continue;
    enY[i] += rSpeed - enOwn[i];
    if (enY[i] > SCRH + 10) { enOn[i] = false; continue; }
    int ex0 = (int)enX[i] + 2, ex1 = (int)enX[i] + enW[i] - 2, ey0 = (int)enY[i] + 2, ey1 = (int)enY[i] + enH[i] - 2;
    if (px0 < ex1 && px1 > ex0 && py0 < ey1 && py1 > ey0) {
      spawnParts(rpx + 6, R_PY + 10, C_ORANGE, 14);
      spawnParts(rpx + 6, R_PY + 10, C_YELLOW, 8);
      endGame("CRASH!");
      return;
    }
  }
  for (int i = 0; i < 3; i++) {
    if (!coOn[i]) continue;
    coY[i] += rSpeed;
    if (coY[i] > SCRH + 6) { coOn[i] = false; continue; }
    if (fabsf(coX[i] - (rpx + R_CARW / 2)) < 10 && coY[i] > R_PY - 2 && coY[i] < R_PY + R_CARH) {
      coOn[i] = false;
      rCoinScore += 50;
      beep(1800, 30);
      popup((int)coX[i], R_PY - 10, "+50", C_YELLOW);
    }
  }
}
void drawCar(int x, int y, int w, int h, uint16_t col, bool truck) {
  uint16_t win = rgb(150, 205, 235), dwin = rgb(40, 70, 110);
  if (truck) {
    cv.fillRect(x, y + 12, w, h - 12, rgb(215, 215, 220));
    cv.drawRect(x, y + 12, w, h - 12, C_GRAY);
    cv.fillRoundRect(x + 1, y, w - 2, 13, 2, col);
    cv.fillRect(x + 3, y + 2, w - 6, 4, win);
  } else {
    cv.fillRoundRect(x, y, w, h, 3, col);
    cv.fillRect(x + 2, y + 5, w - 4, 5, win);
    cv.fillRect(x + 2, y + h - 8, w - 4, 3, dwin);
    cv.drawFastHLine(x + 3, y + 11, w - 6, mix565(col, C_BLACK, 80));
  }
  cv.fillRect(x - 1, y + 3, 2, 6, C_BLACK);
  cv.fillRect(x + w - 1, y + 3, 2, 6, C_BLACK);
  cv.fillRect(x - 1, y + h - 9, 2, 6, C_BLACK);
  cv.fillRect(x + w - 1, y + h - 9, 2, 6, C_BLACK);
  cv.drawPixel(x + 1, y, C_YELLOW);
  cv.drawPixel(x + w - 2, y, C_YELLOW);
  cv.drawPixel(x + 1, y + h - 1, C_RED);
  cv.drawPixel(x + w - 2, y + h - 1, C_RED);
}
void racerDraw() {
  int off = (int)rOff;
  cv.fillScreen(rgb(30, 120, 40));
  uint16_t dg = rgb(24, 100, 34);
  for (int y = -32 + (off % 32); y < SCRH; y += 32) {
    cv.fillRect(0, y, R_X0, 16, dg);
    cv.fillRect(R_X1, y, SCRW - R_X1, 16, dg);
  }
  for (int k = 0; k < 4; k++) {
    int ty = ((k * 48 + off) % 192) - 24;
    cv.fillRect(12, ty + 6, 3, 5, rgb(100, 70, 30));
    cv.fillCircle(13, ty, 6, rgb(20, 85, 30));
    cv.fillRect(146, ty + 26, 3, 5, rgb(100, 70, 30));
    cv.fillCircle(147, ty + 20, 6, rgb(20, 85, 30));
  }
  cv.fillRect(R_X0, 0, R_X1 - R_X0, SCRH, rgb(58, 58, 64));
  for (int y = -12 + (off % 12); y < SCRH; y += 12) {
    uint16_t kc = (((y + off) / 12) & 1) ? C_RED : C_WHITE;
    cv.fillRect(R_X0 - 2, y, 2, 12, kc);
    cv.fillRect(R_X1, y, 2, 12, kc);
  }
  for (int l = 1; l < 4; l++)
    for (int y = -24 + (off % 24); y < SCRH; y += 24) cv.fillRect(R_X0 + l * 25 - 1, y, 2, 12, rgb(230, 230, 230));
  for (int i = 0; i < 3; i++)
    if (coOn[i]) {
      cv.fillCircle((int)coX[i], (int)coY[i], 5, C_YELLOW);
      cv.fillCircle((int)coX[i], (int)coY[i], 3, C_ORANGE);
      cv.drawPixel((int)coX[i] - 1, (int)coY[i] - 1, C_WHITE);
    }
  for (int i = 0; i < NENEM; i++)
    if (enOn[i]) drawCar((int)enX[i], (int)enY[i], enW[i], enH[i], enCol[i], enTruck[i]);
  if (rNitroOn) {
    int fl = 6 + (rFrame & 3) * 2;
    cv.fillTriangle((int)rpx + 2, R_PY + R_CARH, (int)rpx + R_CARW - 3, R_PY + R_CARH, (int)rpx + R_CARW / 2 - 1, R_PY + R_CARH + fl, C_ORANGE);
    cv.fillTriangle((int)rpx + 4, R_PY + R_CARH, (int)rpx + R_CARW - 5, R_PY + R_CARH, (int)rpx + R_CARW / 2 - 1, R_PY + R_CARH + fl - 3, C_YELLOW);
  }
  if (!g_over) drawCar((int)rpx, R_PY, R_CARW, R_CARH, C_RED, false);
  partDraw();
  popDraw();
  txt(1, 2, "PTS", C_WHITE, 1);
  txt(1, 11, String(score), C_YELLOW, 1);
  txt(1, 24, "BEST", C_WHITE, 1);
  txt(1, 33, String(hs[8]), C_LGRAY, 1);
  txtC(144, 4, "N2O", C_WHITE, 1);
  cv.drawRect(138, 16, 12, 82, C_WHITE);
  int nh = (int)(80.0f * rNitro / 100.0f);
  cv.fillRect(139, 97 - nh, 10, nh, rNitroOn ? C_ORANGE : C_CYAN);
  txtC(144, 104, String((int)(rSpeed * 30)), C_WHITE, 1);
  txtC(144, 114, "KMH", C_LGRAY, 1);
}

// ---------------- GAME DISPATCH ----------------
void gameInit(int g) {
  partClear();
  popLife = 0;
  switch (g) {
    case 0: snakeInit(); break;
    case 1: tetrisInit(); break;
    case 2: breakoutInit(); break;
    case 3: flappyInit(); break;
    case 4: dinoInit(); break;
    case 5: pongInit(); break;
    case 6: invadersInit(); break;
    case 7: g2048Init(); break;
    case 8: racerInit(); break;
  }
}
void gameUpdate(int g) {
  switch (g) {
    case 0: snakeUpdate(); break;
    case 1: tetrisUpdate(); break;
    case 2: breakoutUpdate(); break;
    case 3: flappyUpdate(); break;
    case 4: dinoUpdate(); break;
    case 5: pongUpdate(); break;
    case 6: invadersUpdate(); break;
    case 7: g2048Update(); break;
    case 8: racerUpdate(); break;
  }
}
void gameDraw(int g) {
  switch (g) {
    case 0: snakeDraw(); break;
    case 1: tetrisDraw(); break;
    case 2: breakoutDraw(); break;
    case 3: flappyDraw(); break;
    case 4: dinoDraw(); break;
    case 5: pongDraw(); break;
    case 6: invadersDraw(); break;
    case 7: g2048Draw(); break;
    case 8: racerDraw(); break;
  }
}
// Music and games never run at the same time.
void pauseMusic() {
  wantPlay = false;   // audio task stops decoding and does not touch the SD card
}

void startGame(int g) {
  curGame = g;
  mode = M_GAME;
  pauseMusic();
  paused = false;
  g_over = false;
  newHigh = false;
  score = 0;
  pauseSel = 0;
  gameInit(g);
}

void gameTick() {
  if (g_over) {
    partStep();
    gameDraw(curGame);
    cv.fillRect(20, 30, 120, 66, C_BLACK);
    cv.drawRect(20, 30, 120, 66, C_WHITE);
    txtC(80, 36, overMsg, C_RED, 2);
    if (curGame == 3 && score >= 10) {  // flappy medal
      uint16_t mc = score >= 40 ? rgb(229, 228, 226) : score >= 30 ? rgb(255, 215, 0) : score >= 20 ? rgb(192, 192, 192) : rgb(205, 127, 50);
      cv.fillCircle(34, 66, 7, mc);
      cv.fillCircle(34, 66, 4, mix565(mc, C_BLACK, 80));
    }
    txtC(80, 58, "SCORE " + String(score), C_WHITE, 1);
    if (newHigh) txtC(80, 69, "NEW HIGH SCORE!", C_YELLOW, 1);
    else txtC(80, 69, "BEST " + String(hs[curGame]), C_GRAY, 1);
    txtC(80, 82, "A:RETRY   B:MENU", C_CYAN, 1);
    if (pressedEv[K_A]) startGame(curGame);
    else if (pressedEv[K_B]) mode = M_MENU;
    return;
  }
  if (pressedEv[K_START]) { paused = !paused; pauseSel = 0; }
  if (paused) {
    if (repeatEv[K_UP] && pauseSel > 0) pauseSel--;
    if (repeatEv[K_DOWN] && pauseSel < 2) pauseSel++;
    if (pressedEv[K_B]) paused = false;
    if (pressedEv[K_A]) {
      if (pauseSel == 0) paused = false;
      else if (pauseSel == 1) { startGame(curGame); return; }
      else { mode = M_MENU; paused = false; return; }
    }
    gameDraw(curGame);
    cv.fillRect(30, 30, 100, 66, C_BLACK);
    cv.drawRect(30, 30, 100, 66, C_WHITE);
    txtC(80, 36, "PAUSED", C_YELLOW, 1);
    const char* opt[3] = {"RESUME", "RESTART", "QUIT"};
    for (int i = 0; i < 3; i++) {
      uint16_t c = (i == pauseSel) ? C_GREEN : C_GRAY;
      if (i == pauseSel) txt(48, 52 + i * 12, ">", C_GREEN, 1);
      txt(58, 52 + i * 12, opt[i], c, 1);
    }
    return;
  }
  gameUpdate(curGame);
  gameDraw(curGame);
}

// ---------------- MENU ----------------
void drawIcon(int i, int cx, int cy, uint16_t col) {
  switch (i) {
    case 0: {
      const int8_t ox[6] = {-18, -11, -4, -4, -4, 3};
      const int8_t oy[6] = {8, 8, 8, 1, -6, -6};
      for (int k = 0; k < 6; k++) cv.fillRect(cx + ox[k], cy + oy[k], 7, 7, col);
      cv.fillRect(cx + 14, cy - 6, 6, 6, C_RED);
    } break;
    case 1:
      cv.fillRect(cx - 12, cy - 6, 8, 8, col);
      cv.fillRect(cx - 4, cy - 6, 8, 8, col);
      cv.fillRect(cx + 4, cy - 6, 8, 8, col);
      cv.fillRect(cx - 4, cy + 2, 8, 8, col);
      break;
    case 2:
      cv.fillRect(cx - 18, cy - 14, 11, 5, col);
      cv.fillRect(cx - 5, cy - 14, 11, 5, col);
      cv.fillRect(cx + 8, cy - 14, 11, 5, col);
      cv.fillRect(cx - 12, cy + 12, 24, 3, C_WHITE);
      cv.fillCircle(cx + 4, cy + 3, 2, C_WHITE);
      break;
    case 3:
      cv.fillRect(cx + 8, cy - 18, 10, 16, C_GREEN);
      cv.fillRect(cx + 8, cy + 6, 10, 14, C_GREEN);
      cv.fillCircle(cx - 8, cy, 7, col);
      cv.fillCircle(cx - 5, cy - 2, 2, C_WHITE);
      cv.fillRect(cx - 2, cy, 5, 2, C_ORANGE);
      break;
    case 4:
      cv.fillRect(cx - 6, cy - 4, 12, 12, col);
      cv.fillRect(cx - 2, cy - 14, 12, 10, col);
      cv.fillRect(cx + 5, cy - 12, 2, 2, C_BLACK);
      cv.fillRect(cx - 12, cy + 2, 6, 4, col);
      cv.fillRect(cx - 4, cy + 8, 3, 6, col);
      cv.fillRect(cx + 2, cy + 8, 3, 6, col);
      break;
    case 5:
      cv.fillRect(cx - 22, cy - 8, 3, 16, col);
      cv.fillRect(cx + 19, cy - 4, 3, 16, col);
      cv.fillRect(cx - 2, cy - 2, 4, 4, C_WHITE);
      break;
    case 6:
      drawSprite8(cx - 20, cy - 12, SPR_INV[1][0], 6, col);
      drawSprite8(cx + 4, cy - 12, SPR_INV[2][1], 6, col);
      drawSprite8(cx - 8, cy - 2, SPR_INV[0][0], 6, col);
      cv.fillRect(cx - 5, cy + 12, 11, 3, C_GREEN);
      cv.fillRect(cx - 1, cy + 9, 3, 3, C_GREEN);
      break;
    case 7:
      cv.fillRoundRect(cx - 18, cy - 14, 17, 17, 3, 0xEEF9);
      cv.fillRoundRect(cx + 1, cy - 14, 17, 17, 3, 0xF58F);
      cv.fillRoundRect(cx - 18, cy + 5, 17, 17, 3, 0xF3EB);
      cv.fillRoundRect(cx + 1, cy + 5, 17, 17, 3, 0xEDC6);
      break;
    case 8:
      cv.fillRoundRect(cx - 7, cy - 13, 14, 26, 4, col);
      cv.fillRect(cx - 5, cy - 6, 10, 6, 0x9F7D);
      cv.fillRect(cx - 5, cy + 5, 10, 4, 0x2B6D);
      cv.fillRect(cx - 9, cy - 9, 3, 7, C_BLACK);
      cv.fillRect(cx + 7, cy - 9, 3, 7, C_BLACK);
      cv.fillRect(cx - 9, cy + 5, 3, 7, C_BLACK);
      cv.fillRect(cx + 7, cy + 5, 3, 7, C_BLACK);
      break;
    case 9:
      cv.fillCircle(cx - 4, cy + 10, 5, col);
      cv.fillRect(cx, cy - 14, 2, 24, col);
      cv.fillRect(cx, cy - 14, 10, 4, col);
      break;
    case 10:
      cv.fillCircle(cx, cy, 11, col);
      cv.fillCircle(cx, cy, 5, C_BLACK);
      cv.fillRect(cx - 2, cy - 16, 4, 5, col);
      cv.fillRect(cx - 2, cy + 11, 4, 5, col);
      cv.fillRect(cx - 16, cy - 2, 5, 4, col);
      cv.fillRect(cx + 11, cy - 2, 5, 4, col);
      break;
  }
}

void menuTick() {
  if (repeatEv[K_LEFT]) { menuSel = (menuSel + MENU_N - 1) % MENU_N; beep(900, 10); }
  if (repeatEv[K_RIGHT]) { menuSel = (menuSel + 1) % MENU_N; beep(900, 10); }
  if (pressedEv[K_A]) {
    beep(1400, 40);
    if (menuSel < NGAMES) startGame(menuSel);
    else if (menuSel == MENU_MUSIC) { prevMode = M_MENU; mode = M_MP3; }
    else { prevMode = M_MENU; mode = M_SETTINGS; }
    return;
  }
  cv.fillScreen(rgb(8, 10, 24));
  txtC(80, 3, "RETRO CONSOLE", C_GRAY, 1);
  cv.drawFastHLine(10, 13, 140, C_DGRAY);
  uint16_t col = ITEM_COL[menuSel];
  cv.drawRoundRect(20, 18, 120, 70, 6, col);
  drawIcon(menuSel, 80, 42, col);
  txtC(80, 68, ITEM_NAME[menuSel], C_WHITE, 2);
  cv.fillTriangle(8, 53, 14, 47, 14, 59, C_GRAY);
  cv.fillTriangle(152, 53, 146, 47, 146, 59, C_GRAY);
  if (menuSel < NGAMES) txtC(80, 93, "HI SCORE " + String(hs[menuSel]), C_YELLOW, 1);
  else if (menuSel == MENU_MUSIC) txtC(80, 93, "MP3 + COVER ART", C_GRAY, 1);
  else txtC(80, 93, "SOUND  JOYSTICK  SD", C_GRAY, 1);
  for (int i = 0; i < MENU_N; i++)
    cv.fillCircle(80 + i * 10 - (MENU_N - 1) * 5, 105, i == menuSel ? 3 : 2, i == menuSel ? col : C_DGRAY);
  if (wantPlay && nTracks > 0) {
    String t = cleanName(tracks[curTrack]);
    if (t.length() > 22) t = t.substring(0, 21) + "~";
    cv.fillRect(0, 115, SCRW, 13, rgb(20, 24, 50));
    txt(3, 118, String("\x0E ") + t, C_CYAN, 1);
  }
}

// ---------------- SETTINGS ----------------
int calStep = 0;          // 0 = off, 1 = centre, 2..5 = UP/DOWN/LEFT/RIGHT, 6 = result
int calVx[4], calVy[4];
String calMsg = "";
uint16_t calMsgCol = C_WHITE;
uint32_t rebootAt = 0;

void joySample(int& ax, int& ay) {
  long sx = 0, sy = 0;
  for (int i = 0; i < 12; i++) { sx += analogRead(JOY_X); sy += analogRead(JOY_Y); delay(4); }
  ax = sx / 12;
  ay = sy / 12;
}
void calEvaluate() {
  float mg[4];
  for (int d = 0; d < 4; d++) mg[d] = sqrtf((float)calVx[d] * calVx[d] + (float)calVy[d] * calVy[d]);
  const char* nm[4] = {"UP", "DOWN", "LEFT", "RIGHT"};
  for (int d = 0; d < 4; d++)
    if (mg[d] < 500) {
      calMsg = String(nm[d]) + " MOVED TOO LITTLE";
      calMsgCol = C_RED;
      return;
    }
  for (int i = 0; i < 4; i++)
    for (int j = i + 1; j < 4; j++) {
      float cs = ((float)calVx[i] * calVx[j] + (float)calVy[i] * calVy[j]) / (mg[i] * mg[j]);
      if (cs > 0.8f) {
        calMsg = String(nm[i]) + " & " + String(nm[j]) + " ARE THE SAME";
        calMsgCol = C_RED;
        return;
      }
    }
  for (int d = 0; d < 4; d++) { jvx[d] = calVx[d]; jvy[d] = calVy[d]; jmag[d] = mg[d]; }
  joyCal = true;
  saveJoyCal();
  calMsg = "SAVED! JOYSTICK READY";
  calMsgCol = C_GREEN;
}
void calTick() {
  if (pressedEv[K_B]) { calStep = 0; return; }
  const char* nm[4] = {"UP", "DOWN", "LEFT", "RIGHT"};
  if (pressedEv[K_A]) {
    if (calStep == 1) {
      joySample(joyCx, joyCy);
      calStep = 2;
    } else if (calStep >= 2 && calStep <= 5) {
      int ax, ay;
      joySample(ax, ay);
      calVx[calStep - 2] = ax - joyCx;
      calVy[calStep - 2] = ay - joyCy;
      calStep++;
      if (calStep == 6) calEvaluate();
    } else if (calStep == 6) {
      calStep = 0;
      return;
    }
  }
  cv.fillScreen(rgb(8, 10, 24));
  txtC(80, 3, "JOYSTICK SETUP", C_WHITE, 1);
  cv.drawFastHLine(10, 13, 140, C_DGRAY);
  if (calStep == 1) {
    txtC(80, 30, "LET GO OF THE STICK", C_LGRAY, 1);
    txtC(80, 44, "(centre position)", C_GRAY, 1);
    txtC(80, 70, "PRESS A", C_GREEN, 2);
  } else if (calStep >= 2 && calStep <= 5) {
    txtC(80, 24, "PUSH THE STICK", C_LGRAY, 1);
    txtC(80, 38, nm[calStep - 2], C_YELLOW, 2);
    txtC(80, 60, "ALL THE WAY AND HOLD", C_LGRAY, 1);
    txtC(80, 76, "THEN PRESS A", C_GREEN, 1);
    txtC(80, 92, "(" + String(calStep - 1) + "/4)", C_GRAY, 1);
  } else {
    txtC(80, 30, calMsg, calMsgCol, 1);
    if (calMsgCol == C_RED) {
      txtC(80, 54, "Check: GND, 3V3 and", C_GRAY, 1);
      txtC(80, 64, "the VRX / VRY wires", C_GRAY, 1);
    }
    for (int d = 0; d < 4; d++)
      txt(20, 76 + d * 9, String(nm[d]) + ": " + String(calVx[d]) + ", " + String(calVy[d]), C_LGRAY, 1);
    txtC(80, 118, "A: CLOSE", C_GRAY, 1);
  }
  if (calStep >= 1 && calStep <= 5) txt(4, 118, "X:" + String(joyRawX) + " Y:" + String(joyRawY), C_DGRAY, 1);
  if (calStep >= 1 && calStep <= 5) txtR(156, 118, "B:EXIT", C_DGRAY, 1);
}

void settingsTick() {
  if (calStep > 0) { calTick(); return; }
  if (repeatEv[K_UP] && setSel > 0) setSel--;
  if (repeatEv[K_DOWN] && setSel < 3) setSel++;
  if (setSel == 0 && (pressedEv[K_LEFT] || pressedEv[K_RIGHT] || pressedEv[K_A])) {
    sfxOn = !sfxOn;
    beep(1000, 40);
  }
  if (setSel == 1) {
    if (repeatEv[K_LEFT] && vol > 0) { vol--; applyVol(); }
    if (repeatEv[K_RIGHT] && vol < 10) { vol++; applyVol(); }
  }
  if (setSel == 2 && pressedEv[K_A]) { calStep = 1; return; }
  if (setSel == 3 && pressedEv[K_A] && !rebootAt) {
    prefs.putInt("safe", 0);
    prefs.putInt("bootTry", 0);
    rebootAt = millis() + 900;
  }
  if (pressedEv[K_B]) { saveSettings(); mode = M_MENU; return; }
  cv.fillScreen(rgb(8, 10, 24));
  txtC(80, 3, "SETTINGS", C_WHITE, 1);
  cv.drawFastHLine(10, 13, 140, C_DGRAY);
  txt(4, 18, setSel == 0 ? ">" : " ", C_GREEN, 1);
  txt(14, 18, "SOUND FX", C_LGRAY, 1);
  txtR(154, 18, sfxOn ? "ON" : "OFF", sfxOn ? C_GREEN : C_RED, 1);
  txt(4, 30, setSel == 1 ? ">" : " ", C_GREEN, 1);
  txt(14, 30, "VOLUME", C_LGRAY, 1);
  for (int i = 0; i < 10; i++) cv.fillRect(92 + i * 6, 30, 5, 7, i < vol ? C_GREEN : C_DGRAY);
  txt(4, 42, setSel == 2 ? ">" : " ", C_GREEN, 1);
  txt(14, 42, "CALIBRATE JOYSTICK", C_LGRAY, 1);
  txtR(154, 42, joyCal ? "OK" : "", C_GREEN, 1);
  txt(4, 54, setSel == 3 ? ">" : " ", C_GREEN, 1);
  if (rebootAt) txt(14, 54, "REBOOTING...", C_YELLOW, 1);
  else {
    txt(14, 54, "RESET SAFE MODE", C_LGRAY, 1);
    txtR(154, 54, safeLevel ? ("LV" + String(safeLevel)) : String("OFF"), safeLevel ? C_RED : C_GRAY, 1);
  }
  cv.drawFastHLine(10, 64, 140, C_DGRAY);
  String sd = sdOK ? ("SD OK  " + String(nTracks) + " TRACKS") : (safeLevel >= 2 ? String("SD OFF (SAFE MODE)") : String("SD NOT FOUND"));
  txt(4, 69, sd, sdOK ? C_CYAN : C_RED, 1);
  txt(4, 81, "JOY X:" + String(joyRawX) + " Y:" + String(joyRawY), C_LGRAY, 1);
  String d = "";
  if (held[K_UP]) d += "UP ";
  if (held[K_DOWN]) d += "DOWN ";
  if (held[K_LEFT]) d += "LEFT ";
  if (held[K_RIGHT]) d += "RIGHT";
  txt(4, 93, d, C_YELLOW, 1);
  txt(4, 105, "LAST RESET: " + lastRst, rstAbnormal() ? C_RED : C_GRAY, 1);
  txtC(80, 119, "B: SAVE & BACK", C_DGRAY, 1);
  if (rebootAt && millis() >= rebootAt) ESP.restart();
}

// ---------------- MP3 PLAYER UI ----------------
float eqH[10], eqT[10];

String fmtTime(uint32_t ms) {
  uint32_t s = ms / 1000;
  char b[10];
  snprintf(b, sizeof(b), "%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
  return String(b);
}
uint32_t hashStr(const String& s) {
  uint32_t h = 2166136261u;
  for (unsigned i = 0; i < s.length(); i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
  return h;
}
void drawCover(int x, int y) {
  cv.drawRoundRect(x - 1, y - 1, ART_D + 2, ART_D + 2, 3, C_DGRAY);
  if (artValid && artBuf[artCur]) {
    cv.drawRGBBitmap(x, y, artBuf[artCur], ART_D, ART_D);
    return;
  }
  // no embedded picture: colourful placeholder (colour comes from the song name)
  uint32_t h = hashStr(tracks[curTrack]);
  uint16_t c1 = rgb(60 + (h & 0x7F), 60 + ((h >> 7) & 0x7F), 60 + ((h >> 14) & 0x7F));
  uint16_t c2 = mix565(c1, C_BLACK, 170);
  for (int i = 0; i < ART_D; i += 2) cv.fillRect(x, y + i, ART_D, 2, mix565(c1, c2, i * 255 / ART_D));
  cv.fillCircle(x + 26, y + 44, 7, C_WHITE);
  cv.fillRect(x + 31, y + 16, 3, 29, C_WHITE);
  cv.fillRect(x + 31, y + 16, 16, 6, C_WHITE);
}

void mp3Tick() {
  if (nTracks > 0) {
    if (pressedEv[K_A]) wantPlay = !wantPlay;
    if (repeatEv[K_UP] && vol < 10) { vol++; applyVol(); }
    if (repeatEv[K_DOWN] && vol > 0) { vol--; applyVol(); }
    if (pressedEv[K_RIGHT]) {
      reqTrack = (curTrack + 1) % nTracks;
      wantPlay = true;
    }
    if (pressedEv[K_LEFT]) {
      if (elapsedMs > 3000) reqTrack = curTrack;
      else reqTrack = (curTrack - 1 + nTracks) % nTracks;
      wantPlay = true;
    }
  } else if (pressedEv[K_A]) {
    retrySD();
  }
  cv.fillScreen(rgb(10, 8, 20));
  txt(4, 2, "\x0E MP3 PLAYER", C_CYAN, 1);
  if (nTracks > 0) txtR(156, 2, String(curTrack + 1) + "/" + String(nTracks), C_GRAY, 1);
  cv.drawFastHLine(0, 11, SCRW, C_DGRAY);

  if (nTracks == 0 && safeLevel >= 2) {
    txtC(80, 26, "SAFE MODE: MUSIC OFF", C_YELLOW, 1);
    txtC(80, 42, "The console crashed a", C_GRAY, 1);
    txtC(80, 52, "few times at startup", C_GRAY, 1);
    txtC(80, 62, "so SD + music are off.", C_GRAY, 1);
    txtC(80, 80, "Settings > RESET SAFE", C_LGRAY, 1);
    txtC(80, 90, "MODE to try again", C_LGRAY, 1);
    txtC(80, 112, "LAST RESET: " + lastRst, C_RED, 1);
    return;
  }
  if (nTracks == 0) {
    txtC(80, 28, sdOK ? "NO MP3 FILES" : "SD CARD ERROR", C_RED, 1);
    txtC(80, 44, "Use FAT32 card (max 32GB)", C_GRAY, 1);
    txtC(80, 56, "put .mp3 files in the", C_GRAY, 1);
    txtC(80, 66, "main folder (root)", C_GRAY, 1);
    txtC(80, 86, "A: RETRY", C_GREEN, 1);
    txtC(80, 112, "HOLD SELECT: BACK", C_DGRAY, 1);
    return;
  }

  drawCover(6, 14);

  // right column: big elapsed time, estimated total, equalizer
  txt(80, 14, fmtTime(elapsedMs), C_WHITE, 2);
  float frac = (float)aPos / (float)aSize;
  if (frac > 1) frac = 1;
  String tot = (frac > 0.03f && elapsedMs > 3000) ? fmtTime((uint32_t)(elapsedMs / frac)) : String("--:--");
  txt(80, 33, "/ " + tot, C_GRAY, 1);
  for (int i = 0; i < 10; i++) {
    if (wantPlay) {
      if (random(100) < 35) eqT[i] = random(5, 30);
    } else {
      eqT[i] = 3;
    }
    eqH[i] += (eqT[i] - eqH[i]) * 0.4f;
    int h = (int)eqH[i];
    if (h < 2) h = 2;
    cv.fillRect(80 + i * 7, 77 - h, 5, h, rgb(40 + i * 18, 210 - i * 10, 255 - i * 12));
  }

  // song title (no .mp3), scrolls if too long
  String ttl = cleanName(tracks[curTrack]);
  mq.fillScreen(C_BLACK);
  mq.setTextSize(2);
  mq.setTextColor(C_WHITE);
  int tw = (int)ttl.length() * 12;
  if (tw <= 150) {
    mq.setCursor((150 - tw) / 2, 1);
    mq.print(ttl);
  } else {
    int span = tw + 36;
    mq.setCursor(-mScroll, 1);
    mq.print(ttl);
    mq.setCursor(-mScroll + span, 1);
    mq.print(ttl);
    mScroll = (mScroll + 2) % span;
  }
  cv.drawRGBBitmap(5, 82, mq.getBuffer(), 150, 16);

  // progress bar
  cv.fillRoundRect(5, 101, 150, 5, 2, C_DGRAY);
  int pw = (int)(frac * 150);
  if (pw > 0) cv.fillRoundRect(5, 101, pw < 4 ? 4 : pw, 5, 2, C_GREEN);

  // transport icons (left) + volume bars (right)
  int cy = 117;
  cv.fillRect(6, cy - 5, 2, 10, C_LGRAY);
  cv.fillTriangle(20, cy - 5, 20, cy + 5, 9, cy, C_LGRAY);
  cv.drawCircle(34, cy, 9, C_GREEN);
  if (wantPlay) {
    cv.fillRect(30, cy - 4, 3, 8, C_GREEN);
    cv.fillRect(36, cy - 4, 3, 8, C_GREEN);
  } else {
    cv.fillTriangle(32, cy - 5, 32, cy + 5, 40, cy, C_GREEN);
  }
  cv.fillRect(60, cy - 5, 2, 10, C_LGRAY);
  cv.fillTriangle(48, cy - 5, 48, cy + 5, 59, cy, C_LGRAY);
  for (int i = 0; i < 10; i++) {
    uint16_t c = i < vol ? (i < 6 ? C_GREEN : (i < 9 ? C_YELLOW : C_RED)) : C_DGRAY;
    cv.fillRect(80 + i * 7, 126 - (3 + i), 5, 3 + i, c);
  }
}

// ---------------- SETUP / LOOP ----------------
void toggleMp3() {
  if (mode == M_MP3) {
    saveSettings();
    pauseMusic();
    mode = prevMode;
  } else {
    prevMode = mode;
    mode = M_MP3;
  }
}

void splash() {
  bool warn = rstAbnormal();
  for (int i = 0; i <= 20; i++) {
    cv.fillScreen(C_BLACK);
    cv.drawFastHLine(80 - i * 4, 52, i * 8, C_CYAN);
    cv.drawFastHLine(80 - i * 4, 76, i * 8, C_MAGENTA);
    if (i > 8) txtC(80, 58, "RETRO CONSOLE", C_WHITE, 1);
    if (i > 12 && warn) {
      txtC(80, 90, "LAST RESET:", C_RED, 1);
      txtC(80, 100, lastRst, C_RED, 1);
    }
    if (i > 12 && safeLevel > 0) txtC(80, 112, "SAFE MODE " + String(safeLevel), C_YELLOW, 1);
    tft.drawRGBBitmap(0, 0, cv.getBuffer(), SCRW, SCRH);
    delay(30);
  }
  if (sfxOn) {
    buzzTone(523); delay(90);
    buzzTone(659); delay(90);
    buzzTone(784); delay(120);
    buzzTone(0);
  }
  delay((warn || safeLevel > 0) ? 1800 : 300);
}

void setup() {
#if DISABLE_BROWNOUT
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
#endif
  Serial.begin(115200);
  lastRst = rstName();
  Serial.println("");
  Serial.println("== RETRO CONSOLE v5 ==");
  Serial.printf("reset reason: %s, heap %lu\n", lastRst.c_str(), (unsigned long)ESP.getFreeHeap());
  pinMode(PIN_A, INPUT_PULLUP);
  pinMode(PIN_B, INPUT_PULLUP);
  pinMode(PIN_START, INPUT_PULLUP);
  pinMode(PIN_SELECT, INPUT_PULLUP);

  loadSettings();
  loadJoyCal();

  // crash guard: if the last boots never reached "stable", switch features off step by step
  safeLevel = prefs.getInt("safe", 0);
  int bt = prefs.getInt("bootTry", 0);
  if (bt >= 2) {
    if (safeLevel < 2) safeLevel++;
    prefs.putInt("safe", safeLevel);
    bt = 0;
  }
  prefs.putInt("bootTry", bt + 1);
  artEnabled = (safeLevel < 1);
  Serial.printf("safe level %d, joystick calibrated %d\n", safeLevel, joyCal);

  tft.initR(INITR_BLACKTAB);  // if colours/offset look off try INITR_GREENTAB or INITR_REDTAB
  tft.setRotation(1);
  tft.fillScreen(C_BLACK);
  cv.setTextWrap(false);
  mq.setTextWrap(false);
  buzzInit();

  // joystick centre calibration (leave stick untouched at power-on)
  long sx = 0, sy = 0;
  for (int i = 0; i < 16; i++) { sx += analogRead(JOY_X); sy += analogRead(JOY_Y); delay(2); }
  joyCx = sx / 16;
  joyCy = sy / 16;
  Serial.printf("joystick centre %d, %d\n", joyCx, joyCy);

  // cover-art buffers (2 x 64x64 RGB565)
  artBuf[0] = (uint16_t*)malloc(ART_D * ART_D * 2);
  artBuf[1] = (uint16_t*)malloc(ART_D * ART_D * 2);

  // SD card module on its own SPI bus (the TFT uses the default one)
  if (safeLevel < 2) {
    sdSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
    Serial.println("mounting SD...");
    sdOK = mountSD();
    if (sdOK) {
      delay(200);  // let the card's power settle
      scanTracks();
      Serial.printf("found %d mp3 files\n", nTracks);
    }
  } else {
    Serial.println("SAFE MODE 2: SD and music disabled");
  }

  aOut = new AudioOutputI2S();
  aOut->SetPinout(I2S_BCLK, I2S_LRC, I2S_DIN);
#if AUDIO_MONO
  aOut->SetOutputModeMono(true);
#endif
  applyVol();

  splash();  // show the screen first, then start the music engine
  if (safeLevel < 2) startAudioIfReady();
}

void loop() {
  uint32_t t0 = millis();
  readInputs();
  musicBookkeeping();

  // 5 seconds without a crash = this boot was healthy
  static bool stableMarked = false;
  if (!stableMarked && millis() > 5000) {
    stableMarked = true;
    prefs.putInt("bootTry", 0);
    Serial.println("boot marked stable");
  }

  // hold SELECT to jump between games and MP3 player
  static bool selHandled = false;
  if (held[K_SELECT]) {
    if (!selHandled && millis() - holdStart[K_SELECT] >= SELECT_HOLD_MS) {
      selHandled = true;
      toggleMp3();
      beep(1500, 40);
    }
  } else {
    selHandled = false;
  }

  switch (mode) {
    case M_MENU: menuTick(); break;
    case M_GAME: gameTick(); break;
    case M_MP3: mp3Tick(); break;
    case M_SETTINGS: settingsTick(); break;
  }

  tft.drawRGBBitmap(0, 0, cv.getBuffer(), SCRW, SCRH);

  if (buzzOff && millis() > buzzOff) {
    buzzTone(0);
    buzzOff = 0;
  }
  uint32_t dt = millis() - t0;
  if (dt < 33) delay(33 - dt);
}
