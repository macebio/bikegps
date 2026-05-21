import Foundation
import CoreLocation
import UIKit

/// Sends GPS + telemetry + navigation to the ESP32 via BLE every second.
/// Payload: {"lat":..,"lon":..,"speed":..,"heading":..,"time":"HH:MM",
///           "alt":..,"bat":0.87,"wtmp":18.5,"wrain":30,
///           "nav":0,"ndist":120,"nst":"Via Roma"}   ← navigation fields (when active)
@MainActor
final class GPSTransmitter: ObservableObject {

    private let locationMgr: LocationManager
    private let bleMgr:      BLEManager
    private let navMgr:      NavigationManager
    private var timer:       Timer?

    // Weather cache (updated every 10 min)
    private var weatherTemp:       Double? = nil
    private var weatherRain:       Int?    = nil
    private var lastWeatherFetch:  Date    = .distantPast

    init(locationMgr: LocationManager, bleMgr: BLEManager, navMgr: NavigationManager) {
        self.locationMgr = locationMgr
        self.bleMgr      = bleMgr
        self.navMgr      = navMgr
        UIDevice.current.isBatteryMonitoringEnabled = true
    }

    func start() {
        guard timer == nil else { return }
        timer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            Task { await self?.transmit() }
        }
    }

    func stop() {
        timer?.invalidate()
        timer = nil
    }

    private func transmit() async {
        guard bleMgr.isConnected else { return }
        guard let snap = locationMgr.snapshot else { return }

        // Update navigation step tracking first (fresh values for this tick)
        if let loc = locationMgr.location {
            navMgr.tick(location: loc)
        }

        // Fetch weather every 10 minutes
        if Date().timeIntervalSince(lastWeatherFetch) > 600 {
            await fetchWeather(lat: snap.coordinate.latitude, lon: snap.coordinate.longitude)
        }

        let hhmm = DateFormatter.localizedString(from: Date(), dateStyle: .none, timeStyle: .short)
        let bat  = UIDevice.current.batteryLevel   // -1 if unavailable
        let alt  = locationMgr.location?.altitude ?? 0

        var json = String(format:
            "{\"lat\":%.6f,\"lon\":%.6f,\"speed\":%.1f,\"heading\":%.1f,\"time\":\"%@\",\"alt\":%.1f",
            snap.coordinate.latitude,
            snap.coordinate.longitude,
            max(snap.speed, 0),
            snap.heading >= 0 ? snap.heading : 0,
            hhmm,
            alt
        )
        if bat >= 0 { json += String(format: ",\"bat\":%.2f", Double(bat)) }
        if let wt = weatherTemp { json += String(format: ",\"wtmp\":%.1f", wt) }
        if let wr = weatherRain { json += ",\"wrain\":\(wr)" }

        // Navigation state — always include "nav" so ESP32 knows when to exit NAV_MODE
        json += ",\"nav\":\(navMgr.maneuver)"
        if navMgr.isNavigating && navMgr.maneuver >= 0 {
            json += ",\"ndist\":\(navMgr.distanceToStep)"
            // Sanitise: strip quotes, limit to 24 chars
            let safe = navMgr.streetName
                .replacingOccurrences(of: "\"", with: "'")
                .replacingOccurrences(of: "\\", with: "")
            json += ",\"nst\":\"\(safe)\""
        }

        json += "}"

        guard let data = json.data(using: .utf8) else { return }
        bleMgr.send(data)
    }

    private func fetchWeather(lat: Double, lon: Double) async {
        let urlStr = "https://api.open-meteo.com/v1/forecast?latitude=\(lat)&longitude=\(lon)&current=temperature_2m,precipitation_probability&timezone=auto&forecast_days=1"
        guard let url = URL(string: urlStr) else { return }
        do {
            let (data, _) = try await URLSession.shared.data(from: url)
            if let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
               let current = json["current"] as? [String: Any] {
                weatherTemp = current["temperature_2m"] as? Double
                weatherRain = current["precipitation_probability"] as? Int
                lastWeatherFetch = Date()
            }
        } catch {}
    }
}
