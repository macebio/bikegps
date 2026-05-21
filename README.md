# BikeGPS

A DIY real-time GPS display for cyclists — an iPhone app sends your position and turn-by-turn navigation to a tiny ESP32 screen mounted on the handlebar via Bluetooth Low Energy.

![BikeGPS display showing map tile and navigation](icon.jpg)

> **⚠️ Prototipo v1** — questo è un progetto personale al suo primo rilascio pubblico. Funziona, ma ci sono sicuramente molte cose da migliorare: hardware, firmware, app, mount 3D. Ogni suggerimento, segnalazione di bug, pull request o idea è benvenuta. Il progetto è pensato per crescere con la comunità.

> **📱 Nota iOS** — per limiti imposti da Apple, l'app iPhone richiede di essere ricompilata da Xcode ogni 7 giorni (account gratuito). È scomodo ma non c'è alternativa senza pagare 99€/anno. [→ Leggi la guida completa](docs/ios-sideloading.md). Una **versione Android** è in lavorazione e non avrà questo problema.

---

## What it does

| Feature | Details |
|---|---|
| **Live map tile** | OSM or Esri satellite tiles pre-loaded on SD card, no internet needed while riding |
| **Speed + compass** | Real-time from iPhone GPS, displayed in large digits |
| **Turn-by-turn nav** | Search a destination on the iPhone → ESP32 shows arrow + distance + voice guidance |
| **Rerouting** | Automatically recalculates if you take a wrong turn |
| **Temperature** | ESP32 chip temperature sensor shown on screen |
| **Touch to zoom** | Tap top half of screen → toggle between z15 (detail) and z14 (overview) |
| **Touch to change view** | Tap bottom half → cycle Speed / Map+Info / Full Map / Satellite / Night |

---

## Hardware you need

| Component | Where to buy | Price (approx.) |
|---|---|---|
| **Waveshare ESP32-C6-Touch-LCD-1.47** *(touch version!)* | [AliExpress](https://www.aliexpress.com/item/1005009816465254.html) | ~$13–15 |
| microSD card (≥4 GB, Class 10) | Any | ~$5 |
| iPhone (iOS 16+) | — | you already have one |

> ⚠️ **Buy the TOUCH version** — the non-touch version has different GPIO pins and this firmware will not work on it.

No soldering, no extra wiring. Everything — display, touch, BLE, SD slot — is on the Waveshare module.

---

## 3D-printed handlebar mount

The `3d-mounts/` directory contains a STEP file for a handlebar mount:

```
3d-mounts/
├── bikegps_mount_v1.step   ← primo prototipo (STEP AP214)
└── README.md               ← istruzioni stampa + hardware
```

> **⚠️ Prototipo v1** — funziona ma probabilmente ha margini di miglioramento. Modifiche, varianti e versioni alternative sono molto benvenute.

**Hardware necessario:**
- 4× viti **M2.5** (~6 mm) — fissaggio modulo LCD al tray
- 2× bulloni **M4** (~20 mm) + dado — C-clamp sul manubrio

Stampa in PETG o ASA (resistenza UV/intemperie), infill 40%.

---

## Architecture

```
┌──────────────────────────────────────┐
│          iPhone (iOS app)            │
│  CoreLocation → GPS coordinates      │
│  MapKit       → turn-by-turn routing │
│  AVFoundation → Italian voice nav    │
│  Sends JSON via BLE NUS every ~1 s   │
└──────────────────┬───────────────────┘
                   │ Bluetooth LE (Nordic NUS)
                   │ {"lat":…,"lon":…,"speed":…,
                   │  "heading":…,"nav":1,"ndist":120}
┌──────────────────▼───────────────────┐
│     Waveshare ESP32-C6 (firmware)    │
│  BLE receive  → parse JSON           │
│  SD card      → load map tile (JPEG) │
│  JPEGDEC      → decode to RGB565     │
│  Arduino_GFX  → 172×320 IPS display  │
│  Touch AXS5106L → gesture handling   │
└──────────────────────────────────────┘
```

BLE packet example:
```json
{
  "lat": 41.9028, "lon": 12.4964,
  "speed": 23.5,  "heading": 275.0,
  "time": "14:32", "alt": 47.0,
  "bat": 0.87,
  "nav": 1, "ndist": 350, "nst": "Via Nazionale"
}
```
`nav` values: `-1` = off · `0` = straight · `1` = right · `2` = left · `3` = U-turn · `4` = arrived

---

## Repository structure

```
bikeesp32gps/
├── esp32-firmware/
│   └── bikegps_v3/
│       └── bikegps_v3.ino      ← main Arduino sketch (all-in-one)
├── iphone-app/
│   ├── project.yml             ← XcodeGen spec (run to regenerate .xcodeproj)
│   └── BikeGPS/
│       ├── BikeGPSApp.swift
│       ├── ContentView.swift
│       ├── Views/
│       │   └── MapNavView.swift
│       └── Managers/
│           ├── LocationManager.swift
│           ├── BLEManager.swift
│           ├── NavigationManager.swift
│           └── GPSTransmitter.swift
├── tools/
│   └── download_tiles.py       ← downloads OSM/Esri tiles to SD card
└── 3d-mounts/                  ← STEP files for handlebar mount
```

---

## Setup: ESP32 firmware

### 1. Install arduino-cli (or Arduino IDE 2.x)

```bash
brew install arduino-cli
arduino-cli core install esp32:esp32
```

### 2. Install libraries

```bash
arduino-cli lib install "Arduino_GFX_Library"
arduino-cli lib install "JPEGDEC"
arduino-cli lib install "ArduinoJson"
arduino-cli lib install "ESP32 BLE Arduino"
```

Also install **esp_lcd_touch_axs5106l** manually — download from [this repo](https://github.com/waveshareteam/ESP32-C6-Touch-LCD-1.47) and place it in `~/Documents/Arduino/libraries/`.

### 3. Compile and flash

```bash
# Compile
arduino-cli compile \
  --fqbn esp32:esp32:esp32c6:CDCOnBoot=cdc \
  esp32-firmware/bikegps_v3/

# Flash (adjust port as needed — /dev/cu.usbmodem* on macOS)
arduino-cli upload \
  -p /dev/cu.usbmodem1101 \
  --fqbn esp32:esp32:esp32c6:CDCOnBoot=cdc \
  esp32-firmware/bikegps_v3/
```

### 4. Prepare the SD card

The firmware expects tiles at `/tiles/{zoom}/{x}/{y}.jpg` (OSM slippy map convention).

```bash
# Download map tiles for your area (edit bounding box in the script first)
pip install requests pillow
python tools/download_tiles.py \
  --bbox 41.2,11.5,42.8,14.0 \  # lat_min, lon_min, lat_max, lon_max
  --zoom 14 15 \                 # zoom levels
  --layer map \                  # or: sat (Esri satellite)
  --out /Volumes/SDCARD/tiles
```

Zoom 14+15 for a region like Lazio (~100 MB). The 4 GB card has plenty of room for zoom 12–16 if needed.

---

## Setup: iPhone app

> 📖 **Guida dettagliata (con troubleshooting e info sui 7 giorni):** [docs/ios-sideloading.md](docs/ios-sideloading.md)

### 1. Install XcodeGen

```bash
brew install xcodegen
```

### 2. Generate and open the project

```bash
cd iphone-app
xcodegen generate
open BikeGPS.xcodeproj
```

### 3. Sign and install

1. In Xcode → BikeGPS target → **Signing & Capabilities** → set your Apple ID as Team.
2. Change `com.yourname.bikegps` to any unique bundle ID.
3. Plug in your iPhone, select it as destination, press ▶.
4. First run: **Settings → General → VPN & Device Management → [your Apple ID] → Trust**.

> Free developer accounts expire after 7 days — re-run from Xcode to refresh.

---

## Troubleshooting

| Symptom | Fix |
|---|---|
| Screen blank | Check USB-C connection; hold BOOT + press RESET to enter download mode |
| Display garbage on first boot | The `lcd_reg_init()` call in the sketch is mandatory for the JD9853 driver — make sure it's not commented out |
| BLE not connecting | Grant Bluetooth permission in iOS Settings; kill and reopen the app |
| No location data | Settings → Privacy → Location → BikeGPS → **Always** |
| Map shows grey tiles | SD card not mounted; check SPI.begin(1,3,2,4) precedes SD.begin(4) in setup() |
| JPEG decode fails | Files must be **baseline JPEG** (not progressive). Re-convert: `sips -s format jpeg *.jpg` |
| macOS copies `._` files to SD | Safe to delete. On macOS: `dot_clean /Volumes/SDCARD` |
| Nav doesn't reroute | Make sure iPhone has network access (MapKit needs it for rerouting) |

---

## Contributing (including vibe-coders 🤙)

This project was built with a mix of Arduino C++ and Swift, largely through AI-assisted coding sessions. **You don't need to be an expert** — if you use Claude Code, Cursor, or similar tools, here's how to get up to speed fast:

### Quick context for your AI assistant

Paste this into your first message:
```
I'm working on BikeGPS — an ESP32-C6 + iPhone BLE GPS display.
Hardware: Waveshare ESP32-C6-Touch-LCD-1.47 (TOUCH version).
Pins: LCD SPI SCK=1 MOSI=2 CS=14 DC=15 RST=22 BL=23
      SD: MISO=3 CS=4 (shares SCK+MOSI with LCD)
      Touch I2C: SDA=18 SCL=19 RST=20 INT=21
Display: 172×320 IPS, Arduino_GFX_Library + custom lcd_reg_init() required.
BLE: Nordic NUS profile, JSON packets, built-in ESP32 BLE library.
iPhone: SwiftUI, CoreLocation, MapKit routing, AVSpeechSynthesizer.
```

### Good first issues to tackle

- [ ] Nighttime map tiles (darker palette, less glare)
- [ ] Show elevation profile on the map
- [ ] Speed limit display (using OSM data)
- [ ] Android companion app
- [ ] Widget for iPhone Lock Screen showing current speed
- [ ] Battery-level optimisation (dim display when stationary)
- [ ] Multi-language navigation voice (currently Italian only)

### How to submit changes

1. Fork the repo
2. Make your change (AI-assisted is totally fine)
3. Test on real hardware if you can, or describe what you tested
4. Open a PR — describe what you changed and why

No formal code review process. If it works and doesn't break existing features, it goes in.

---

## License

MIT — free to use, modify, build, and sell.  
OpenStreetMap tiles © OpenStreetMap contributors (ODbL).  
Esri satellite tiles © Esri, Maxar, GeoEye (free for non-commercial use under Esri's standard terms).
