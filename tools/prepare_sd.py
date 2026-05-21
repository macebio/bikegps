#!/usr/bin/env python3
"""
BikeGPS SD Card Wizard — interactive tool to load offline map tiles.

Usage (interactive):
    python3 prepare_sd.py

Usage (scripted / CI):
    python3 prepare_sd.py --city "Roma" --profile city --layer map --out /Volumes/SDCARD/tiles
    python3 prepare_sd.py --region lazio --profile region --layer both --yes
    python3 prepare_sd.py --lat 41.9028 --lon 12.4964 --radius 12 --zoom 14,15 --layer sat

Flags:
    --city TEXT        Geocode a city name (Nominatim)
    --region NAME      Predefined region: lazio, rome, viterbo, frosinone, latina
    --lat / --lon      Manual center (requires --radius)
    --radius KM        Radius in km around center (default 10)
    --zoom 14,15       Comma-separated zoom levels (overrides --profile)
    --profile NAME     city | trip | region | custom
    --layer map|sat|both
    --out PATH         SD card tiles directory (default: auto-detect)
    --yes / -y         Skip confirmation
    --info             Show tile count + size only, no download
    --quality N        JPEG quality 1-95 (default 75)
"""

import os, sys, time, math, argparse, urllib.request, json, glob
from io import BytesIO

# ── Optional Pillow ───────────────────────────────────────────────────────────

try:
    from PIL import Image
    HAS_PIL = True
except ImportError:
    HAS_PIL = False

# ── OSM tile math ─────────────────────────────────────────────────────────────

def lat_lon_to_tile(lat, lon, zoom):
    lat_r = math.radians(lat)
    n = 2 ** zoom
    x = int((lon + 180.0) / 360.0 * n)
    y = int((1.0 - math.log(math.tan(lat_r) + 1.0 / math.cos(lat_r)) / math.pi) / 2.0 * n)
    return x, y

def tile_to_lat_lon(x, y, zoom):
    n = 2 ** zoom
    lon = x / n * 360.0 - 180.0
    lat_r = math.atan(math.sinh(math.pi * (1 - 2 * y / n)))
    return math.degrees(lat_r), lon

def tiles_in_bbox(lat_min, lat_max, lon_min, lon_max, zoom):
    x_min, y_max = lat_lon_to_tile(lat_min, lon_min, zoom)
    x_max, y_min = lat_lon_to_tile(lat_max, lon_max, zoom)
    return [(x, y) for x in range(x_min, x_max + 1) for y in range(y_min, y_max + 1)]

def tiles_around(lat, lon, zoom, radius_km):
    dlat = radius_km / 111.0
    dlon = radius_km / (111.0 * math.cos(math.radians(lat)))
    return tiles_in_bbox(lat - dlat, lat + dlat, lon - dlon, lon + dlon, zoom)

# ── Predefined regions ────────────────────────────────────────────────────────

REGIONS = {
    "lazio":     (41.2, 42.8, 11.5, 14.1),
    "rome":      (41.6, 42.1, 12.2, 12.8),
    "viterbo":   (42.2, 42.6, 11.8, 12.4),
    "frosinone": (41.4, 41.9, 13.2, 14.0),
    "latina":    (41.2, 41.7, 12.6, 13.3),
}

# ── Zoom profiles ─────────────────────────────────────────────────────────────

PROFILES = {
    # name: (zoom_levels, radius_km_for_point_area, description)
    "city":   ([15],      6,  "City ride   — z15, ~6 km radius, ~30–60 MB"),
    "trip":   ([14, 15],  15, "Day trip    — z14+15, ~15 km radius, ~80–150 MB"),
    "region": ([14, 15],  None, "Full region — z14+15, entire region bbox"),
    "wide":   ([13, 14],  None, "Wide area   — z13+14, entire region bbox (lighter, less detail)"),
}

# ── Geocoding via Nominatim ───────────────────────────────────────────────────

def geocode(query):
    """Return list of (name, lat, lon) matches for a place query."""
    url = (
        "https://nominatim.openstreetmap.org/search"
        f"?q={urllib.request.quote(query)}&format=json&limit=5&addressdetails=0"
    )
    req = urllib.request.Request(url, headers={
        "User-Agent": "BikeGPS-PrepSD/1.0 (personal use; github.com/macebio/bikegps)"
    })
    with urllib.request.urlopen(req, timeout=10) as resp:
        results = json.loads(resp.read())
    return [(r.get("display_name", "?"), float(r["lat"]), float(r["lon"])) for r in results]

# ── SD card auto-detection ────────────────────────────────────────────────────

def find_sd_cards():
    """Return list of likely SD card mount points."""
    candidates = []
    if sys.platform == "darwin":
        for vol in glob.glob("/Volumes/*"):
            if vol == "/Volumes/Macintosh HD":
                continue
            if os.path.ismount(vol):
                candidates.append(vol)
    elif sys.platform.startswith("linux"):
        for mnt in glob.glob("/media/*/*") + glob.glob("/mnt/*"):
            if os.path.ismount(mnt):
                candidates.append(mnt)
    return candidates

# ── Download ─────────────────────────────────────────────────────────────────

MAP_SERVERS = [
    "https://tile.openstreetmap.org/{z}/{x}/{y}.png",
    "https://a.tile.openstreetmap.org/{z}/{x}/{y}.png",
    "https://b.tile.openstreetmap.org/{z}/{x}/{y}.png",
]
SAT_SERVER = (
    "https://server.arcgisonline.com/ArcGIS/rest/services/"
    "World_Imagery/MapServer/tile/{z}/{y}/{x}"
)
_srv = 0

def download_raw(z, x, y, layer="map", retries=3):
    global _srv
    if layer == "sat":
        url = SAT_SERVER.format(z=z, x=x, y=y)
    else:
        url = MAP_SERVERS[_srv % len(MAP_SERVERS)].format(z=z, x=x, y=y)
        _srv += 1
    req = urllib.request.Request(url, headers={
        "User-Agent": "BikeGPS-PrepSD/1.0 (personal use)"
    })
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
    """Download and save one tile. Returns 'skip', 'ok', or raises."""
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
        data = to_baseline_jpeg(raw, quality)
        with open(path, "wb") as f:
            f.write(data)
    else:
        # Esri → already JPEG; OSM → PNG saved as-is (no conversion)
        ext = ".jpg" if layer == "sat" else ".png"
        save_path = path if ext == ".jpg" else alt_png
        with open(save_path, "wb") as f:
            f.write(raw)
    return "ok"

# ── Progress bar ──────────────────────────────────────────────────────────────

def progress_bar(done, total, width=40):
    pct = done / total if total else 0
    filled = int(width * pct)
    bar = "█" * filled + "░" * (width - filled)
    return f"[{bar}] {done}/{total} ({pct*100:.0f}%)"

# ── Prompts ───────────────────────────────────────────────────────────────────

def prompt(msg, default=None):
    hint = f" [{default}]" if default is not None else ""
    try:
        val = input(f"{msg}{hint}: ").strip()
    except (EOFError, KeyboardInterrupt):
        print()
        sys.exit(0)
    return val if val else (str(default) if default is not None else "")

def prompt_choice(msg, options, default=None):
    """options: list of (key, label). Returns key."""
    print(f"\n{msg}")
    for k, label in options:
        marker = " ← default" if str(k) == str(default) else ""
        print(f"  [{k}] {label}{marker}")
    while True:
        val = prompt("Enter choice", default).lower()
        for k, _ in options:
            if val == str(k).lower():
                return str(k).lower()
        print(f"  Please enter one of: {', '.join(str(k) for k, _ in options)}")

def confirm(msg, default="y"):
    val = prompt(f"\n{msg} [y/n]", default).lower()
    return val in ("y", "yes")

# ── Wizard ────────────────────────────────────────────────────────────────────

def run_wizard(args):
    print()
    print("╔═══════════════════════════════════════════╗")
    print("║       BikeGPS — SD Card Prep Wizard       ║")
    print("╚═══════════════════════════════════════════╝")
    print()

    # ── 1. Output path ──────────────────────────────────────────────────────
    if args.out:
        out_base = args.out
        print(f"Output: {out_base}")
    else:
        cards = find_sd_cards()
        if cards:
            print("Detected removable volumes:")
            for i, c in enumerate(cards):
                free = ""
                try:
                    st = os.statvfs(c)
                    mb = st.f_bavail * st.f_frsize // (1024 * 1024)
                    free = f"  ({mb} MB free)"
                except Exception:
                    pass
                print(f"  [{i+1}] {c}{free}")
            print(f"  [0] Enter path manually")
            idx_str = prompt("Select SD card", "1")
            try:
                idx = int(idx_str) - 1
                if idx < 0:
                    raise ValueError
                out_base = os.path.join(cards[idx], "tiles")
            except (ValueError, IndexError):
                out_base = prompt("Path to tiles directory", "/Volumes/SDCARD/tiles")
        else:
            out_base = prompt("Path to tiles directory", "/Volumes/SDCARD/tiles")

    out_dir = out_base  # tiles go directly here

    # ── 2. Area ─────────────────────────────────────────────────────────────
    bbox = None      # (lat_min, lat_max, lon_min, lon_max)
    center = None    # (lat, lon)
    area_name = ""

    if args.region:
        bbox = REGIONS[args.region]
        area_name = args.region
    elif args.lat and args.lon:
        center = (args.lat, args.lon)
        area_name = f"{args.lat:.4f}, {args.lon:.4f}"
    elif args.city:
        city_query = args.city
    else:
        # Interactive area choice
        print()
        print("─── Area ────────────────────────────────────────")
        area_choice = prompt_choice(
            "How do you want to choose the area?",
            [("c", "Search by city/place name"),
             ("r", f"Predefined region ({', '.join(REGIONS.keys())})"),
             ("m", "Manual: enter lat/lon + radius")],
            default="c"
        )

        if area_choice == "r":
            region_key = prompt_choice(
                "Choose region:",
                [(k, k.capitalize()) for k in REGIONS],
                default="rome"
            )
            bbox = REGIONS[region_key]
            area_name = region_key
        elif area_choice == "m":
            center = (float(prompt("Latitude", "41.9028")),
                      float(prompt("Longitude", "12.4964")))
            area_name = f"{center[0]:.4f},{center[1]:.4f}"
        else:
            city_query = prompt("City / place name (e.g. 'Roma' or 'Lago di Garda')", "Roma")

    # Geocode if needed
    if 'city_query' in dir() and not center and not bbox:
        print(f"  Searching for '{city_query}'…", end="", flush=True)
        try:
            results = geocode(city_query)
            print()
            if not results:
                print("  No results. Try a different name or use lat/lon.")
                sys.exit(1)
            if len(results) == 1 or args.yes or args.info:
                area_name, clat, clon = results[0]
                print(f"  Found: {area_name[:70]}")
                center = (clat, clon)
            else:
                print("  Found multiple results:")
                for i, (name, _, _) in enumerate(results[:5]):
                    print(f"    [{i+1}] {name[:80]}")
                idx = int(prompt("  Select result", "1")) - 1
                area_name, clat, clon = results[idx]
                center = (clat, clon)
        except Exception as e:
            print(f"\n  Geocoding failed: {e}")
            print("  Falling back to manual entry.")
            center = (float(prompt("Latitude", "41.9028")),
                      float(prompt("Longitude", "12.4964")))
            area_name = f"{center[0]:.4f},{center[1]:.4f}"

    # ── 3. Zoom / profile ────────────────────────────────────────────────────
    if args.zoom:
        zooms = [int(z.strip()) for z in args.zoom.split(",")]
        radius_km = args.radius
        profile_name = "custom"
    else:
        profile_arg = args.profile
        if not profile_arg:
            print()
            print("─── Coverage profile ────────────────────────────")
            p_options = [(k, v[2]) for k, v in PROFILES.items()]
            if bbox:
                # Full region makes more sense for region/bbox mode
                p_options = [(k, v[2]) for k, v in PROFILES.items() if v[1] is None or k != "city"]
            profile_arg = prompt_choice("Choose profile:", p_options, default="trip")
        profile_name = profile_arg
        zooms = PROFILES[profile_name][0]
        radius_km = PROFILES[profile_name][1] if PROFILES[profile_name][1] else args.radius

    # ── 4. Layer ─────────────────────────────────────────────────────────────
    if args.layer:
        layer_choice = args.layer
    else:
        print()
        print("─── Map layer ───────────────────────────────────")
        layer_choice = prompt_choice(
            "Which tiles?",
            [("map", "Road map (OSM)  — lighter, ideal for navigation"),
             ("sat", "Satellite (Esri) — scenic, heavier"),
             ("both", "Both map + satellite")],
            default="map"
        )

    layers = ["map", "sat"] if layer_choice == "both" else [layer_choice]

    # ── 5. Build tile list ────────────────────────────────────────────────────
    all_tiles = []  # list of (z, x, y, layer)
    for layer in layers:
        for z in zooms:
            if bbox:
                pts = tiles_in_bbox(*bbox, z)
            else:
                r = radius_km if radius_km else 10
                pts = tiles_around(center[0], center[1], z, r)
            all_tiles += [(z, x, y, layer) for x, y in pts]

    total = len(all_tiles)
    # ~30 KB/tile map, ~40 KB/tile sat
    est_mb = sum(40 if lyr == "sat" else 30 for _, _, _, lyr in all_tiles) / 1024

    # ── 6. Summary ────────────────────────────────────────────────────────────
    print()
    print("─── Summary ─────────────────────────────────────")
    print(f"  Area    : {area_name[:70]}")
    print(f"  Zooms   : {zooms}")
    print(f"  Layers  : {layers}")
    print(f"  Tiles   : {total:,}")
    print(f"  ~Size   : {est_mb:.0f} MB")
    print(f"  Output  : {out_dir}")
    if not HAS_PIL:
        print()
        print("  ⚠️  Pillow not installed — OSM tiles saved as PNG (larger).")
        print("     Install with: pip3 install Pillow")

    if args.info:
        print()
        print("  [--info mode, no download]")
        return

    if total == 0:
        print("\n  No tiles to download. Check your area/zoom settings.")
        return

    if total > 8000 and not args.yes:
        print(f"\n  ⚠️  {total:,} tiles is quite a lot (~{est_mb:.0f} MB, "
              f"~{total*0.35/60:.0f} min to download).")

    if not args.yes:
        if not confirm("Start download?", default="y"):
            print("Aborted.")
            return

    # ── 7. Download ───────────────────────────────────────────────────────────
    print()
    print("─── Downloading ─────────────────────────────────")
    ok = skip = err = 0
    start = time.time()
    delay_map = 0.25   # be polite to OSM tile servers
    delay_sat = 0.05   # Esri is fine with faster requests

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
            # Only print errors; don't spam on retries
            if err <= 10:
                print(f"\n  ERR z={z} x={x} y={y} [{layer}]: {e}")

        # Refresh progress line every ~20 tiles or at the end
        if (i + 1) % 20 == 0 or i == total - 1:
            elapsed = time.time() - start
            rate = (ok + skip) / elapsed if elapsed > 0 else 0
            eta = int((total - i - 1) / rate) if rate > 0 else 0
            eta_str = f"{eta//60}m{eta%60:02d}s" if eta >= 60 else f"{eta}s"
            bar = progress_bar(i + 1, total)
            print(f"  {bar}  {rate:.1f} t/s  ETA {eta_str}  err={err}   ", end="\r")

    elapsed = time.time() - start
    print(f"\n\n  ✓ Done in {elapsed/60:.1f} min  "
          f"(downloaded={ok}, skipped={skip}, errors={err})")

    # Disk usage
    try:
        total_bytes = sum(
            os.path.getsize(os.path.join(root, f))
            for root, _, files in os.walk(out_dir)
            for f in files
            if f.endswith((".jpg", ".png"))
        )
        print(f"  Disk used by tiles: {total_bytes/1024/1024:.1f} MB")
    except Exception:
        pass

    print()
    print("  Tiles are ready. Insert the SD card into the ESP32-C6 module and start riding!")
    print()

# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="BikeGPS SD Card Prep Wizard",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__
    )
    parser.add_argument("--city",    help="Search for a city/place by name")
    parser.add_argument("--region",  choices=list(REGIONS.keys()), help="Predefined region")
    parser.add_argument("--lat",     type=float, help="Center latitude")
    parser.add_argument("--lon",     type=float, help="Center longitude")
    parser.add_argument("--radius",  type=float, default=10, help="Radius in km (default 10)")
    parser.add_argument("--zoom",    help="Zoom levels, e.g. '14,15'")
    parser.add_argument("--profile", choices=list(PROFILES.keys()),
                        help="Zoom profile: city | trip | region | wide")
    parser.add_argument("--layer",   choices=["map", "sat", "both"], help="Tile layer(s)")
    parser.add_argument("--out",     help="Output directory (default: auto-detect SD card)")
    parser.add_argument("--yes", "-y", action="store_true", help="Skip confirmation")
    parser.add_argument("--info",    action="store_true",   help="Count tiles only, no download")
    parser.add_argument("--quality", type=int, default=75,  help="JPEG quality 1-95 (default 75)")
    args = parser.parse_args()

    run_wizard(args)

if __name__ == "__main__":
    main()
