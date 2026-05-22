# BikeGPS

> 🇬🇧 [English version](README.en.md)

Un display GPS fai-da-te per ciclisti — un'app iPhone manda la tua posizione e la navigazione turn-by-turn a un piccolo schermo ESP32 montato sul manubrio via Bluetooth Low Energy.

![BikeGPS display showing map tile and navigation](icon.jpg)

> **⚠️ Prototipo v1** — questo è un progetto personale al suo primo rilascio pubblico. Funziona, ma ci sono sicuramente molte cose da migliorare: hardware, firmware, app, mount 3D. Ogni suggerimento, segnalazione di bug, pull request o idea è benvenuta. Il progetto è pensato per crescere con la comunità.

> **📱 Nota iOS** — per limiti imposti da Apple, l'app iPhone richiede di essere ricompilata da Xcode ogni 7 giorni (account gratuito). È scomodo ma non c'è alternativa senza pagare 99€/anno. [→ Leggi la guida completa](docs/ios-sideloading.md). Una **versione Android** è in lavorazione e non avrà questo problema.

---

## Cosa ti serve

Tre cose fisiche + un iPhone:

| # | Cosa | Come ottenerla |
|---|------|----------------|
| 1 | **Waveshare ESP32-C6-Touch-LCD-1.47** *(versione TOUCH!)* | [AliExpress](https://www.aliexpress.com/item/1005009816465254.html) ~$13–15 |
| 2 | **Mount da manubrio stampato in 3D** | Stampa il file STEP in `3d-mounts/` (PETG/ASA, ~2h) |
| 3 | **Scheda microSD ≥ 4 GB** (Class 10) con mappe offline | Qualsiasi — le tile si caricano con `tools/prepare_sd.py` |
| 4 | **iPhone** (iOS 16+) | — |

> ⚠️ **Compra la versione TOUCH** del modulo ESP32 — la versione non-touch ha pin GPIO diversi e il firmware non funzionerà.

Nessuna saldatura, nessun cablaggio aggiuntivo. Il modulo ESP32-C6 include display, touch, BLE e slot SD su una singola scheda.

---

## Cosa fa

| Funzione | Dettaglio |
|---|---|
| **Mappa live** | Tile OSM o satellite Esri pre-caricate su SD, nessuna connessione necessaria durante il percorso |
| **Velocità + bussola** | In tempo reale dal GPS dell'iPhone, mostrati in grandi cifre |
| **Navigazione turn-by-turn** | Cerca una destinazione sull'iPhone → l'ESP32 mostra freccia + distanza + guida vocale |
| **Ricalcolo percorso** | Ricalcola automaticamente se si prende la strada sbagliata |
| **Temperatura** | Sensore di temperatura del chip ESP32 mostrato a schermo |
| **Touch per zoom** | Tocca la metà superiore dello schermo → alterna tra z15 (dettaglio) e z14 (panoramica) |
| **Touch per cambiare vista** | Tocca la metà inferiore → cicla Velocità / Mappa+Info / Mappa Intera / Satellite / Notte |

---

## Alimentazione

Il modulo Waveshare ESP32-C6 si alimenta via **USB-C a 5V** (stessa porta usata per la programmazione).

### Soluzione attuale: porta USB dell'e-bike

Se hai una **bici elettrica con porta USB sul controller del manubrio** (come le Haibike con motore Yamaha), basta un cavo **USB micro → USB-C**: la bici alimenta il modulo quando è accesa e smette quando è spenta. Nessun componente aggiuntivo, nessuna batteria da ricaricare.

### Alternative per bici senza porta USB

| Soluzione | Adatta a | Note |
|---|---|---|
| **Powerbank** (5000 mAh, ~100g) | Qualsiasi bici | La più semplice — dura giorni, si ricarica via USB-C |
| **LiPo 500 mAh + TP4056** | Mount integrato | ~4h di autonomia (ESP32 consuma ~100–150 mA con display attivo) |
| **Buck converter 36/48V → 5V** | E-bike senza porta USB | Wired diretto alla batteria, sempre acceso con la bici |
| **Dinamo mozzo** | Bici non elettrica | Autonomo, ma serve raddrizzatore + regolatore 5V |
| **Pannello solare** | Uso estivo | Inaffidabile da solo, utile come supplemento a un LiPo |

> 💡 **Vuoi contribuire?** L'alimentazione è uno dei punti più aperti del progetto. Soluzioni interessanti da esplorare: mount con LiPo integrato e ricarica wireless, integrazione con il BUS CAN delle e-bike Yamaha/Bosch, supporto dinamo con supercapacitor per alimentare anche da fermo. Ogni proposta, schema o prototipo è benvenuto.

---

## Mount da manubrio stampato in 3D

La directory `3d-mounts/` contiene un file STEP per il mount da manubrio:

```
3d-mounts/
├── bikegps_mount_v1.step   ← primo prototipo (STEP AP214)
└── README.md               ← istruzioni stampa + hardware
```

> **⚠️ Prototipo v1** — funziona ma probabilmente ha margini di miglioramento. Modifiche, varianti e versioni alternative sono molto benvenute.

**Hardware necessario:**
- 4× viti **M2.5** (~6 mm) — fissaggio modulo LCD al tray
- 2× bulloni **M4** (~20 mm) + dado — C-clamp sul manubrio

**Assemblaggio giunto a sfera:** la sfera è un pezzo stampato separato. Inserisci prima il dado M4 nel giunto, poi incolla la sfera alla staffa del modulo Waveshare (epossidica o cianocrilato). Non incollare prima di aver messo il dado — non riusciresti più a chiudere il giunto.

Stampa in PETG o ASA (resistenza UV/intemperie), infill 40%.

---

## Architettura

```
┌──────────────────────────────────────┐
│          iPhone (app iOS)            │
│  CoreLocation → coordinate GPS       │
│  MapKit       → navigazione          │
│  AVFoundation → voce in italiano     │
│  Invia JSON via BLE NUS ogni ~1 s    │
└──────────────────┬───────────────────┘
                   │ Bluetooth LE (Nordic NUS)
                   │ {"lat":…,"lon":…,"speed":…,
                   │  "heading":…,"nav":1,"ndist":120}
┌──────────────────▼───────────────────┐
│     Waveshare ESP32-C6 (firmware)    │
│  BLE receive  → parse JSON           │
│  SD card      → carica tile (JPEG)   │
│  JPEGDEC      → decode in RGB565     │
│  Arduino_GFX  → display IPS 172×320  │
│  Touch AXS5106L → gestione tocchi    │
└──────────────────────────────────────┘
```

Esempio pacchetto BLE:
```json
{
  "lat": 41.9028, "lon": 12.4964,
  "speed": 23.5,  "heading": 275.0,
  "time": "14:32", "alt": 47.0,
  "bat": 0.87,
  "nav": 1, "ndist": 350, "nst": "Via Nazionale"
}
```
Valori `nav`: `-1` = off · `0` = dritto · `1` = destra · `2` = sinistra · `3` = inversione · `4` = arrivato

---

## Struttura del repository

```
bikeesp32gps/
├── esp32-firmware/
│   └── bikegps_v3/
│       └── bikegps_v3.ino      ← sketch Arduino principale (tutto in un file)
├── iphone-app/
│   ├── project.yml             ← spec XcodeGen (esegui per rigenerare .xcodeproj)
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
│   ├── prepare_sd.py           ← wizard interattivo: città, zoom, layer → tile sulla SD
│   └── download_tiles.py       ← CLI avanzata: download diretto per bbox/regione/lat-lon
├── docs/
│   └── ios-sideloading.md      ← guida Xcode passo per passo + problema dei 7 giorni
└── 3d-mounts/                  ← file STEP per il mount da manubrio
```

---

## Setup: firmware ESP32

### 1. Installa arduino-cli (o Arduino IDE 2.x)

```bash
brew install arduino-cli
arduino-cli core install esp32:esp32
```

### 2. Installa le librerie

```bash
arduino-cli lib install "Arduino_GFX_Library"
arduino-cli lib install "JPEGDEC"
arduino-cli lib install "ArduinoJson"
arduino-cli lib install "ESP32 BLE Arduino"
```

Installa anche **esp_lcd_touch_axs5106l** manualmente — scaricala da [questo repo](https://github.com/waveshareteam/ESP32-C6-Touch-LCD-1.47) e mettila in `~/Documents/Arduino/libraries/`.

### 3. Compila e flash

```bash
# Compila
arduino-cli compile \
  --fqbn esp32:esp32:esp32c6:CDCOnBoot=cdc \
  esp32-firmware/bikegps_v3/

# Flash (aggiusta la porta se necessario — /dev/cu.usbmodem* su macOS)
arduino-cli upload \
  -p /dev/cu.usbmodem1101 \
  --fqbn esp32:esp32:esp32c6:CDCOnBoot=cdc \
  esp32-firmware/bikegps_v3/
```

---

## Setup: mappe sulla SD card

Il firmware carica le tile dalla SD al percorso `/tiles/{zoom}/{x}/{y}.jpg` (formato OSM slippy map). Le tile si pre-caricano prima dell'uscita — nessuna connessione internet necessaria durante il percorso.

### Wizard interattivo (consigliato)

`tools/prepare_sd.py` è un wizard a quattro passi che ti guida nella scelta dell'area e nel download:

```bash
pip3 install Pillow             # una volta sola (serve per la conversione JPEG)
python3 tools/prepare_sd.py
```

**Passo 1 — Dove vuoi andare in bici?**
Scegli tra le regioni predefinite (Lazio, Roma, Viterbo…) oppure digita il nome di qualsiasi città. Il wizard la geocodifica in automatico e, se trova più risultati, ti chiede quale intendevi.

```
Passo 1/4  Dove vuoi andare in bici?

  → [1] Cerca una città o un luogo...
    [2] Lazio
    [3] Roma
    ...
```

**Passo 2 — Quanta area vuoi coprire?**

| Profilo | Zoom | Raggio | Dimensione approssimativa |
|---------|------|--------|---------------------------|
| Solo città | 15 | ~6 km | 30–60 MB |
| Gita / uscita | 14 + 15 | ~15 km | 80–200 MB |
| Intera zona | 14 + 15 | tutta la bbox della regione | variabile |
| Ampia area | 13 + 14 | tutta la bbox della regione | leggero, meno dettaglio |

**Passo 3 — Tipo di mappa?**
- `Mappa stradale` — tile OpenStreetMap, ideale per la navigazione
- `Satellite` — immagini aeree Esri World Imagery, file più pesanti
- `Entrambe` — scarica entrambi i set

**Passo 4 — Dove si trova la SD?**
Il wizard rileva automaticamente i volumi rimovibili su macOS (`/Volumes/*`) e mostra lo spazio libero. Scegli dalla lista o inserisci il percorso manualmente.

Dopo i quattro passi viene mostrato un riepilogo — numero di tile, MB stimati e tempo di download stimato — prima di chiedere conferma:

```
════════════════════════════════════════════
  RIEPILOGO
════════════════════════════════════════════
  Zona          Firenze, Toscana, Italia
  Zoom          14, 15
  Tipo mappa    map
  Tile totali   1.549
  Dimensione    ~45 MB
  Tempo stimato ~6 minuti
  Destinazione  /Volumes/SDCARD/tiles
════════════════════════════════════════════

  Vuoi iniziare il download? [S/n] ›
```

Il download è riprendibile — se lo interrompi e lo riavvii, le tile già presenti vengono saltate.

### CLI avanzata (uso da script o CI)

Se preferisci i flag alla modalità interattiva, `download_tiles.py` accetta tutto dalla riga di comando:

```bash
# Regione predefinita
python3 tools/download_tiles.py \
  --region lazio --zoom 14,15 --layer map \
  --out /Volumes/SDCARD/tiles

# Area personalizzata attorno a un punto
python3 tools/download_tiles.py \
  --lat 43.7696 --lon 11.2558 --radius 15 \
  --zoom 14,15 --layer sat \
  --out /Volumes/SDCARD/tiles

# Dry-run: conta le tile senza scaricare
python3 tools/download_tiles.py \
  --region lazio --zoom 14,15 --dry-run
```

Entrambi gli strumenti salvano le tile nel percorso atteso dal firmware:
- Mappa stradale → `/tiles/{z}/{x}/{y}.jpg`
- Satellite → `/tiles/sat/{z}/{x}/{y}.jpg`

> **Nota:** le tile devono essere **JPEG baseline**, non progressive — JPEGDEC sull'ESP32 non supporta il JPEG progressivo. Entrambi gli strumenti gestiscono la conversione in automatico quando Pillow è installato. Se aggiungi tile manualmente, convertile prima: `sips -s format jpeg *.jpg --out .`

---

## Setup: app iPhone

> 📖 **Guida dettagliata (con troubleshooting e info sui 7 giorni):** [docs/ios-sideloading.md](docs/ios-sideloading.md)

### 1. Installa XcodeGen

```bash
brew install xcodegen
```

### 2. Genera e apri il progetto

```bash
cd iphone-app
xcodegen generate
open BikeGPS.xcodeproj
```

### 3. Firma e installa

1. In Xcode → target BikeGPS → **Signing & Capabilities** → imposta il tuo Apple ID come Team.
2. Cambia `com.yourname.bikegps` con un bundle ID unico.
3. Collega l'iPhone, selezionalo come destinazione, premi ▶.
4. Al primo avvio: **Impostazioni → Generali → VPN e gestione dispositivo → [il tuo Apple ID] → Autorizza**.

> Gli account sviluppatore gratuiti scadono dopo 7 giorni — ri-esegui da Xcode per rinnovare.

---

## Risoluzione dei problemi

| Sintomo | Soluzione |
|---|---|
| Schermo spento | Controlla il connettore USB-C; tieni premuto BOOT + premi RESET per entrare in modalità download |
| Display con artefatti al primo avvio | La chiamata `lcd_reg_init()` nello sketch è obbligatoria per il driver JD9853 — assicurati che non sia commentata |
| BLE non si connette | Concedi il permesso Bluetooth nelle Impostazioni iOS; chiudi e riapri l'app |
| Nessun dato di posizione | Impostazioni → Privacy → Posizione → BikeGPS → **Sempre** |
| Mappa mostra tile grigie | SD non montata; verifica che SPI.begin(1,3,2,4) preceda SD.begin(4) nel setup() |
| Decodifica JPEG fallisce | I file devono essere **JPEG baseline** (non progressivo). Riconverti: `sips -s format jpeg *.jpg` |
| macOS copia file `._` sulla SD | Si possono ignorare o cancellare. Su macOS: `dot_clean /Volumes/SDCARD` |
| La navigazione non ricalcola | Assicurati che l'iPhone abbia accesso a internet (MapKit ne ha bisogno per il ricalcolo) |

---

## Contribuire (anche facendo vibecoding 🤙)

Questo progetto è stato costruito con un mix di Arduino C++ e Swift, in gran parte attraverso sessioni di coding assistito da AI. **Non devi essere un esperto** — se usi Claude Code, Cursor o strumenti simili, ecco come entrare subito nel contesto:

### Contesto rapido per il tuo assistente AI

Incolla questo nel tuo primo messaggio:
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

### Idee per chi vuole contribuire

- [ ] Tile notturne (palette più scura, meno abbagliamento)
- [ ] Profilo altimetrico sulla mappa
- [ ] Visualizzazione limiti di velocità (dati OSM)
- [ ] App companion per Android
- [ ] Widget per la Lock Screen dell'iPhone con la velocità attuale
- [ ] Ottimizzazione batteria (schermo più scuro quando si è fermi)
- [ ] Voce di navigazione multilingua (attualmente solo italiano)
- [ ] Soluzioni di alimentazione per bici non elettriche (LiPo integrato nel mount, dinamo mozzo, buck converter da batteria e-bike)

### Come inviare modifiche

1. Fai un fork del repo
2. Apporta la tua modifica (il coding assistito da AI è benvenuto)
3. Testa su hardware reale se puoi, altrimenti descrivi cosa hai verificato
4. Apri una PR — descrivi cosa hai cambiato e perché

Nessun processo formale di code review. Se funziona e non rompe le funzionalità esistenti, viene accettato.

---

## Licenza

MIT — libero di usare, modificare, costruire e vendere.  
Tile OpenStreetMap © OpenStreetMap contributors (ODbL).  
Tile satellite Esri © Esri, Maxar, GeoEye (libero per uso non commerciale secondo i termini standard Esri).
