import UIKit
import Foundation

/// Downloads and caches OSM raster tiles; crops a 172×160 region centred on
/// the current GPS position, stitching adjacent tiles when needed.
final class TileManager {

    // ── Constants ─────────────────────────────────────────────────────────────
    private let tileSize   = 256      // OSM tile pixels
    private let targetW    = 172      // display tile width
    private let targetH    = 160      // display tile height

    // ── Tile cache (keyed "zoom/x/y") ─────────────────────────────────────────
    private var cache: [String: UIImage] = [:]
    private let cacheLimit = 64

    // ── URLSession with OSM-compliant User-Agent ───────────────────────────────
    private lazy var session: URLSession = {
        let cfg = URLSessionConfiguration.default
        cfg.httpAdditionalHeaders = [
            "User-Agent": "BikeGPS/1.0 (iOS; open-source hobby project)"
        ]
        cfg.urlCache            = URLCache(memoryCapacity: 4*1024*1024,
                                          diskCapacity:  32*1024*1024)
        cfg.requestCachePolicy  = .returnCacheDataElseLoad
        cfg.timeoutIntervalForRequest = 8
        return URLSession(configuration: cfg)
    }()

    // MARK: – Tile coordinate maths

    /// Returns (tileX, tileY) at `zoom` for a lat/lon.
    static func tileXY(lat: Double, lon: Double, zoom: Int) -> (x: Int, y: Int) {
        let n   = pow(2.0, Double(zoom))
        let x   = Int((lon + 180.0) / 360.0 * n)
        let lat = lat * .pi / 180.0
        let y   = Int((1.0 - log(tan(lat) + 1.0 / cos(lat)) / .pi) / 2.0 * n)
        return (x, y)
    }

    /// Returns the sub-tile pixel position (0…255, 0…255) for lat/lon.
    static func pixelInTile(lat: Double, lon: Double, zoom: Int) -> CGPoint {
        let n    = pow(2.0, Double(zoom))
        let xf   = (lon + 180.0) / 360.0 * n
        let latR = lat * .pi / 180.0
        let yf   = (1.0 - log(tan(latR) + 1.0 / cos(latR)) / .pi) / 2.0 * n
        return CGPoint(x: (xf - floor(xf)) * 256.0,
                       y: (yf - floor(yf)) * 256.0)
    }

    // MARK: – Public API

    /// Returns a `targetW × targetH` UIImage centred on `lat`/`lon`.
    func croppedTile(lat: Double, lon: Double, zoom: Int = 16) async throws -> UIImage {
        let (baseTX, baseTY) = Self.tileXY(lat: lat, lon: lon, zoom: zoom)
        let pix              = Self.pixelInTile(lat: lat, lon: lon, zoom: zoom)

        // Global pixel of the target centre
        let gx = baseTX * tileSize + Int(pix.x)
        let gy = baseTY * tileSize + Int(pix.y)

        // Crop rectangle in global pixel space
        let cropLeft = gx - targetW / 2
        let cropTop  = gy - targetH / 2
        let cropRect = CGRect(x: cropLeft, y: cropTop,
                              width: targetW, height: targetH)

        // Which tile columns / rows are required?
        let txMin = Int(floor(Double(cropLeft) / Double(tileSize)))
        let tyMin = Int(floor(Double(cropTop)  / Double(tileSize)))
        let txMax = Int(floor(Double(cropLeft + targetW - 1) / Double(tileSize)))
        let tyMax = Int(floor(Double(cropTop  + targetH - 1) / Double(tileSize)))

        // Download needed tiles concurrently
        let tiles: [(tx: Int, ty: Int, img: UIImage)] =
            try await withThrowingTaskGroup(of: (Int, Int, UIImage).self) { group in
                for ty in tyMin...tyMax {
                    for tx in txMin...txMax {
                        let ltx = tx, lty = ty
                        group.addTask { [weak self] in
                            guard let self else { throw URLError(.cancelled) }
                            let img = try await self.fetchTile(tx: ltx, ty: lty, zoom: zoom)
                            return (ltx, lty, img)
                        }
                    }
                }
                var results: [(Int, Int, UIImage)] = []
                for try await r in group { results.append(r) }
                return results
            }

        // Stitch tiles onto a canvas
        let canvasW = (txMax - txMin + 1) * tileSize
        let canvasH = (tyMax - tyMin + 1) * tileSize
        let stitchedRenderer = UIGraphicsImageRenderer(
            size: CGSize(width: canvasW, height: canvasH))
        let stitched = stitchedRenderer.image { _ in
            for (tx, ty, img) in tiles {
                let drawX = (tx - txMin) * tileSize
                let drawY = (ty - tyMin) * tileSize
                img.draw(in: CGRect(x: drawX, y: drawY,
                                    width: tileSize, height: tileSize))
            }
        }

        // Crop from stitched canvas
        let localCropX = cropLeft - txMin * tileSize
        let localCropY = cropTop  - tyMin * tileSize
        let localCrop  = CGRect(x: localCropX, y: localCropY,
                                width: targetW, height: targetH)
        guard let cg = stitched.cgImage?.cropping(to: localCrop) else {
            throw URLError(.badServerResponse)
        }

        // Return at exact target size (UIImage from cropping preserves scale)
        let final = UIImage(cgImage: cg, scale: 1.0, orientation: .up)
        return final
    }

    // MARK: – Private download

    private func fetchTile(tx: Int, ty: Int, zoom: Int) async throws -> UIImage {
        let key = "\(zoom)/\(tx)/\(ty)"

        // LRU-lite: check in-memory cache on the caller's task
        if let cached = cache[key] { return cached }

        // Clamp tile coordinates (OSM wraps X, not Y)
        let n       = 1 << zoom
        let wrappedX = ((tx % n) + n) % n
        guard ty >= 0 && ty < n else {
            // Off the edge of the map – return solid grey
            return solidTile(color: UIColor(white: 0.85, alpha: 1))
        }

        guard let url = URL(string:
            "https://tile.openstreetmap.org/\(zoom)/\(wrappedX)/\(ty).png") else {
            throw URLError(.badURL)
        }

        let (data, resp) = try await session.data(from: url)
        guard let http = resp as? HTTPURLResponse, http.statusCode == 200 else {
            throw URLError(.badServerResponse)
        }
        guard let img = UIImage(data: data) else {
            throw URLError(.cannotDecodeContentData)
        }

        // Evict oldest entries if cache is full
        if cache.count >= cacheLimit {
            cache.removeValue(forKey: cache.keys.first!)
        }
        cache[key] = img
        return img
    }

    /// Generates a plain-colour 256×256 tile as a fallback.
    private func solidTile(color: UIColor) -> UIImage {
        let r = UIGraphicsImageRenderer(size: CGSize(width: 256, height: 256))
        return r.image { ctx in color.setFill(); ctx.fill(CGRect(x:0,y:0,width:256,height:256)) }
    }
}
