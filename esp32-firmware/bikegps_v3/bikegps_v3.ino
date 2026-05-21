/*
 * BikeGPS v3 — Full firmware
 *
 * Board : Waveshare ESP32-C6-Touch-LCD-1.47 (TOUCH version)
 * FQBN  : esp32:esp32:esp32c6:CDCOnBoot=cdc
 *
 * iPhone → BLE NUS JSON: {"lat":..,"lon":..,"speed":..,"heading":..}
 * SD tiles: /tiles/{z}/{x}/{y}.jpg  (map)
 *           /tiles/sat/{z}/{x}/{y}.jpg  (satellite)
 *
 * Touch cycles through 5 display modes:
 *   0  MAP_FULL   — map full screen + speed/pos overlay
 *   1  SAT_FULL   — satellite full screen + speed/pos overlay
 *   2  MAP_INFO   — map top half + info bar bottom
 *   3  SAT_INFO   — satellite top half + info bar bottom
 *   4  SPEED_ONLY — large speed + temperature, no map
 */

#include <Arduino_GFX_Library.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>
#include <JPEGDEC.h>
#include <math.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <ArduinoJson.h>
#include "esp_lcd_touch_axs5106l.h"

// ── Pins (TOUCH version) ─────────────────────────────────────
#define LCD_DC      15
#define LCD_CS      14
#define LCD_SCK      1
#define LCD_MOSI     2
#define LCD_RST     22
#define LCD_BL      23
#define SD_MISO      3
#define SD_CS        4
#define TOUCH_SDA   18
#define TOUCH_SCL   19
#define TOUCH_RST   20
#define TOUCH_INT   21

// ── BLE NUS UUIDs ────────────────────────────────────────────
#define NUS_SERVICE_UUID  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID       "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX_UUID       "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

// ── Display modes ─────────────────────────────────────────────
enum DisplayMode : uint8_t {
  MAP_FULL   = 0,   // map full screen + overlay
  SAT_FULL   = 1,   // satellite full screen + overlay
  MAP_INFO   = 2,   // map top + info bar bottom
  SAT_INFO   = 3,   // satellite top + info bar bottom
  SPEED_ONLY = 4,   // large speed + temp
  NAV_MODE   = 5    // turn-by-turn navigation (auto-only, not in touch cycle)
};
#define NUM_TOUCH_MODES 5   // touch cycles through modes 0..4 only

// ── Tile coord struct (must be before any function definition) ─
struct TileCoord { int x, y; float pixX, pixY; };

// ── Display ──────────────────────────────────────────────────
Arduino_DataBus *bus = new Arduino_HWSPI(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI);
Arduino_GFX    *gfx = new Arduino_ST7789(bus, LCD_RST, 0, false, 172, 320, 34, 0, 34, 0);

JPEGDEC jpeg;

// ── Global state ─────────────────────────────────────────────
volatile bool        gNewGps    = false;
volatile double      gLat       = 0.0,  gLon = 0.0;
volatile float       gSpeed     = 0.0f, gHeading = 0.0f;
volatile bool        gBleConn   = false;
static   DisplayMode gMode      = MAP_INFO;
static   float       gTempC     = NAN;
static   unsigned long gLastTempMs  = 0;
static   int           gZoom        = 15;
static   bool          gZoomChanged = false;
static   unsigned long gLastTouchMs = 0;
static   char          gTimeStr[6]  = "--:--";
static   float         gMaxSpeed    = 0.0f;
static   float         gDistKm      = 0.0f;
static   float         gAltM        = NAN;
static   float         gBatPct      = NAN;   // 0.0–1.0 da iPhone
static   float         gWtmp        = NAN;   // temperatura meteo esterna
static   int           gWrain       = -1;    // probabilità pioggia %
static   double        gPrevLat     = 0.0, gPrevLon = 0.0;
static   bool          gHasPrev     = false;
static   unsigned long gLastMoveMs  = 0;
static   bool          gIsDimmed    = false;
static   bool          gSdOk        = false;
static   bool          gHasValidGps = false;

// ── Navigation state ─────────────────────────────────────────
volatile int  gNavMan    = -1;       // -1=off 0=straight 1=right 2=left 3=uturn 4=arrive
volatile int  gNavDist   = 0;        // metres to next maneuver
static   char gNavStreet[28] = "";   // instruction text (truncated, ≤27 chars)
static   DisplayMode gPrevMode = MAP_INFO;   // mode before NAV_MODE auto-switch

// ── JD9853 init ───────────────────────────────────────────────
void lcd_reg_init() {
  static const uint8_t ops[] = {
    BEGIN_WRITE,
    WRITE_COMMAND_8, 0x11, END_WRITE, DELAY, 120,
    BEGIN_WRITE,
    WRITE_C8_D16, 0xDF, 0x98, 0x53,
    WRITE_C8_D8,  0xB2, 0x23,
    WRITE_COMMAND_8, 0xB7, WRITE_BYTES, 4, 0x00,0x47,0x00,0x6F,
    WRITE_COMMAND_8, 0xBB, WRITE_BYTES, 6, 0x1C,0x1A,0x55,0x73,0x63,0xF0,
    WRITE_C8_D16, 0xC0, 0x44, 0xA4,
    WRITE_C8_D8,  0xC1, 0x16,
    WRITE_COMMAND_8, 0xC3, WRITE_BYTES, 8,  0x7D,0x07,0x14,0x06,0xCF,0x71,0x72,0x77,
    WRITE_COMMAND_8, 0xC4, WRITE_BYTES, 12, 0x00,0x00,0xA0,0x79,0x0B,0x0A,0x16,0x79,0x0B,0x0A,0x16,0x82,
    WRITE_COMMAND_8, 0xC8, WRITE_BYTES, 32,
      0x3F,0x32,0x29,0x29,0x27,0x2B,0x27,0x28,0x28,0x26,0x25,0x17,0x12,0x0D,0x04,0x00,
      0x3F,0x32,0x29,0x29,0x27,0x2B,0x27,0x28,0x28,0x26,0x25,0x17,0x12,0x0D,0x04,0x00,
    WRITE_COMMAND_8, 0xD0, WRITE_BYTES, 5, 0x04,0x06,0x6B,0x0F,0x00,
    WRITE_C8_D16, 0xD7, 0x00,0x30,
    WRITE_C8_D8,  0xE6, 0x14,
    WRITE_C8_D8,  0xDE, 0x01,
    WRITE_COMMAND_8, 0xB7, WRITE_BYTES, 5, 0x03,0x13,0xEF,0x35,0x35,
    WRITE_COMMAND_8, 0xC1, WRITE_BYTES, 3, 0x14,0x15,0xC0,
    WRITE_C8_D16, 0xC2, 0x06,0x3A,
    WRITE_C8_D16, 0xC4, 0x72,0x12,
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
    END_WRITE, DELAY, 10,
    BEGIN_WRITE, WRITE_COMMAND_8, 0x29, END_WRITE
  };
  bus->batchOperation(ops, sizeof(ops));
}

// ── Haversine (metri tra due coordinate) ─────────────────────
static float haversineM(double lat1, double lon1, double lat2, double lon2) {
  double dLat = (lat2 - lat1) * M_PI / 180.0;
  double dLon = (lon2 - lon1) * M_PI / 180.0;
  double a = sin(dLat/2)*sin(dLat/2) +
             cos(lat1*M_PI/180.0)*cos(lat2*M_PI/180.0)*sin(dLon/2)*sin(dLon/2);
  return (float)(6371000.0 * 2.0 * atan2(sqrt(a), sqrt(1.0 - a)));
}

// ── OSM tile math ─────────────────────────────────────────────
TileCoord latLonToTile(double lat, double lon, int zoom) {
  double n = pow(2.0, zoom), lat_r = lat * M_PI / 180.0;
  double fx = (lon + 180.0) / 360.0 * n;
  double fy = (1.0 - log(tan(lat_r) + 1.0 / cos(lat_r)) / M_PI) / 2.0 * n;
  return { (int)fx, (int)fy, (float)((fx-(int)fx)*256.0f), (float)((fy-(int)fy)*256.0f) };
}

// ── JPEG draw callback ────────────────────────────────────────
static int _cropX, _cropY;    // crop origin within 256×256 tile
static int _dstY;              // destination y on screen
static int _mapH;              // tile rows da processare
static int _tileScale = 2;     // 1=1:1, 2=2x upscale (mappe più grandi)

int jpegDrawCB(JPEGDRAW *pDraw) {
  int sx = pDraw->x, sy = pDraw->y, sw = pDraw->iWidth, sh = pDraw->iHeight;
  int x0 = max(sx, _cropX),  x1 = min(sx + sw, _cropX + 172 / _tileScale);
  int y0 = max(sy, _cropY),  y1 = min(sy + sh, _cropY + _mapH);
  if (x1 <= x0 || y1 <= y0) return 1;

  if (_tileScale == 1) {
    for (int row = y0; row < y1; row++) {
      uint16_t *p = pDraw->pPixels + (row - sy) * sw + (x0 - sx);
      gfx->draw16bitRGBBitmap(x0 - _cropX, _dstY + (row - _cropY), p, x1 - x0, 1);
    }
  } else {
    // 2x upscale: ogni pixel tile → blocco 2×2 su schermo
    static uint16_t rowBuf[172];
    for (int row = y0; row < y1; row++) {
      uint16_t *src = pDraw->pPixels + (row - sy) * sw;
      int dstCol = 0;
      for (int col = x0; col < x1; col++) {
        uint16_t pix = src[col - sx];
        rowBuf[dstCol++] = pix;
        rowBuf[dstCol++] = pix;
      }
      int dstX = (x0 - _cropX) * _tileScale;
      int dstW = (x1 - x0)     * _tileScale;
      int dstRow = _dstY + (row - _cropY) * _tileScale;
      gfx->draw16bitRGBBitmap(dstX, dstRow,     rowBuf, dstW, 1);
      gfx->draw16bitRGBBitmap(dstX, dstRow + 1, rowBuf, dstW, 1);
    }
  }
  return 1;
}

// ── Load tile from SD → screen ───────────────────────────────
// fullScreen=false: 172×160 at y=0 (MAP_INFO/SAT_INFO)
// fullScreen=true:  172×256 at y=32 (MAP_FULL/SAT_FULL)
bool showTile(int z, int tx, int ty, float gpsPxX, float gpsPxY,
              bool satMode, bool fullScreen) {
  int visW = 172 / _tileScale;   // pixel tile orizzontali visibili
  if (fullScreen) {
    int visH = 256 / _tileScale;
    _cropX = constrain((int)(gpsPxX - visW / 2), 0, 256 - visW);
    _cropY = constrain((int)(gpsPxY - visH / 2), 0, 256 - visH);
    _mapH  = visH;
    _dstY  = 32;
  } else {
    int visH = 160 / _tileScale;
    _cropX = constrain((int)(gpsPxX - visW / 2), 0, 256 - visW);
    _cropY = constrain((int)(gpsPxY - visH / 2), 0, 256 - visH);
    _mapH  = visH;
    _dstY  = 0;
  }

  char path[72];
  if (satMode) snprintf(path, sizeof(path), "/tiles/sat/%d/%d/%d.jpg", z, tx, ty);
  else         snprintf(path, sizeof(path), "/tiles/%d/%d/%d.jpg",     z, tx, ty);
  Serial.printf("Tile: %s  crop(%d,%d)\n", path, _cropX, _cropY);

  if (!gSdOk) { gSdOk = SD.begin(SD_CS); }
  if (!gSdOk) return false;

  File f = SD.open(path);
  if (!f) return false;
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

// ── Position dot ─────────────────────────────────────────────
void drawPosDot(float gpsPxX, float gpsPxY, bool fullScreen) {
  int dx = (int)(gpsPxX - _cropX) * _tileScale;
  int dy = _dstY + (int)(gpsPxY - _cropY) * _tileScale;
  int yMax = fullScreen ? 287 : 155;
  if (dx >= 4 && dx < 168 && dy >= _dstY + 4 && dy < yMax) {
    gfx->fillCircle(dx, dy, 5, RGB565_RED);
    gfx->drawCircle(dx, dy, 5, RGB565_WHITE);
    gfx->drawCircle(dx, dy, 6, RGB565_BLACK);
  }
}

// ── Helpers ───────────────────────────────────────────────────
const char *headingStr(float h) {
  const char *d[] = {"N","NE","E","SE","S","SW","W","NW"};
  return d[(int)((h + 22.5f) / 45.0f) % 8];
}

const char *modeName(DisplayMode m) {
  const char *n[] = {"MAP FULL","SAT FULL","MAP+INFO","SAT+INFO","SPEED","NAV"};
  int i = (int)m;
  return (i >= 0 && i <= 5) ? n[i] : "?";
}

// ── Draw: full-screen overlay (modes 0 and 1) ─────────────────
// Tile already drawn at y=32..287. We draw:
//   y=0..31   dark bar → speed + heading
//   y=288..319 dark bar → lat/lon + mode badge
void drawFullOverlay(float speed, float heading, double lat, double lon,
                     bool gpsLive, bool satMode, float tempC, int zoom) {
  // Top bar
  gfx->fillRect(0, 0, 172, 32, 0x0821 /* very dark grey */);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(4, 6);
  if (gpsLive) gfx->printf("%.0f", speed);
  else         gfx->print("--");
  gfx->setTextSize(1);
  gfx->setTextColor(RGB565_DARKGREY);
  gfx->setCursor(36, 8);
  gfx->print("km/h");
  gfx->setTextColor(RGB565_YELLOW);
  gfx->setCursor(72, 4);
  if (gpsLive) gfx->printf("%.0f\xB0 %s", heading, headingStr(heading));
  // Mode badge top-right: "MAP z15" / "SAT z14" etc.
  uint16_t badgeC = satMode ? 0x0410 : 0x000F;
  gfx->fillRoundRect(110, 4, 58, 14, 3, badgeC);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(113, 8);
  gfx->printf("%s z%d", satMode ? "SAT" : "MAP", zoom);

  // Bottom bar
  gfx->fillRect(0, 288, 172, 32, 0x0821);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(1);
  gfx->setCursor(4, 292);
  if (gpsLive) gfx->printf("%.5f", lat);
  else         gfx->print("---.-----");
  gfx->setCursor(4, 306);
  if (gpsLive) gfx->printf("%.5f", lon);
  else         gfx->print("---.-----");
  if (!isnan(tempC)) {
    gfx->setTextColor(RGB565_YELLOW);
    gfx->setCursor(100, 306);
    gfx->printf("%.1f\xB0""C", tempC);
  }
}

// ── Draw: info bar (modes 2 and 3, y=160..319) ───────────────
void drawInfoBar(double lat, double lon, float speed, float heading,
                 int zoom, int tx, int ty,
                 bool bleConn, bool gpsLive, float tempC, bool satMode) {
  gfx->fillRect(0, 160, 172, 160, RGB565_BLACK);

  gfx->fillCircle(8, 170, 4, bleConn ? RGB565_CYAN : RGB565_DARKGREY);
  gfx->setTextColor(bleConn ? RGB565_CYAN : RGB565_DARKGREY);
  gfx->setTextSize(1);
  gfx->setCursor(18, 166);
  gfx->print(bleConn ? "BLE Connected" : "BLE Waiting...");

  gfx->fillCircle(8, 184, 4, gpsLive ? RGB565_GREEN : RGB565_DARKGREY);
  gfx->setTextColor(gpsLive ? RGB565_GREEN : RGB565_DARKGREY);
  gfx->setCursor(18, 180);
  gfx->print(gpsLive ? "GPS Live" : "GPS --");

  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(4, 196);
  if (gpsLive) gfx->printf("%.1f km/h", speed);
  else         gfx->print("-- km/h");

  gfx->setTextSize(1);
  gfx->setTextColor(RGB565_YELLOW);
  gfx->setCursor(4, 218);
  if (gpsLive) gfx->printf("HDG: %.0f\xB0 %s", heading, headingStr(heading));
  else         gfx->print("HDG: ---");

  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(4, 232);
  if (gpsLive) gfx->printf("%.5f", lat);
  else         gfx->print("---.-----");
  gfx->setCursor(96, 232);
  if (!isnan(tempC)) { gfx->setTextColor(RGB565_YELLOW); gfx->printf("%.1f\xB0""C", tempC); }

  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(4, 246);
  if (gpsLive) gfx->printf("%.5f", lon);
  else         gfx->print("---.-----");

  // Distanza + velocità max
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(4, 258);
  gfx->printf("%.1fkm  max:%.0f", gDistKm, gMaxSpeed);

  // Altitudine | batteria
  gfx->setTextColor(RGB565_DARKGREY);
  gfx->setCursor(4, 270);
  if (!isnan(gAltM)) gfx->printf("alt %.0fm", gAltM);
  if (!isnan(gBatPct)) {
    gfx->setTextColor(gBatPct < 0.2f ? RGB565_RED : RGB565_DARKGREY);
    gfx->setCursor(96, 270);
    gfx->printf("bat%d%%", (int)(gBatPct * 100));
  }

  // Meteo esterno
  if (!isnan(gWtmp) || gWrain >= 0) {
    gfx->setTextColor(RGB565_YELLOW);
    gfx->setCursor(4, 282);
    if (!isnan(gWtmp)) gfx->printf("ext %.0f\xB0""C", gWtmp);
    if (gWrain >= 0) {
      gfx->setTextColor(gWrain > 50 ? RGB565_CYAN : RGB565_DARKGREY);
      gfx->setCursor(96, 282);
      gfx->printf("piog%d%%", gWrain);
    }
  }

  // Layer badge
  gfx->setTextColor(satMode ? 0x07E0 : 0x001F);
  gfx->setCursor(4, 296);
  gfx->printf("[ %s z%d ]", satMode ? "SAT" : "MAP", zoom);

  gfx->setTextColor(0x2945);
  gfx->setCursor(4, 308);
  gfx->print("BikeGPS v3");
}

// ── Draw: speed only (mode 4) ─────────────────────────────────
void drawSpeedOnly(float speed, float heading, float tempC,
                   bool bleConn, bool gpsLive, const char *timeStr) {
  gfx->fillScreen(RGB565_BLACK);

  // Velocità grande (textSize 8 → 48×64px/char)
  gfx->setTextColor(gpsLive ? RGB565_WHITE : RGB565_DARKGREY);
  gfx->setTextSize(8);
  char spd[8];
  if (gpsLive) snprintf(spd, sizeof(spd), "%.0f", speed);
  else         snprintf(spd, sizeof(spd), "--");
  int sw = strlen(spd) * 48;
  gfx->setCursor((172 - sw) / 2, 12);
  gfx->print(spd);

  // km/h  |  batteria iPhone (destra)
  gfx->setTextColor(RGB565_DARKGREY);
  gfx->setTextSize(2);
  gfx->setCursor(4, 84);
  gfx->print("km/h");
  if (gBatPct >= 0.0f) {
    gfx->setTextColor(gBatPct < 0.2f ? RGB565_RED : RGB565_DARKGREY);
    gfx->setCursor(96, 84);
    gfx->printf("bat %d%%", (int)(gBatPct * 100));
  }

  // Ora (textSize 3, centrata)
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(3);
  int tw = strlen(timeStr) * 18;
  gfx->setCursor((172 - tw) / 2, 106);
  gfx->print(timeStr);

  // Direzione (senza simbolo gradi — la font lo renderizza male)
  gfx->setTextColor(RGB565_YELLOW);
  gfx->setTextSize(2);
  gfx->setCursor(4, 142);
  if (gpsLive) gfx->printf("%s %.0f", headingStr(heading), heading);
  else         gfx->print("---");

  // Altitudine
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(4, 162);
  if (!isnan(gAltM)) gfx->printf("alt %.0fm", gAltM);
  else               gfx->print("alt ---m");

  // Distanza + velocità max
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(4, 182);
  gfx->printf("%.1fkm  max:%.0f", gDistKm, gMaxSpeed);

  // Temperatura chip | meteo esterno (senza simbolo gradi)
  gfx->setTextColor(RGB565_CYAN);
  gfx->setTextSize(2);
  gfx->setCursor(4, 206);
  if (!isnan(tempC)) gfx->printf("%.0fC", tempC);
  if (!isnan(gWtmp)) {
    gfx->setTextColor(RGB565_YELLOW);
    gfx->setCursor(72, 206);
    gfx->printf("ext %.0fC", gWtmp);
  }
  if (gWrain >= 0) {
    gfx->setTextColor(gWrain > 50 ? RGB565_CYAN : RGB565_DARKGREY);
    gfx->setCursor(4, 222);
    gfx->printf("pioggia %d%%", gWrain);
  }

  // Status
  gfx->fillCircle(12, 272, 5, bleConn ? RGB565_CYAN : RGB565_DARKGREY);
  gfx->setTextSize(1);
  gfx->setTextColor(bleConn ? RGB565_CYAN : RGB565_DARKGREY);
  gfx->setCursor(22, 268);
  gfx->print("BLE");

  gfx->fillCircle(12, 288, 5, gpsLive ? RGB565_GREEN : RGB565_DARKGREY);
  gfx->setTextColor(gpsLive ? RGB565_GREEN : RGB565_DARKGREY);
  gfx->setCursor(22, 284);
  gfx->print(gpsLive ? "GPS Live" : "GPS --");

  gfx->setTextColor(0x2945);
  gfx->setCursor(4, 308);
  gfx->print("BikeGPS v3");
}

// ── Missing tile placeholder ──────────────────────────────────
void showMissingTile(int z, int tx, int ty, bool satMode, bool fullScreen) {
  int y0 = fullScreen ? 32 : 0;
  int h  = fullScreen ? 256 : 160;
  gfx->fillRect(0, y0, 172, h, 0x2104);
  gfx->setTextColor(RGB565_DARKGREY);
  gfx->setTextSize(1);
  gfx->setCursor(4, y0 + 50);
  gfx->printf("%s\n/tiles/%d/%d/%d.jpg", satMode ? "[SAT]" : "[MAP]", z, tx, ty);
  gfx->setCursor(4, y0 + 80);
  gfx->print("tile missing");
}

// ── Navigation arrow (y=32..207, 176×172 area) ───────────────
// Arrow is drawn around center (cx=86, cy=119).
void drawNavArrow(int maneuver, uint16_t bgColor) {
  const uint16_t AC = RGB565_WHITE;
  const int cx = 86, cy = 119;

  gfx->fillRect(0, 32, 172, 176, bgColor);   // clear arrow area

  switch (maneuver) {

    case 0:   // ↑ Straight ahead
      // Shaft: 32px wide, 85px tall below mid-point
      gfx->fillRect(cx - 16, cy, 32, 85, AC);
      // Head: wide triangle pointing up
      gfx->fillTriangle(cx - 52, cy, cx + 52, cy, cx, cy - 76, AC);
      break;

    case 1:   // → Turn right
      // Horizontal shaft going left from mid-point
      gfx->fillRect(cx - 62, cy - 16, 74, 32, AC);
      // Arrow head pointing right
      gfx->fillTriangle(cx + 12, cy - 54, cx + 12, cy + 54, cx + 74, cy, AC);
      break;

    case 2:   // ← Turn left
      // Horizontal shaft going right from mid-point
      gfx->fillRect(cx - 12, cy - 16, 74, 32, AC);
      // Arrow head pointing left
      gfx->fillTriangle(cx - 12, cy - 54, cx - 12, cy + 54, cx - 74, cy, AC);
      break;

    case 3: { // ↺ U-turn
      // Right leg going down
      gfx->fillRect(cx + 16, cy - 65, 22, 78, AC);
      // Bottom connector
      gfx->fillRect(cx - 16, cy + 13, 32, 22, AC);
      // Left leg going up
      gfx->fillRect(cx - 38, cy - 55, 22, 68, AC);
      // Arrow head pointing UP on left leg
      gfx->fillTriangle(cx - 56, cy - 55, cx - 20, cy - 55, cx - 38, cy - 86, AC);
      break;
    }

    case 4:   // ⬤ Arrive — map pin
      gfx->fillCircle(cx, cy - 18, 40, AC);          // pin head
      gfx->fillCircle(cx, cy - 18, 24, bgColor);     // hollow ring
      gfx->fillCircle(cx, cy - 18, 10, AC);           // centre dot
      gfx->fillTriangle(cx - 20, cy + 16, cx + 20, cy + 16, cx, cy + 56, AC);  // pin tail
      break;

    default:  // Unknown — filled circle
      gfx->fillCircle(cx, cy, 42, AC);
      gfx->fillCircle(cx, cy, 28, bgColor);
      break;
  }
}

// ── Draw: navigation mode (full-screen, 172×320) ─────────────
void drawNavMode(int maneuver, int distM, const char *street,
                 float speed, float heading,
                 bool bleConn, bool gpsLive, const char *timeStr) {

  // Background colour changes with proximity to next maneuver
  uint16_t bgColor;
  if      (maneuver == 4) bgColor = 0x0340;   // arrived  — dark green
  else if (distM < 50)    bgColor = 0xA000;   // imminent — dark red
  else if (distM < 200)   bgColor = 0x8400;   // close    — amber
  else                    bgColor = 0x000A;   // far      — dark navy

  gfx->fillScreen(bgColor);

  // ── Top bar y=0..31: speed + heading + time ──────────────────
  gfx->fillRect(0, 0, 172, 32, 0x0821);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(4, 8);
  if (gpsLive) gfx->printf("%.0f km/h", speed);
  else         gfx->print("-- km/h");
  gfx->setTextSize(1);
  gfx->setTextColor(RGB565_YELLOW);
  gfx->setCursor(108, 5);
  gfx->print(timeStr);
  gfx->setCursor(108, 18);
  if (gpsLive) gfx->printf("%s %.0f", headingStr(heading), heading);

  // ── Arrow y=32..207 ──────────────────────────────────────────
  drawNavArrow(maneuver, bgColor);

  // ── Distance y=210..249 ──────────────────────────────────────
  char distStr[16];
  if      (maneuver == 4) snprintf(distStr, sizeof(distStr), "ARRIVO!");
  else if (distM >= 1000) snprintf(distStr, sizeof(distStr), "%.1f km", distM / 1000.0f);
  else                    snprintf(distStr, sizeof(distStr), "%d m", distM);

  uint8_t dsz = (distM >= 1000 || maneuver == 4) ? 3 : 4;
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(dsz);
  int tw = strlen(distStr) * (dsz * 6);
  gfx->setCursor((172 - tw) / 2, 212);
  gfx->print(distStr);

  // ── Street / instruction y=252..259 (textSize 1, no wrap) ────
  gfx->setTextWrap(false);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(1);
  gfx->setCursor(4, 254);
  gfx->print(street);
  gfx->setTextWrap(true);

  // ── Heading y=266..281 ───────────────────────────────────────
  gfx->setTextColor(RGB565_YELLOW);
  gfx->setTextSize(2);
  gfx->setCursor(4, 268);
  if (gpsLive) gfx->printf("%.0f  %s", heading, headingStr(heading));

  // ── Status dots y=290..307 ───────────────────────────────────
  gfx->fillCircle(8, 292, 4, bleConn ? RGB565_CYAN : RGB565_DARKGREY);
  gfx->setTextSize(1);
  gfx->setTextColor(bleConn ? RGB565_CYAN : RGB565_DARKGREY);
  gfx->setCursor(18, 288);
  gfx->print("BLE");

  gfx->fillCircle(8, 304, 4, gpsLive ? RGB565_GREEN : RGB565_DARKGREY);
  gfx->setTextColor(gpsLive ? RGB565_GREEN : RGB565_DARKGREY);
  gfx->setCursor(18, 300);
  gfx->print(gpsLive ? "GPS" : "GPS--");

  gfx->setTextColor(0x2945);
  gfx->setCursor(100, 312);
  gfx->print("BikeGPS v3");
}

// ── BLE callbacks ─────────────────────────────────────────────
class BleServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer*)    override { gBleConn = true;  Serial.println("BLE connected"); }
  void onDisconnect(BLEServer*) override {
    gBleConn  = false;
    gNavMan   = -1;   // exit NAV_MODE if BLE drops while navigating
    Serial.println("BLE disconnected");
    BLEDevice::startAdvertising();
  }
};

class RxCharCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pChar) override {
    String val = pChar->getValue();
    if (val.length() == 0) return;
    JsonDocument doc;
    if (deserializeJson(doc, val.c_str())) return;
    if (!doc["lat"].is<double>() || !doc["lon"].is<double>()) return;
    gLat     = doc["lat"].as<double>();
    gLon     = doc["lon"].as<double>();
    gSpeed   = doc["speed"]   | 0.0f;
    gHeading = doc["heading"] | 0.0f;
    if (doc["time"].is<const char*>()) {
      strncpy(gTimeStr, doc["time"].as<const char*>(), 5);
      gTimeStr[5] = '\0';
    }
    if (doc["alt"].is<float>())   gAltM   = doc["alt"].as<float>();
    if (doc["bat"].is<float>())   gBatPct = doc["bat"].as<float>();
    if (doc["wtmp"].is<float>())  gWtmp   = doc["wtmp"].as<float>();
    if (doc["wrain"].is<int>())   gWrain  = doc["wrain"].as<int>();
    // Navigation fields
    gNavMan  = doc["nav"]   | -1;
    gNavDist = doc["ndist"] | 0;
    if (doc["nst"].is<const char*>()) {
      strncpy(gNavStreet, doc["nst"].as<const char*>(), 27);
      gNavStreet[27] = '\0';
    }
    gNewGps  = true;
  }
};

// ── Setup ─────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  delay(100);
  Serial.println("\n=== BikeGPS v3 ===");

  SPI.begin(LCD_SCK, SD_MISO, LCD_MOSI, SD_CS);
  gfx->begin();
  lcd_reg_init();
  pinMode(LCD_BL, OUTPUT);
  analogWrite(LCD_BL, 255);
  gfx->fillScreen(RGB565_BLACK);

  // Splash
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(22, 140);
  gfx->print("BikeGPS v3");
  gfx->setTextSize(1);
  gfx->setTextColor(RGB565_DARKGREY);
  gfx->setCursor(46, 164);
  gfx->print("Loading...");

  gSdOk = SD.begin(SD_CS);
  if (!gSdOk) {
    Serial.println("SD mount failed — continuing without map tiles");
    gfx->fillRect(0, 60, 172, 40, RGB565_RED);
    gfx->setTextColor(RGB565_WHITE); gfx->setTextSize(1);
    gfx->setCursor(10, 72); gfx->print("SD non trovata");
    gfx->setCursor(10, 84); gfx->print("Inserisci la SD");
    delay(2000);
  } else {
    Serial.printf("SD OK — %llu MB\n", SD.cardSize()/(1024*1024));
  }

  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  bsp_touch_init(&Wire, TOUCH_RST, TOUCH_INT, 0, 172, 320);

  gTempC      = temperatureRead();
  gLastTempMs = millis();
  gLastMoveMs = millis();

  BLEDevice::init("BikeGPS");
  BLEDevice::setPower(ESP_PWR_LVL_P9);
  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new BleServerCB());
  BLEService *pSvc = pServer->createService(NUS_SERVICE_UUID);
  BLECharacteristic *pRx = pSvc->createCharacteristic(
    NUS_RX_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  pRx->setCallbacks(new RxCharCB());
  BLECharacteristic *pTx = pSvc->createCharacteristic(NUS_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  pTx->addDescriptor(new BLE2902());
  pSvc->start();
  BLEAdvertising *pAdv = BLEDevice::getAdvertising();
  pAdv->addServiceUUID(NUS_SERVICE_UUID);
  pAdv->setScanResponse(true);
  pAdv->setMinPreferred(0x06);
  BLEDevice::startAdvertising();
  Serial.println("BLE advertising as 'BikeGPS'");

  // Initial screen
  gfx->fillScreen(RGB565_BLACK);
  drawInfoBar(0,0,0,0,15,0,0,false,false,gTempC,false);
  gfx->fillRect(0,0,172,160,0x2104);
  gfx->setTextColor(RGB565_DARKGREY);
  gfx->setTextSize(1);
  gfx->setCursor(18,70); gfx->print("Waiting for GPS...");
  gfx->setCursor(26,84); gfx->print("Connect iPhone");
}

// ── Loop ──────────────────────────────────────────────────────
static int         lastTileX   = -1, lastTileY = -1;
static float       lastDotPxX  = -100, lastDotPxY = -100;
static DisplayMode lastMode    = (DisplayMode)255;
// gZoom è definito in global state (default 15)

void loop() {
  // ── Temperature every 5s ──
  if (millis() - gLastTempMs > 5000) {
    gTempC      = temperatureRead();
    gLastTempMs = millis();
  }

  // ── Auto-dim: 5 min senza movimento ──
  if (!gIsDimmed && millis() - gLastMoveMs > 300000UL) {
    analogWrite(LCD_BL, 50);
    gIsDimmed = true;
  }

  // ── Nav: auto-switch to/from NAV_MODE ───────────────────────
  {
    static int prevNavMan = -99;
    int curNav = (int)gNavMan;
    if (curNav != prevNavMan) {
      if (curNav >= 0 && gMode != NAV_MODE) {
        gPrevMode = gMode;
        gMode     = NAV_MODE;
        lastTileX = -1; lastTileY = -1;
        Serial.printf("NAV start → mode NAV\n");
      } else if (curNav < 0 && gMode == NAV_MODE) {
        gMode     = gPrevMode;
        lastTileX = -1; lastTileY = -1;
        Serial.printf("NAV end   → mode %s\n", modeName(gMode));
      }
      prevNavMan = curNav;
    }
  }

  // ── Touch: metà sup → zoom, metà inf → cicla visuale ──
  {
    touch_data_t td;
    bsp_touch_read();
    if (bsp_touch_get_coordinates(&td)) {
      unsigned long now = millis();
      if (now - gLastTouchMs > 600 && gMode != NAV_MODE) {
        gLastTouchMs = now;
        if (td.coords[0].y < 160) {
          gZoom = (gZoom == 15) ? 14 : 15;
          gZoomChanged = true;
          Serial.printf("Zoom z%d\n", gZoom);
        } else {
          gMode = (DisplayMode)((gMode + 1) % NUM_TOUCH_MODES);
          Serial.printf("Mode: %s\n", modeName(gMode));
        }
      }
    }
  }

  bool modeChanged = (gMode != lastMode);
  if (!gNewGps && !modeChanged && !gZoomChanged) return;
  if (gZoomChanged) { lastTileX = -1; lastTileY = -1; gZoomChanged = false; }

  bool hadNewGps = gNewGps;
  gNewGps = false;

  // ── Distanza, velocità max, wake backlight ──
  if (hadNewGps) {
    float spd = gSpeed;
    if (spd > gMaxSpeed) gMaxSpeed = spd;
    if (spd > 0.5f) {
      gLastMoveMs = millis();
      if (gIsDimmed) { analogWrite(LCD_BL, 255); gIsDimmed = false; }
      if (gHasPrev) {
        float d = haversineM(gPrevLat, gPrevLon, gLat, gLon);
        if (d < 300.0f) gDistKm += d / 1000.0f;  // ignora salti >300m
      }
    }
    gPrevLat = gLat; gPrevLon = gLon; gHasPrev = true;
    gHasValidGps = true;
  }

  // SPEED_ONLY: no tile needed
  if (gMode == SPEED_ONLY) {
    if (hadNewGps || modeChanged) {
      drawSpeedOnly(gSpeed, gHeading, gTempC, gBleConn, hadNewGps || lastMode != SPEED_ONLY, gTimeStr);
    }
    lastMode = gMode;
    return;
  }

  // NAV_MODE: full-screen navigation overlay (auto-activated by BLE nav fields)
  if (gMode == NAV_MODE) {
    if (hadNewGps || modeChanged) {
      drawNavMode((int)gNavMan, (int)gNavDist, gNavStreet,
                  gSpeed, gHeading, gBleConn, gHasValidGps, gTimeStr);
    }
    lastMode = gMode;
    return;
  }

  // Senza GPS valido mostra schermata di attesa invece di tile (0,0)
  if (!gHasValidGps) {
    if (modeChanged) {
      int y0 = (gMode==MAP_FULL||gMode==SAT_FULL) ? 32 : 0;
      int h  = (gMode==MAP_FULL||gMode==SAT_FULL) ? 256 : 160;
      gfx->fillRect(0, y0, 172, h, 0x2104);
      gfx->setTextColor(RGB565_DARKGREY); gfx->setTextSize(1);
      gfx->setCursor(18, y0+60); gfx->print("Waiting for GPS...");
      gfx->setCursor(26, y0+74); gfx->print("Connect iPhone");
    }
    lastMode = gMode;
    return;
  }

  bool satMode   = (gMode == SAT_FULL || gMode == SAT_INFO);
  bool fullMode  = (gMode == MAP_FULL || gMode == SAT_FULL);

  double lat = gLat, lon = gLon;
  float  spd = gSpeed, hdg = gHeading;
  TileCoord tc = latLonToTile(lat, lon, gZoom);

  // Ricarica tile se: tile diversa, modo cambiato, zoom cambiato,
  // oppure il dot GPS si è spostato di >2px (per cancellare il vecchio dot)
  bool dotMoved = hadNewGps &&
                  (fabsf(tc.pixX - lastDotPxX) > 2.0f ||
                   fabsf(tc.pixY - lastDotPxY) > 2.0f);
  bool tileChanged = (tc.x != lastTileX || tc.y != lastTileY || modeChanged || dotMoved);

  if (tileChanged) {
    if (fullMode) gfx->fillRect(0, 32, 172, 256, 0x2104);
    else          gfx->fillRect(0, 0,  172, 160, 0x2104);

    bool ok = showTile(gZoom, tc.x, tc.y, tc.pixX, tc.pixY, satMode, fullMode);
    if (ok)  { drawPosDot(tc.pixX, tc.pixY, fullMode); lastDotPxX = tc.pixX; lastDotPxY = tc.pixY; }
    else     showMissingTile(gZoom, tc.x, tc.y, satMode, fullMode);

    lastTileX = tc.x;
    lastTileY = tc.y;
  }

  // Draw UI overlay / info bar
  if (fullMode) {
    drawFullOverlay(spd, hdg, lat, lon, hadNewGps || modeChanged, satMode, gTempC, gZoom);
  } else {
    drawInfoBar(lat, lon, spd, hdg, gZoom, tc.x, tc.y,
                gBleConn, hadNewGps || modeChanged, gTempC, satMode);
  }

  lastMode = gMode;
}
