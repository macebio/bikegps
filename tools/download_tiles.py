#!/usr/bin/env python3
"""
Download OSM map or Esri satellite tiles for offline use on the BikeGPS SD card.

Usage:
  python3 download_tiles.py --zoom 15 --lat 41.9028 --lon 12.4964 --radius 10 --out /Volumes/MSD/tiles
  python3 download_tiles.py --zoom 15 --lat 41.9028 --lon 12.4964 --radius 10 --out /Volumes/MSD/tiles --layer sat
  python3 download_tiles.py --region lazio --zoom 14,15 --out /Volumes/MSD/tiles
  python3 download_tiles.py --region lazio --zoom 14,15 --out /Volumes/MSD/tiles --layer sat

Tile format saved:
  map layer: /out/{zoom}/{x}/{y}.jpg
  sat layer: /out/sat/{zoom}/{x}/{y}.jpg
"""

import os, sys, time, math, argparse, urllib.request
from io import BytesIO

try:
    from PIL import Image
    HAS_PIL = True
except ImportError:
    HAS_PIL = False
    print("NOTE: Pillow not installed — tiles saved as PNG (will still work but larger).")
    print("Install with: pip3 install Pillow\n")


# ── OSM tile math ────────────────────────────────────────────────────────────

def lat_lon_to_tile(lat, lon, zoom):
    """Return (x, y) OSM tile for a lat/lon at given zoom."""
    lat_r = math.radians(lat)
    n = 2 ** zoom
    x = int((lon + 180.0) / 360.0 * n)
    y = int((1.0 - math.log(math.tan(lat_r) + 1.0 / math.cos(lat_r)) / math.pi) / 2.0 * n)
    return x, y

def tile_to_lat_lon(x, y, zoom):
    """Return (lat, lon) of the NW corner of a tile."""
    n = 2 ** zoom
    lon = x / n * 360.0 - 180.0
    lat_r = math.atan(math.sinh(math.pi * (1 - 2 * y / n)))
    lat = math.degrees(lat_r)
    return lat, lon

def tiles_in_bbox(lat_min, lat_max, lon_min, lon_max, zoom):
    """Return list of (x, y) tiles covering a bounding box."""
    x_min, y_max = lat_lon_to_tile(lat_min, lon_min, zoom)
    x_max, y_min = lat_lon_to_tile(lat_max, lon_max, zoom)
    tiles = []
    for x in range(x_min, x_max + 1):
        for y in range(y_min, y_max + 1):
            tiles.append((x, y))
    return tiles

def tiles_around(lat, lon, zoom, radius_km):
    """Return tiles within radius_km of a point."""
    # 1 degree lat ≈ 111 km
    delta_lat = radius_km / 111.0
    delta_lon = radius_km / (111.0 * math.cos(math.radians(lat)))
    return tiles_in_bbox(
        lat - delta_lat, lat + delta_lat,
        lon - delta_lon, lon + delta_lon,
        zoom
    )


# ── Regions ──────────────────────────────────────────────────────────────────

REGIONS = {
    "lazio":  (41.2, 42.8, 11.5, 14.1),   # lat_min, lat_max, lon_min, lon_max
    "rome":   (41.6, 42.1, 12.2, 12.8),
    "viterbo":(42.2, 42.6, 11.8, 12.4),
    "frosinone":(41.4,41.9,13.2,14.0),
    "latina": (41.2, 41.7, 12.6, 13.3),
}


# ── Download ─────────────────────────────────────────────────────────────────

MAP_SERVERS = [
    "https://tile.openstreetmap.org/{z}/{x}/{y}.png",
    "https://a.tile.openstreetmap.org/{z}/{x}/{y}.png",
    "https://b.tile.openstreetmap.org/{z}/{x}/{y}.png",
]
# Esri World Imagery — free for non-commercial use, no API key required
# Returns JPEG directly (no conversion needed)
SAT_SERVER = "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}"

_server_idx = 0

def download_tile_raw(z, x, y, layer="map", retries=3):
    """Download tile bytes. layer='map' uses OSM PNG, layer='sat' uses Esri JPEG."""
    global _server_idx
    if layer == "sat":
        url = SAT_SERVER.format(z=z, x=x, y=y)
    else:
        url = MAP_SERVERS[_server_idx % len(MAP_SERVERS)].format(z=z, x=x, y=y)
        _server_idx += 1
    headers = {"User-Agent": "BikeGPS-TileDownloader/1.0 (personal use)"}
    for attempt in range(retries):
        try:
            req = urllib.request.Request(url, headers=headers)
            with urllib.request.urlopen(req, timeout=15) as resp:
                return resp.read()
        except Exception as e:
            if attempt == retries - 1:
                raise
            time.sleep(1 + attempt)

def to_baseline_jpeg(data, quality=75):
    """Convert image bytes (PNG or JPEG) to baseline JPEG via Pillow."""
    img = Image.open(BytesIO(data)).convert("RGB")
    buf = BytesIO()
    img.save(buf, format="JPEG", quality=quality, optimize=False,
             progressive=False, subsampling=0)
    return buf.getvalue()


def download_and_save(z, x, y, out_dir, layer="map", jpeg_quality=75, skip_existing=True):
    if layer == "sat":
        path_jpg = os.path.join(out_dir, "sat", str(z), str(x), f"{y}.jpg")
    else:
        path_jpg = os.path.join(out_dir, str(z), str(x), f"{y}.jpg")
    path_png = path_jpg.replace(".jpg", ".png")

    if skip_existing and (os.path.exists(path_jpg) or os.path.exists(path_png)):
        return "skip"

    os.makedirs(os.path.dirname(path_jpg), exist_ok=True)

    raw_data = download_tile_raw(z, x, y, layer)

    if HAS_PIL:
        jpeg_data = to_baseline_jpeg(raw_data, jpeg_quality)
        with open(path_jpg, "wb") as f:
            f.write(jpeg_data)
        return f"{len(jpeg_data)//1024}KB"
    else:
        # Esri returns JPEG directly; OSM returns PNG — save as-is
        ext = ".jpg" if layer == "sat" else ".png"
        save_path = path_jpg if ext == ".jpg" else path_png
        with open(save_path, "wb") as f:
            f.write(raw_data)
        return f"{len(raw_data)//1024}KB"


# ── Main ─────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Download map/satellite tiles for BikeGPS SD card")
    parser.add_argument("--out", default="/Volumes/MSD/tiles", help="Output directory (default: /Volumes/MSD/tiles)")
    parser.add_argument("--zoom", default="15", help="Zoom level(s), e.g. '14,15' (default: 15)")
    parser.add_argument("--layer", choices=["map", "sat"], default="map",
                        help="Tile layer: 'map' (OSM) or 'sat' (Esri satellite). Default: map")
    parser.add_argument("--region", choices=list(REGIONS.keys()), help="Predefined region")
    parser.add_argument("--lat", type=float, help="Center latitude")
    parser.add_argument("--lon", type=float, help="Center longitude")
    parser.add_argument("--radius", type=float, default=15, help="Radius in km around lat/lon (default: 15)")
    parser.add_argument("--quality", type=int, default=75, help="JPEG quality 1-95 (default: 75)")
    parser.add_argument("--dry-run", action="store_true", help="Count tiles without downloading")
    parser.add_argument("--yes", "-y", action="store_true", help="Skip confirmation prompt for large downloads")
    parser.add_argument("--delay", type=float, default=None,
                        help="Seconds between requests (default: 0.15 map, 0.05 sat)")
    args = parser.parse_args()

    zooms = [int(z.strip()) for z in args.zoom.split(",")]

    # Build tile list
    all_tiles = []  # list of (z, x, y)
    for z in zooms:
        if args.region:
            lat_min, lat_max, lon_min, lon_max = REGIONS[args.region]
            tiles = tiles_in_bbox(lat_min, lat_max, lon_min, lon_max, z)
        elif args.lat and args.lon:
            tiles = tiles_around(args.lat, args.lon, z, args.radius)
        else:
            # Default: Rome + 15km
            tiles = tiles_around(41.9028, 12.4964, z, args.radius)
            print(f"No location specified — using Rome center (41.9028, 12.4964), radius {args.radius}km")
        all_tiles += [(z, x, y) for x, y in tiles]
        print(f"Zoom {z}: {len(tiles)} tiles")

    total = len(all_tiles)
    est_mb = total * 30 / 1024  # ~30KB per tile
    sat_note = " → /sat/ subdir" if args.layer == "sat" else ""
    print(f"\nTotal: {total} tiles [{args.layer.upper()}], estimated ~{est_mb:.0f} MB")
    print(f"Output: {args.out}{sat_note}")

    if args.dry_run:
        print("\n[dry-run] No files downloaded.")
        return

    if total > 5000 and not args.yes:
        resp = input(f"\n⚠️  {total} tiles is a lot. Continue? [y/N] ")
        if resp.lower() != "y":
            print("Aborted.")
            return

    print(f"\nDownloading {total} tiles...")
    ok = skip = err = 0
    start = time.time()

    for i, (z, x, y) in enumerate(all_tiles):
        try:
            result = download_and_save(z, x, y, args.out, args.layer, args.quality)
            if result == "skip":
                skip += 1
            else:
                ok += 1
                delay = args.delay if args.delay is not None else (0.05 if args.layer == "sat" else 0.3)
                time.sleep(delay)
        except Exception as e:
            print(f"\n  ERR z={z} x={x} y={y}: {e}")
            err += 1

        # Progress
        if (i + 1) % 20 == 0 or i == total - 1:
            elapsed = time.time() - start
            rate = (ok + skip) / elapsed if elapsed > 0 else 0
            eta = (total - i - 1) / rate if rate > 0 else 0
            print(f"  [{i+1}/{total}] ok={ok} skip={skip} err={err}  "
                  f"{rate:.1f} t/s  ETA {eta/60:.1f}min", end="\r")

    print(f"\n\nDone! ok={ok} skip={skip} err={err} in {(time.time()-start)/60:.1f} min")

    # Show disk usage
    total_size = sum(
        os.path.getsize(os.path.join(root, f))
        for root, _, files in os.walk(args.out)
        for f in files
    )
    print(f"Disk used: {total_size/1024/1024:.1f} MB")


if __name__ == "__main__":
    main()
