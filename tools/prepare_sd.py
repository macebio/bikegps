#!/usr/bin/env python3
"""
BikeGPS – SD Card Prep Wizard
Scarica le mappe offline per il modulino ESP32.

Uso interattivo (raccomandato):
    python3 prepare_sd.py

Uso da riga di comando (avanzato):
    python3 prepare_sd.py --city "Roma" --coverage trip --layer map --out /Volumes/SDCARD/tiles
    python3 prepare_sd.py --region lazio --coverage region --layer both --yes
    python3 prepare_sd.py --lat 41.9028 --lon 12.4964 --radius 12 --zoom 14,15 --layer sat --info
"""

import os, sys, time, math, argparse, urllib.request, json, glob, textwrap
from io import BytesIO

# ── Pillow (opzionale) ────────────────────────────────────────────────────────
try:
    from PIL import Image
    HAS_PIL = True
except ImportError:
    HAS_PIL = False

# ── Colori ANSI ───────────────────────────────────────────────────────────────
def _c(code, text):
    return f"\033[{code}m{text}\033[0m" if sys.stdout.isatty() else text

def cyan(t):   return _c("36",     t)
def bold(t):   return _c("1",      t)
def green(t):  return _c("32",     t)
def yellow(t): return _c("33",     t)
def red(t):    return _c("31",     t)
def dim(t):    return _c("2",      t)

# ── Tile math ─────────────────────────────────────────────────────────────────
def lat_lon_to_tile(lat, lon, zoom):
    lat_r = math.radians(lat)
    n = 2 ** zoom
    x = int((lon + 180.0) / 360.0 * n)
    y = int((1.0 - math.log(math.tan(lat_r) + 1.0 / math.cos(lat_r)) / math.pi) / 2.0 * n)
    return x, y

def tiles_in_bbox(lat_min, lat_max, lon_min, lon_max, zoom):
    x0, y1 = lat_lon_to_tile(lat_min, lon_min, zoom)
    x1, y0 = lat_lon_to_tile(lat_max, lon_max, zoom)
    return [(x, y) for x in range(x0, x1 + 1) for y in range(y0, y1 + 1)]

def tiles_around(lat, lon, zoom, radius_km):
    dlat = radius_km / 111.0
    dlon = radius_km / (111.0 * math.cos(math.radians(lat)))
    return tiles_in_bbox(lat - dlat, lat + dlat, lon - dlon, lon + dlon, zoom)

# ── Regioni predefinite ───────────────────────────────────────────────────────
REGIONS = {
    "lazio":      (41.2, 42.8, 11.5, 14.1),
    "roma":       (41.6, 42.1, 12.2, 12.8),
    "viterbo":    (42.2, 42.6, 11.8, 12.4),
    "frosinone":  (41.4, 41.9, 13.2, 14.0),
    "latina":     (41.2, 41.7, 12.6, 13.3),
}

# ── Profili copertura ─────────────────────────────────────────────────────────
COVERAGES = {
    # key: (zoom_levels, radius_km_for_point, label, note)
    "city":   ([15],      6,    "Solo città",    "zoom 15 · raggio ~6 km · ~30–60 MB"),
    "trip":   ([14, 15],  15,   "Gita / uscita", "zoom 14+15 · raggio ~15 km · ~80–200 MB"),
    "region": ([14, 15],  None, "Intera zona",   "zoom 14+15 · tutta la regione selezionata"),
    "wide":   ([13, 14],  None, "Ampia area",    "zoom 13+14 · dettaglio ridotto, file leggeri"),
}

# ── Geocoding (Nominatim) ─────────────────────────────────────────────────────
def geocode(query, limit=5):
    url = (
        "https://nominatim.openstreetmap.org/search"
        f"?q={urllib.request.quote(query)}&format=json&limit={limit}&addressdetails=0"
    )
    req = urllib.request.Request(url, headers={
        "User-Agent": "BikeGPS-PrepSD/1.0 (github.com/macebio/bikegps)"
    })
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read())

# ── Rilevamento schede SD ─────────────────────────────────────────────────────
def find_sd_cards():
    candidates = []
    if sys.platform == "darwin":
        for vol in sorted(glob.glob("/Volumes/*")):
            if "Macintosh HD" in vol:
                continue
            if os.path.ismount(vol):
                candidates.append(vol)
    elif sys.platform.startswith("linux"):
        for mnt in glob.glob("/media/*/*") + glob.glob("/mnt/*"):
            if os.path.ismount(mnt):
                candidates.append(mnt)
    return candidates

def free_mb(path):
    try:
        st = os.statvfs(path)
        return st.f_bavail * st.f_frsize // (1024 * 1024)
    except Exception:
        return None

# ── Download tile ─────────────────────────────────────────────────────────────
MAP_SERVERS = [
    "https://tile.openstreetmap.org/{z}/{x}/{y}.png",
    "https://a.tile.openstreetmap.org/{z}/{x}/{y}.png",
    "https://b.tile.openstreetmap.org/{z}/{x}/{y}.png",
]
SAT_URL = ("https://server.arcgisonline.com/ArcGIS/rest/services/"
           "World_Imagery/MapServer/tile/{z}/{y}/{x}")
_srv = 0

def download_raw(z, x, y, layer="map", retries=3):
    global _srv
    url = SAT_URL.format(z=z, x=x, y=y) if layer == "sat" else (
        MAP_SERVERS[_srv % len(MAP_SERVERS)].format(z=z, x=x, y=y)
    )
    if layer == "map":
        _srv += 1
    req = urllib.request.Request(url, headers={"User-Agent": "BikeGPS-PrepSD/1.0"})
    for attempt in range(retries):
        try:
            with urllib.request.urlopen(req, timeout=15) as r:
                return r.read()
        except Exception:
            if attempt == retries - 1:
                raise
            time.sleep(1 + attempt)

def to_baseline_jpeg(data, quality=75):
    img = Image.open(BytesIO(data)).convert("RGB")
    buf = BytesIO()
    img.save(buf, format="JPEG", quality=quality, optimize=False,
             progressive=False, subsampling=0)
    return buf.getvalue()

def save_tile(z, x, y, out_dir, layer="map", quality=75, skip_existing=True):
    if layer == "sat":
        path = os.path.join(out_dir, "sat", str(z), str(x), f"{y}.jpg")
    else:
        path = os.path.join(out_dir, str(z), str(x), f"{y}.jpg")
    alt_png = path.replace(".jpg", ".png")
    if skip_existing and (os.path.exists(path) or os.path.exists(alt_png)):
        return "skip"
    os.makedirs(os.path.dirname(path), exist_ok=True)
    raw = download_raw(z, x, y, layer)
    if HAS_PIL:
        with open(path, "wb") as f:
            f.write(to_baseline_jpeg(raw, quality))
    else:
        ext = ".jpg" if layer == "sat" else ".png"
        with open(path if ext == ".jpg" else alt_png, "wb") as f:
            f.write(raw)
    return "ok"

# ── UI helpers ────────────────────────────────────────────────────────────────
def hr(char="─", width=52):
    print(dim(char * width))

def step_header(n, total, title):
    print()
    hr()
    print(f"  {cyan(bold(f'Passo {n}/{total}'))}  {bold(title)}")
    hr()

def ask(prompt_text, default=None, validator=None):
    """Chiede una risposta all'utente. Restituisce la stringa."""
    default_hint = f"  {dim(f'[invio = {default}]')}" if default is not None else ""
    while True:
        try:
            raw = input(f"\n  {prompt_text}{default_hint}\n  › ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            sys.exit(0)
        val = raw if raw else (str(default) if default is not None else "")
        if val == "" and default is None:
            print(f"  {yellow('⚠')}  Risposta obbligatoria.")
            continue
        if validator:
            err = validator(val)
            if err:
                print(f"  {yellow('⚠')}  {err}")
                continue
        return val

def ask_choice(prompt_text, choices, default_idx=0):
    """
    choices: lista di (key, label, note_opzionale)
    Restituisce la key scelta.
    """
    print(f"\n  {bold(prompt_text)}")
    print()
    for i, item in enumerate(choices):
        key, label = item[0], item[1]
        note = item[2] if len(item) > 2 else ""
        marker = cyan("→") if i == default_idx else " "
        num = cyan(str(i + 1))
        note_str = f"  {dim(note)}" if note else ""
        print(f"  {marker} [{num}] {label}{note_str}")
    print()
    valid = [str(i + 1) for i in range(len(choices))]
    default_num = str(default_idx + 1)
    while True:
        try:
            raw = input(f"  Scegli [{dim(default_num)}]: ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            sys.exit(0)
        val = raw if raw else default_num
        if val in valid:
            return choices[int(val) - 1][0]
        print(f"  {yellow('⚠')}  Inserisci un numero tra 1 e {len(choices)}.")

def progress_bar(done, total, width=36):
    pct = done / total if total else 0
    filled = int(width * pct)
    bar = green("█") * filled + dim("░") * (width - filled)
    return f"[{bar}] {done}/{total} {dim(f'{pct*100:.0f}%')}"

# ── Wizard ────────────────────────────────────────────────────────────────────

def run_wizard(args):
    print()
    print(bold(cyan("  ╔═══════════════════════════════════════════╗")))
    print(bold(cyan("  ║     BikeGPS — Preparazione SD card        ║")))
    print(bold(cyan("  ╚═══════════════════════════════════════════╝")))
    if not HAS_PIL:
        print()
        print(f"  {yellow('⚠')}  Pillow non installato — le tile OSM vengono salvate come PNG.")
        print(f"     Per convertirle in JPEG: {cyan('pip3 install Pillow')}")

    # ═══ PASSO 1: Dove? ══════════════════════════════════════════════════════
    step_header(1, 4, "Dove vuoi andare in bici?")

    bbox   = None   # (lat_min, lat_max, lon_min, lon_max)
    center = None   # (lat, lon)
    area_label = ""

    if args.region:
        bbox = REGIONS[args.region]
        area_label = args.region
    elif args.lat and args.lon:
        center = (args.lat, args.lon)
        area_label = f"{args.lat:.4f}, {args.lon:.4f}"
    else:
        # Regioni predefinite come scorciatoie
        quick = [(k, k.capitalize(), "") for k in REGIONS]
        quick.insert(0, ("_search", "Cerca una città o un luogo...", ""))

        # Se --city è passato, salta il menu
        if args.city:
            query = args.city
        else:
            region_choice = ask_choice(
                "Scegli una zona o cerca un luogo specifico:",
                quick,
                default_idx=0
            )
            if region_choice != "_search":
                bbox = REGIONS[region_choice]
                area_label = region_choice
            else:
                query = ask("Inserisci il nome della città o del luogo",
                            default="Roma")

        if not bbox:  # tocca geocodificare
            print(f"\n  {dim('Ricerca in corso...')} ", end="", flush=True)
            try:
                results = geocode(query)
                print()
                if not results:
                    print(f"  {red('✗')} Nessun risultato. Prova con un nome diverso.")
                    sys.exit(1)

                # Deduplica per nome visualizzato
                seen = {}
                for r in results:
                    name = r.get("display_name", "?")
                    if name not in seen:
                        seen[name] = r
                unique = list(seen.values())

                # Auto-seleziona se uno solo, oppure --yes/--info
                if len(unique) == 1 or args.yes or args.info:
                    r = unique[0]
                    area_label = r.get("display_name", "?")
                    center = (float(r["lat"]), float(r["lon"]))
                    print(f"  {green('✓')} Trovato: {bold(area_label[:70])}")
                else:
                    choices = [(str(i), r.get("display_name", "?")[:75], "")
                               for i, r in enumerate(unique[:5])]
                    key = ask_choice("Quale intendevi?", choices, default_idx=0)
                    r = unique[int(key)]
                    area_label = r.get("display_name", "?")
                    center = (float(r["lat"]), float(r["lon"]))
            except Exception as e:
                print(f"\n  {yellow('⚠')}  Geocoding non disponibile ({e}).")
                print("  Inserisci le coordinate manualmente.")
                lat_s = ask("Latitudine (es. 41.9028)", default="41.9028")
                lon_s = ask("Longitudine (es. 12.4964)", default="12.4964")
                center = (float(lat_s), float(lon_s))
                area_label = f"{center[0]:.4f}, {center[1]:.4f}"

    # ═══ PASSO 2: Quanta copertura? ══════════════════════════════════════════
    step_header(2, 4, "Quanta area vuoi coprire?")

    if args.zoom:
        zooms = [int(z.strip()) for z in args.zoom.split(",")]
        radius_km = args.radius
        coverage_key = "custom"
    else:
        coverage_arg = args.coverage if args.coverage else None

        # Se è una regione bbox, rimuovi "city" (non ha senso senza centro)
        cov_keys = list(COVERAGES.keys()) if center else [
            k for k in COVERAGES if COVERAGES[k][1] is None
        ]
        if center:
            default_cov_idx = 1  # "trip" default quando c'è un punto
        else:
            default_cov_idx = 0  # "region" default quando c'è una bbox

        if coverage_arg:
            coverage_key = coverage_arg
        else:
            cov_choices = [(k, COVERAGES[k][2], COVERAGES[k][3]) for k in cov_keys]
            coverage_key = ask_choice(
                "Scegli la copertura:",
                cov_choices,
                default_idx=default_cov_idx if default_cov_idx < len(cov_choices) else 0
            )

        zooms     = COVERAGES[coverage_key][0]
        radius_km = COVERAGES[coverage_key][1] if COVERAGES[coverage_key][1] else args.radius

    # ═══ PASSO 3: Tipo di mappa? ══════════════════════════════════════════════
    step_header(3, 4, "Che tipo di mappa vuoi?")

    if args.layer:
        layer_choice = args.layer
    else:
        layer_choice = ask_choice(
            "Scegli il tipo di mappa:",
            [
                ("map",  "Mappa stradale (OSM)",      "ideale per la navigazione, file leggeri"),
                ("sat",  "Satellite (Esri)",           "visuale aerea, file più pesanti"),
                ("both", "Entrambe",                   "scarica sia la mappa che il satellite"),
            ],
            default_idx=0
        )

    layers = ["map", "sat"] if layer_choice == "both" else [layer_choice]

    # ═══ PASSO 4: Dove si trova la SD? ════════════════════════════════════════
    step_header(4, 4, "Dove si trova la scheda SD?")

    if args.out:
        out_dir = args.out
        print(f"\n  {green('✓')} Output: {bold(out_dir)}")
    else:
        cards = find_sd_cards()
        if cards:
            print(f"\n  Volumi rimovibili rilevati:")
            choices = []
            for vol in cards:
                mb = free_mb(vol)
                size_str = f"{mb:,} MB liberi" if mb is not None else "?"
                choices.append((vol, os.path.basename(vol), size_str))
            choices.append(("_manual", "Inserisci percorso manualmente", ""))
            chosen_vol = ask_choice("Scegli la scheda SD:", choices, default_idx=0)
            if chosen_vol == "_manual":
                chosen_vol = ask("Percorso della directory tiles", default="/Volumes/SDCARD")
            out_dir = os.path.join(chosen_vol, "tiles")
            print(f"\n  {green('✓')} Le tile verranno salvate in: {bold(out_dir)}")
        else:
            print(f"\n  {yellow('⚠')}  Nessun volume rimovibile trovato.")
            chosen_vol = ask("Percorso della directory tiles", default="/Volumes/SDCARD")
            out_dir = os.path.join(chosen_vol, "tiles")

    # ═══ Calcolo tile ═════════════════════════════════════════════════════════
    all_tiles = []  # (z, x, y, layer)
    for layer in layers:
        for z in zooms:
            if bbox:
                pts = tiles_in_bbox(*bbox, z)
            else:
                r = radius_km if radius_km else 10
                pts = tiles_around(center[0], center[1], z, r)
            all_tiles += [(z, x, y, layer) for x, y in pts]

    total = len(all_tiles)
    est_mb = sum(40 if lyr == "sat" else 30 for _, _, _, lyr in all_tiles) / 1024
    # Stima tempo: ~2 tile/s mappa, ~10 tile/s satellite (media)
    avg_tps  = 4.0
    est_mins = total / avg_tps / 60

    # ═══ Riepilogo ════════════════════════════════════════════════════════════
    print()
    hr("═")
    print(f"  {bold('RIEPILOGO')}")
    hr("═")
    print(f"  Zona          {bold(area_label[:60])}")
    print(f"  Zoom          {bold(', '.join(str(z) for z in zooms))}")
    print(f"  Tipo mappa    {bold(layer_choice)}")
    print(f"  Tile totali   {bold(f'{total:,}')}")
    print(f"  Dimensione    ~{bold(f'{est_mb:.0f} MB')}")
    print(f"  Tempo stimato ~{bold(f'{est_mins:.0f} minuti')}")
    print(f"  Destinazione  {bold(out_dir)}")
    if not HAS_PIL:
        print(f"\n  {yellow('⚠')}  Pillow assente — tile OSM salvate come PNG (più grandi).")
    hr("═")

    if args.info:
        print(f"\n  {dim('[--info: nessun download]')}")
        return

    if total == 0:
        print(f"\n  {yellow('⚠')}  Nessuna tile da scaricare. Controlla l'area e lo zoom.")
        return

    if not args.yes:
        if total > 10000:
            print(f"\n  {yellow('⚠')}  {total:,} tile sono tante. Il download potrebbe durare a lungo.")
        try:
            ans = input(f"\n  Vuoi iniziare il download? {dim('[S/n]')} › ").strip().lower()
        except (EOFError, KeyboardInterrupt):
            print()
            sys.exit(0)
        if ans in ("n", "no"):
            print("  Annullato.")
            return

    # ═══ Download ═════════════════════════════════════════════════════════════
    print()
    hr()
    print(f"  {bold('DOWNLOAD')}")
    hr()
    ok = skip = err = 0
    start = time.time()
    delay_map = 0.25
    delay_sat = 0.05

    for i, (z, x, y, layer) in enumerate(all_tiles):
        try:
            result = save_tile(z, x, y, out_dir, layer, args.quality)
            if result == "skip":
                skip += 1
            else:
                ok += 1
                time.sleep(delay_sat if layer == "sat" else delay_map)
        except Exception as e:
            err += 1
            if err <= 5:
                print(f"\n  {red('✗')} z={z} x={x} y={y}: {e}")

        if (i + 1) % 20 == 0 or i == total - 1:
            elapsed = time.time() - start
            rate    = (ok + skip) / elapsed if elapsed > 0 else 0
            eta_s   = int((total - i - 1) / rate) if rate > 0 else 0
            eta_str = f"{eta_s//60}m{eta_s%60:02d}s" if eta_s >= 60 else f"{eta_s}s"
            bar     = progress_bar(i + 1, total)
            print(f"  {bar}  {dim(f'{rate:.1f} t/s  ETA {eta_str}')}   ", end="\r")

    elapsed = time.time() - start
    print(f"\n\n  {green('✓')} Fatto in {elapsed/60:.1f} min  "
          f"(scaricati={ok}, saltati={skip}, errori={err})")

    try:
        used = sum(
            os.path.getsize(os.path.join(root, f))
            for root, _, files in os.walk(out_dir)
            for f in files
            if f.endswith((".jpg", ".png"))
        )
        print(f"  Spazio usato dalle tile: {used/1024/1024:.1f} MB")
    except Exception:
        pass

    print()
    print(f"  {bold('Metti la SD nel modulino ESP32 e pedala! 🚴')}")
    print()

# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    p = argparse.ArgumentParser(
        description="BikeGPS SD Card Prep",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__
    )
    p.add_argument("--city",     help="Nome della città da geocodificare")
    p.add_argument("--region",   choices=list(REGIONS.keys()))
    p.add_argument("--lat",      type=float)
    p.add_argument("--lon",      type=float)
    p.add_argument("--radius",   type=float, default=10)
    p.add_argument("--zoom",     help="Es. '14,15'")
    p.add_argument("--coverage", choices=list(COVERAGES.keys()),
                   help="city | trip | region | wide")
    p.add_argument("--layer",    choices=["map", "sat", "both"])
    p.add_argument("--out",      help="Directory di output")
    p.add_argument("--yes", "-y", action="store_true", help="Salta conferme")
    p.add_argument("--info",     action="store_true",  help="Solo conteggio, nessun download")
    p.add_argument("--quality",  type=int, default=75)
    args = p.parse_args()
    run_wizard(args)

if __name__ == "__main__":
    main()
