/*
 * BikeGPS — Phase 2: OSM map tiles from SD card with fake GPS
 *
 * Board: Waveshare ESP32-C6-Touch-LCD-1.47 (TOUCH version)
 * FQBN:  esp32:esp32:esp32c6:CDCOnBoot=cdc
 *
 * SD card structure: /tiles/{zoom}/{x}/{y}.jpg
 * Populate with: python3 tools/download_tiles.py --lat 41.9028 --lon 12.4964 --zoom 15 --radius 5
 *
 * Display layout:
 *   y=0..159   → map tile (172×160 crop from 256×256 OSM tile)
 *   y=160..319 → info bar (coordinates, zoom, tile coords)
 */

#include <Arduino_GFX_Library.h>
#include <SD.h>
#include <SPI.h>
#include <JPEGDEC.h>
#include <math.h>

// ── Pins (TOUCH version) ─────────────────────────────────────
#define LCD_DC   15
#define LCD_CS   14
#define LCD_SCK   1
#define LCD_MOSI  2
#define LCD_RST  22
#define LCD_BL   23
#define SD_MISO   3
#define SD_CS     4

// ── Display ──────────────────────────────────────────────────
Arduino_DataBus *bus = new Arduino_HWSPI(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI);
Arduino_GFX    *gfx = new Arduino_ST7789(
  bus, LCD_RST, 0, false, 172, 320,
  34, 0, 34, 0);

// ── JPEG decoder ─────────────────────────────────────────────
JPEGDEC jpeg;

// ── OSM tile coordinate (must be global before any function) ──
struct TileCoord {
  int x, y;
  float pixX, pixY;  // GPS position within tile (0..256)
};

// ── JD9853 init sequence ─────────────────────────────────────
void lcd_reg_init() {
  static const uint8_t init_operations[] = {
    BEGIN_WRITE,
    WRITE_COMMAND_8, 0x11,
    END_WRITE,
    DELAY, 120,
    BEGIN_WRITE,
    WRITE_C8_D16, 0xDF, 0x98, 0x53,
    WRITE_C8_D8,  0xB2, 0x23,
    WRITE_COMMAND_8, 0xB7, WRITE_BYTES, 4, 0x00, 0x47, 0x00, 0x6F,
    WRITE_COMMAND_8, 0xBB, WRITE_BYTES, 6, 0x1C, 0x1A, 0x55, 0x73, 0x63, 0xF0,
    WRITE_C8_D16, 0xC0, 0x44, 0xA4,
    WRITE_C8_D8,  0xC1, 0x16,
    WRITE_COMMAND_8, 0xC3, WRITE_BYTES, 8,
      0x7D, 0x07, 0x14, 0x06, 0xCF, 0x71, 0x72, 0x77,
    WRITE_COMMAND_8, 0xC4, WRITE_BYTES, 12,
      0x00,0x00,0xA0,0x79,0x0B,0x0A,0x16,0x79,0x0B,0x0A,0x16,0x82,
    WRITE_COMMAND_8, 0xC8, WRITE_BYTES, 32,
      0x3F,0x32,0x29,0x29,0x27,0x2B,0x27,0x28,
      0x28,0x26,0x25,0x17,0x12,0x0D,0x04,0x00,
      0x3F,0x32,0x29,0x29,0x27,0x2B,0x27,0x28,
      0x28,0x26,0x25,0x17,0x12,0x0D,0x04,0x00,
    WRITE_COMMAND_8, 0xD0, WRITE_BYTES, 5, 0x04,0x06,0x6B,0x0F,0x00,
    WRITE_C8_D16, 0xD7, 0x00, 0x30,
    WRITE_C8_D8,  0xE6, 0x14,
    WRITE_C8_D8,  0xDE, 0x01,
    WRITE_COMMAND_8, 0xB7, WRITE_BYTES, 5, 0x03,0x13,0xEF,0x35,0x35,
    WRITE_COMMAND_8, 0xC1, WRITE_BYTES, 3, 0x14,0x15,0xC0,
    WRITE_C8_D16, 0xC2, 0x06, 0x3A,
    WRITE_C8_D16, 0xC4, 0x72, 0x12,
    WRITE_C8_D8,  0xBE, 0x00,
    WRITE_C8_D8,  0xDE, 0x02,
    WRITE_COMMAND_8, 0xE5, WRITE_BYTES, 3, 0x00,0x02,0x00,
    WRITE_COMMAND_8, 0xE5, WRITE_BYTES, 3, 0x01,0x02,0x00,
    WRITE_C8_D8, 0xDE, 0x00,
    WRITE_C8_D8, 0x35, 0x00,
    WRITE_C8_D8, 0x3A, 0x05,
    WRITE_COMMAND_8, 0x2A, WRITE_BYTES, 4, 0x00,0x22,0x00,0xCD,
    WRITE_COMMAND_8, 0x2B, WRITE_BYTES, 4, 0x00,0x00,0x01,0x3F,
    WRITE_C8_D8, 0xDE, 0x02,
    WRITE_COMMAND_8, 0xE5, WRITE_BYTES, 3, 0x00,0x02,0x00,
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

// ── OSM tile math ─────────────────────────────────────────────
TileCoord latLonToTile(double lat, double lon, int zoom) {  // NOLINT
  double n = pow(2.0, zoom);
  double lat_r = lat * M_PI / 180.0;
  TileCoord t;
  double fx = (lon + 180.0) / 360.0 * n;
  double fy = (1.0 - log(tan(lat_r) + 1.0 / cos(lat_r)) / M_PI) / 2.0 * n;
  t.x    = (int)fx;
  t.y    = (int)fy;
  t.pixX = (fx - t.x) * 256.0f;
  t.pixY = (fy - t.y) * 256.0f;
  return t;
}

// ── Draw clipping state ───────────────────────────────────────
// The 256×256 tile needs to be cropped to 172×160 centered on GPS pixel.
// We clip during the JPEGDEC draw callback.
static int _cropX, _cropY;   // top-left of crop within the 256×256 tile
static int _dstY;             // y offset on display (0 = top of screen)

int jpegDrawCB(JPEGDRAW *pDraw) {
  // pDraw->x, pDraw->y = position within the 256×256 source tile
  // We only draw pixels that fall within [_cropX.._cropX+172) x [_cropY.._cropY+160)
  int srcX = pDraw->x;
  int srcY = pDraw->y;
  int srcW = pDraw->iWidth;
  int srcH = pDraw->iHeight;

  // Intersection with crop window
  int x0 = max(srcX, _cropX);
  int x1 = min(srcX + srcW, _cropX + 172);
  int y0 = max(srcY, _cropY);
  int y1 = min(srcY + srcH, _cropY + 160);

  if (x1 <= x0 || y1 <= y0) return 1;  // nothing to draw

  // Draw each row of the intersection
  for (int row = y0; row < y1; row++) {
    int rowInMCU = row - srcY;
    uint16_t *rowPtr = pDraw->pPixels + rowInMCU * srcW + (x0 - srcX);
    gfx->draw16bitRGBBitmap(
      x0 - _cropX,           // dst X on display
      _dstY + (row - _cropY), // dst Y on display
      rowPtr,
      x1 - x0, 1
    );
  }
  return 1;
}

// ── Load and display a tile ────────────────────────────────────
bool showTile(int z, int tx, int ty, float gpsPxX, float gpsPxY) {
  // Compute crop: center GPS pixel in 172×160 window
  _cropX = (int)(gpsPxX - 86);
  _cropY = (int)(gpsPxY - 80);

  // Clamp to tile bounds (0..256-172, 0..256-160)
  _cropX = max(0, min(_cropX, 256 - 172));
  _cropY = max(0, min(_cropY, 256 - 160));
  _dstY  = 0;  // draw at top of screen

  char path[64];
  snprintf(path, sizeof(path), "/tiles/%d/%d/%d.jpg", z, tx, ty);
  Serial.printf("Loading %s (crop %d,%d)\n", path, _cropX, _cropY);

  File f = SD.open(path);
  if (!f) {
    Serial.println("Tile not found!");
    return false;
  }
  size_t sz = f.size();
  uint8_t *buf = (uint8_t *)malloc(sz);
  if (!buf) { f.close(); return false; }
  f.read(buf, sz);
  f.close();

  bool ok = false;
  if (jpeg.openRAM(buf, sz, jpegDrawCB)) {
    jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
    ok = jpeg.decode(0, 0, 0);
    jpeg.close();
  }
  free(buf);
  return ok;
}

// ── Draw position dot on map ──────────────────────────────────
void drawPosDot(float gpsPxX, float gpsPxY) {
  int dotX = (int)(gpsPxX - _cropX);
  int dotY = (int)(gpsPxY - _cropY);
  if (dotX >= 0 && dotX < 172 && dotY >= 0 && dotY < 160) {
    gfx->fillCircle(dotX, dotY, 5, RGB565_RED);
    gfx->drawCircle(dotX, dotY, 5, RGB565_WHITE);
  }
}

// ── Draw info bar (bottom half) ───────────────────────────────
void drawInfoBar(double lat, double lon, int zoom, int tx, int ty) {
  gfx->fillRect(0, 160, 172, 160, RGB565_BLACK);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(1);

  gfx->setCursor(4, 168);
  gfx->printf("Lat: %.5f", lat);

  gfx->setCursor(4, 182);
  gfx->printf("Lon: %.5f", lon);

  gfx->setCursor(4, 196);
  gfx->printf("Zoom: %d  Tile: %d/%d", zoom, tx, ty);

  gfx->setTextColor(RGB565_GREEN);
  gfx->setCursor(4, 215);
  gfx->print("BikeGPS Phase 2 OK");
}

// ── Fake GPS waypoints for testing ───────────────────────────
struct WP { double lat, lon; const char *name; };
static const WP waypoints[] = {
  {41.9028, 12.4964, "Roma Colosseo"},
  {41.9009, 12.4833, "Roma Circo Massimo"},
  {41.8988, 12.4769, "Roma Garbatella"},
  {41.9100, 12.5000, "Roma Termini"},
};
static const int NUM_WP = sizeof(waypoints) / sizeof(waypoints[0]);

// ── Setup ─────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  delay(100);
  Serial.println("\n=== BikeGPS Phase 2: OSM Tiles ===");

  // Init SPI with MISO first
  SPI.begin(LCD_SCK, SD_MISO, LCD_MOSI, SD_CS);

  // Init display
  gfx->begin();
  lcd_reg_init();
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);

  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(10, 140);
  gfx->print("Mounting SD...");

  // Init SD
  if (!SD.begin(SD_CS)) {
    gfx->fillScreen(RGB565_RED);
    gfx->setCursor(10, 150);
    gfx->print("SD FAILED");
    Serial.println("SD mount failed!");
    return;
  }
  Serial.printf("SD OK — %llu MB\n", SD.cardSize() / (1024 * 1024));
}

// ── Loop — cycle through waypoints every 3s ───────────────────
void loop() {
  static int wpIdx = 0;
  static unsigned long lastSwitch = 0;

  if (millis() - lastSwitch < 3000) return;
  lastSwitch = millis();

  const WP &wp = waypoints[wpIdx];
  int zoom = 15;

  Serial.printf("\n[%s] lat=%.5f lon=%.5f zoom=%d\n",
    wp.name, wp.lat, wp.lon, zoom);

  TileCoord tc = latLonToTile(wp.lat, wp.lon, zoom);
  Serial.printf("Tile: %d/%d  pixPos: %.1f,%.1f\n",
    tc.x, tc.y, tc.pixX, tc.pixY);

  // Show tile
  bool ok = showTile(zoom, tc.x, tc.y, tc.pixX, tc.pixY);

  if (ok) {
    drawPosDot(tc.pixX, tc.pixY);
    drawInfoBar(wp.lat, wp.lon, zoom, tc.x, tc.y);
    Serial.println("Tile displayed OK");
  } else {
    // Tile not on SD — show placeholder
    gfx->fillRect(0, 0, 172, 160, RGB565_DARKGREY);
    gfx->setTextColor(RGB565_WHITE);
    gfx->setTextSize(1);
    gfx->setCursor(4, 75);
    gfx->printf("Tile missing:\n/tiles/%d/%d/%d.jpg", zoom, tc.x, tc.y);
    drawInfoBar(wp.lat, wp.lon, zoom, tc.x, tc.y);
    Serial.printf("Missing: /tiles/%d/%d/%d.jpg\n", zoom, tc.x, tc.y);
  }

  wpIdx = (wpIdx + 1) % NUM_WP;
}
