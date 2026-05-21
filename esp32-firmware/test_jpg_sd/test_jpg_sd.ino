/*
 * BikeGPS — Phase 1: JPG from SD card on display
 *
 * Board: Waveshare ESP32-C6-Touch-LCD-1.47 (TOUCH version)
 * FQBN:  esp32:esp32:esp32c6:CDCOnBoot=cdc
 *
 * Place any JPEG named "test.jpg" in the root of the SD card.
 *
 * Pins (touch version — different from non-touch!):
 *   LCD: SCK=1, MOSI=2, RST=22, CS=14, DC=15, BL=23
 *   SD:  SCK=1(shared), MOSI=2(shared), MISO=3, CS=4
 */

#include <Arduino_GFX_Library.h>
#include <SD.h>
#include <SPI.h>
#include <JPEGDEC.h>

// ── Pin definitions ──────────────────────────────────────────
#define LCD_DC    15
#define LCD_CS    14
#define LCD_SCK    1
#define LCD_MOSI   2
#define LCD_RST   22
#define LCD_BL    23
#define SD_MISO    3
#define SD_CS      4

// ── Display ──────────────────────────────────────────────────
Arduino_DataBus *bus = new Arduino_HWSPI(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI);
Arduino_GFX    *gfx = new Arduino_ST7789(
  bus, LCD_RST,
  0 /* rotation */, false /* IPS */,
  172 /* width */, 320 /* height */,
  34 /* col_offset1 */, 0 /* row_offset1 */,
  34 /* col_offset2 */, 0 /* row_offset2 */
);

// ── JD9853 init sequence (required for this display) ─────────
void lcd_reg_init() {
  static const uint8_t init_operations[] = {
    BEGIN_WRITE,
    WRITE_COMMAND_8, 0x11,
    END_WRITE,
    DELAY, 120,

    BEGIN_WRITE,
    WRITE_C8_D16, 0xDF, 0x98, 0x53,
    WRITE_C8_D8,  0xB2, 0x23,

    WRITE_COMMAND_8, 0xB7,
    WRITE_BYTES, 4,
    0x00, 0x47, 0x00, 0x6F,

    WRITE_COMMAND_8, 0xBB,
    WRITE_BYTES, 6,
    0x1C, 0x1A, 0x55, 0x73, 0x63, 0xF0,

    WRITE_C8_D16, 0xC0, 0x44, 0xA4,
    WRITE_C8_D8,  0xC1, 0x16,

    WRITE_COMMAND_8, 0xC3,
    WRITE_BYTES, 8,
    0x7D, 0x07, 0x14, 0x06, 0xCF, 0x71, 0x72, 0x77,

    WRITE_COMMAND_8, 0xC4,
    WRITE_BYTES, 12,
    0x00, 0x00, 0xA0, 0x79, 0x0B, 0x0A, 0x16, 0x79, 0x0B, 0x0A, 0x16, 0x82,

    WRITE_COMMAND_8, 0xC8,
    WRITE_BYTES, 32,
    0x3F,0x32,0x29,0x29,0x27,0x2B,0x27,0x28,
    0x28,0x26,0x25,0x17,0x12,0x0D,0x04,0x00,
    0x3F,0x32,0x29,0x29,0x27,0x2B,0x27,0x28,
    0x28,0x26,0x25,0x17,0x12,0x0D,0x04,0x00,

    WRITE_COMMAND_8, 0xD0,
    WRITE_BYTES, 5,
    0x04, 0x06, 0x6B, 0x0F, 0x00,

    WRITE_C8_D16, 0xD7, 0x00, 0x30,
    WRITE_C8_D8,  0xE6, 0x14,
    WRITE_C8_D8,  0xDE, 0x01,

    WRITE_COMMAND_8, 0xB7,
    WRITE_BYTES, 5,
    0x03, 0x13, 0xEF, 0x35, 0x35,

    WRITE_COMMAND_8, 0xC1,
    WRITE_BYTES, 3,
    0x14, 0x15, 0xC0,

    WRITE_C8_D16, 0xC2, 0x06, 0x3A,
    WRITE_C8_D16, 0xC4, 0x72, 0x12,
    WRITE_C8_D8,  0xBE, 0x00,
    WRITE_C8_D8,  0xDE, 0x02,

    WRITE_COMMAND_8, 0xE5,
    WRITE_BYTES, 3,
    0x00, 0x02, 0x00,

    WRITE_COMMAND_8, 0xE5,
    WRITE_BYTES, 3,
    0x01, 0x02, 0x00,

    WRITE_C8_D8, 0xDE, 0x00,
    WRITE_C8_D8, 0x35, 0x00,
    WRITE_C8_D8, 0x3A, 0x05,

    WRITE_COMMAND_8, 0x2A,
    WRITE_BYTES, 4,
    0x00, 0x22, 0x00, 0xCD,

    WRITE_COMMAND_8, 0x2B,
    WRITE_BYTES, 4,
    0x00, 0x00, 0x01, 0x3F,

    WRITE_C8_D8, 0xDE, 0x02,

    WRITE_COMMAND_8, 0xE5,
    WRITE_BYTES, 3,
    0x00, 0x02, 0x00,

    WRITE_C8_D8, 0xDE, 0x00,
    WRITE_C8_D8, 0x36, 0x00,
    WRITE_COMMAND_8, 0x21,
    END_WRITE,

    DELAY, 10,

    BEGIN_WRITE,
    WRITE_COMMAND_8, 0x29,
    END_WRITE
  };
  bus->batchOperation(init_operations, sizeof(init_operations));
}

// ── JPEG decoder ─────────────────────────────────────────────
JPEGDEC jpeg;

// File handle used inside JPEG callbacks
static File _jpegFile;

void *jpegOpenCB(const char *filename, int32_t *pSize) {
  _jpegFile = SD.open(filename);
  if (!_jpegFile) return nullptr;
  *pSize = _jpegFile.size();
  return &_jpegFile;
}
void jpegCloseCB(void *pHandle) {
  ((File *)pHandle)->close();
}
int32_t jpegReadCB(JPEGFILE *pFile, uint8_t *pBuf, int32_t iLen) {
  return _jpegFile.read(pBuf, iLen);
}
int32_t jpegSeekCB(JPEGFILE *pFile, int32_t iPosition) {
  return _jpegFile.seek(iPosition) ? iPosition : -1;
}

// Draw callback: receives decoded RGB565 rows, writes to display
int jpegDrawCB(JPEGDRAW *pDraw) {
  gfx->draw16bitRGBBitmap(pDraw->x, pDraw->y,
                           pDraw->pPixels,
                           pDraw->iWidth, pDraw->iHeight);
  return 1;  // continue decoding
}

// ── Helpers ──────────────────────────────────────────────────
void showError(const char *msg, uint16_t bgColor) {
  gfx->fillScreen(bgColor);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(4, 140);
  gfx->println(msg);
  Serial.println(msg);
}

// ── Setup ────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  delay(100);
  Serial.println("\n=== BikeGPS Phase1: JPG from SD ===");

  // 1. Init SPI bus WITH MISO (needed for SD) — do this FIRST
  //    The display will reuse this already-started bus.
  SPI.begin(LCD_SCK, SD_MISO, LCD_MOSI, SD_CS);

  // 2. Init display
  if (!gfx->begin()) {
    Serial.println("Display init failed!");
  }
  lcd_reg_init();

  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);

  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(10, 140);
  gfx->print("Mounting SD...");
  Serial.println("Display OK");

  // 3. Init SD card (shares SPI bus, separate CS=4)
  if (!SD.begin(SD_CS)) {
    showError("SD mount FAILED", RGB565_RED);
    return;
  }
  Serial.printf("SD OK — %llu MB\n", SD.cardSize() / (1024 * 1024));

  // List root to find exact filename
  Serial.println("--- SD root ---");
  String foundJpeg = "";
  File root = SD.open("/");
  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    String name = String(entry.name());
    Serial.printf("  [%s] %d bytes\n", name.c_str(), (int)entry.size());
    // Accept test.jpg or test.JPG (case-insensitive check)
    String lower = name;
    lower.toLowerCase();
    if (lower == "test.jpg" || lower == "/test.jpg") {
      foundJpeg = "/" + name;
      // strip double slash
      if (foundJpeg.startsWith("//")) foundJpeg = foundJpeg.substring(1);
    }
    entry.close();
  }
  root.close();

  if (foundJpeg == "") {
    showError("No test.jpg!", RGB565_YELLOW);
    return;
  }
  Serial.printf("Using: %s\n", foundJpeg.c_str());

  gfx->fillScreen(RGB565_BLACK);
  gfx->setCursor(10, 140);
  gfx->print("Loading JPEG...");

  // 4. Read JPEG into RAM then decode
  File jf = SD.open(foundJpeg);
  if (!jf) {
    showError("SD open err!", RGB565_YELLOW);
    return;
  }
  size_t jpegSize = jf.size();
  Serial.printf("Reading %d bytes...\n", (int)jpegSize);

  uint8_t *jpegBuf = (uint8_t *)malloc(jpegSize);
  if (!jpegBuf) {
    jf.close();
    showError("malloc fail!", RGB565_ORANGE);
    return;
  }
  jf.read(jpegBuf, jpegSize);
  jf.close();
  Serial.printf("First bytes: %02X %02X %02X\n",
    jpegBuf[0], jpegBuf[1], jpegBuf[2]);  // should be FF D8 FF

  gfx->fillScreen(RGB565_BLACK);

  jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
  int rc = 0;
  if (jpeg.openRAM(jpegBuf, jpegSize, jpegDrawCB)) {
    jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
    rc = jpeg.decode(0, 0, 0);
    Serial.printf("decode rc=%d err=%d w=%d h=%d\n",
      rc, jpeg.getLastError(),
      jpeg.getWidth(), jpeg.getHeight());
    jpeg.close();
  } else {
    Serial.printf("openRAM failed! err=%d\n", jpeg.getLastError());
  }
  free(jpegBuf);

  if (rc) {
    Serial.println("JPEG displayed OK!");
    gfx->setTextColor(RGB565_GREEN);
    gfx->setTextSize(1);
    gfx->setCursor(4, 312);
    gfx->print("OK - test.jpg");
  } else {
    showError("JPEG decode ERR", RGB565_ORANGE);
  }
}

void loop() {
  // Nothing — image stays on screen
  delay(1000);
}
