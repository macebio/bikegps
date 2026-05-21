import Foundation
import MapKit
import CoreLocation
import AVFoundation

// MARK: – Recent destination (persisted in UserDefaults)

struct RecentDestination: Codable, Identifiable {
    let id:       UUID
    let name:     String
    let subtitle: String
    let lat:      Double
    let lon:      Double
    let date:     Date

    var coordinate: CLLocationCoordinate2D {
        CLLocationCoordinate2D(latitude: lat, longitude: lon)
    }

    func toMapItem() -> MKMapItem {
        let placemark = MKPlacemark(coordinate: coordinate)
        let item      = MKMapItem(placemark: placemark)
        item.name     = name
        return item
    }
}

// MARK: – NavigationManager

/// Manages turn-by-turn navigation via MapKit.
/// Exposes state to GPSTransmitter (BLE → ESP32), ContentView, and MapTab.
@MainActor
final class NavigationManager: ObservableObject {

    // ── Published state ───────────────────────────────────────
    @Published var isNavigating    = false
    @Published var maneuver: Int   = -1     // -1=off 0=straight 1=right 2=left 3=uturn 4=arrive
    @Published var distanceToStep  = 0      // metres to next maneuver
    @Published var stepInstruction = ""     // full instruction for iPhone display
    @Published var streetName      = ""     // ≤24 chars sent to ESP32

    // Route / destination (consumed by MapTab)
    @Published var currentRoute:          MKRoute?
    @Published var destinationCoordinate: CLLocationCoordinate2D?
    @Published var destinationName        = ""
    @Published var isRerouting            = false

    // Search
    @Published var searchResults: [MKMapItem] = []
    @Published var isSearching     = false
    @Published var errorMessage: String?

    // Recents
    @Published var recentDestinations: [RecentDestination] = []

    // Audio toggle (persisted)
    @Published var audioEnabled: Bool = true {
        didSet { UserDefaults.standard.set(audioEnabled, forKey: "audioEnabled") }
    }

    // ── Private ───────────────────────────────────────────────
    private var steps:      [MKRoute.Step] = []
    private var stepIndex   = 0
    private weak var locationMgr: LocationManager?

    // Audio
    private let synth  = AVSpeechSynthesizer()
    private var ann500 = false
    private var ann200 = false
    private var ann50  = false
    private var annNow = false

    // Off-route detection
    private var offRouteCount   = 0
    private let offRouteMetres  = 80.0   // distance from polyline that counts as "off"
    private let offRouteTrigger = 5      // consecutive seconds before rerouting

    // Recents storage
    private let recentsKey   = "recentDestinations"
    private let maxRecents   = 8
    private let dedupeRadius = 100.0   // metres

    // ── Init ──────────────────────────────────────────────────
    init(locationMgr: LocationManager) {
        self.locationMgr        = locationMgr
        self.recentDestinations = loadRecents()
        self.audioEnabled       = UserDefaults.standard.object(forKey: "audioEnabled") as? Bool ?? true
    }

    // MARK: – Search

    func search(query: String) async {
        let q = query.trimmingCharacters(in: .whitespaces)
        guard !q.isEmpty else { return }
        isSearching   = true
        errorMessage  = nil
        searchResults = []

        let req = MKLocalSearch.Request()
        req.naturalLanguageQuery = q
        if let loc = locationMgr?.location {
            req.region = MKCoordinateRegion(
                center: loc.coordinate,
                latitudinalMeters: 80_000, longitudinalMeters: 80_000)
        }
        do {
            let resp = try await MKLocalSearch(request: req).start()
            searchResults = resp.mapItems
        } catch {
            errorMessage = "Ricerca fallita"
        }
        isSearching = false
    }

    // MARK: – Start / stop

    func startNavigation(to destination: MKMapItem) async {
        guard let loc = locationMgr?.location else {
            errorMessage = "GPS non disponibile"; return
        }
        errorMessage  = nil
        searchResults = []

        configureAudio()

        let req = MKDirections.Request()
        req.source        = MKMapItem(placemark: MKPlacemark(coordinate: loc.coordinate))
        req.destination   = destination
        req.transportType = .automobile
        req.requestsAlternateRoutes = false

        do {
            let resp  = try await MKDirections(request: req).calculate()
            guard let route = resp.routes.first else {
                errorMessage = "Nessun percorso trovato"; return
            }
            currentRoute          = route
            destinationCoordinate = destination.placemark.coordinate
            destinationName       = destination.name ?? "Destinazione"

            steps      = route.steps.filter { !$0.instructions.isEmpty }
            stepIndex  = 0
            resetAnnouncements()
            isNavigating = true
            addRecent(destination)
            refreshStep(from: loc)
        } catch {
            errorMessage = "Routing: \(error.localizedDescription)"
        }
    }

    func stopNavigation() {
        isNavigating    = false
        isRerouting     = false
        steps           = []
        stepIndex       = 0
        offRouteCount   = 0
        maneuver        = -1
        distanceToStep  = 0
        stepInstruction = ""
        streetName      = ""
        currentRoute    = nil
        destinationCoordinate = nil
        destinationName = ""
        if synth.isSpeaking { synth.stopSpeaking(at: .immediate) }
        resetAnnouncements()
    }

    // MARK: – Step tracking (called every second from GPSTransmitter)

    func tick(location: CLLocation) {
        guard isNavigating, !steps.isEmpty, !isRerouting else { return }

        // Off-route detection: if the user is too far from the polyline for
        // offRouteTrigger consecutive seconds, recalculate.
        let dist = distanceToRoute(location)
        if dist > offRouteMetres {
            offRouteCount += 1
            if offRouteCount >= offRouteTrigger {
                offRouteCount = 0
                Task { await self.reroute(from: location) }
                return
            }
        } else {
            offRouteCount = 0
        }

        refreshStep(from: location)
    }

    // MARK: – Private: route step logic

    private func refreshStep(from location: CLLocation) {
        guard stepIndex < steps.count else {
            maneuver        = 4
            stepInstruction = "Sei arrivato!"
            streetName      = "Destinazione"
            distanceToStep  = 0
            if !annNow { annNow = true; speak("Sei arrivato a destinazione") }
            return
        }

        let step    = steps[stepIndex]
        let endLoc  = CLLocation(latitude:  endCoord(step).latitude,
                                 longitude: endCoord(step).longitude)
        let rawDist = location.distance(from: endLoc)

        // Advance step when within 30 m of endpoint
        if rawDist < 30, stepIndex + 1 < steps.count {
            stepIndex += 1
            resetAnnouncements()
            refreshStep(from: location)
            return
        }

        let dist        = max(0, Int(rawDist.rounded()))
        distanceToStep  = dist
        stepInstruction = step.instructions
        maneuver        = parseManeuver(step.instructions)
        streetName      = String(step.instructions.prefix(24))

        triggerAudio(dist: dist, instruction: step.instructions)
    }

    private func endCoord(_ step: MKRoute.Step) -> CLLocationCoordinate2D {
        let n = step.polyline.pointCount
        guard n > 0 else { return step.polyline.coordinate }
        return step.polyline.points()[n - 1].coordinate
    }

    // Returns the shortest distance (metres) from `location` to any point on
    // the current route polyline. Exits early once a point < 30 m is found.
    private func distanceToRoute(_ location: CLLocation) -> CLLocationDistance {
        guard let route = currentRoute else { return 0 }
        let pts   = route.polyline.points()
        let count = route.polyline.pointCount
        var minD  = Double.greatestFiniteMagnitude
        for i in 0..<count {
            let c = pts[i].coordinate
            let d = location.distance(from: CLLocation(latitude: c.latitude,
                                                       longitude: c.longitude))
            if d < minD { minD = d }
            if minD < 30 { return minD }    // close enough, no need to keep scanning
        }
        return minD
    }

    // Recalculate route from current position to the original destination.
    private func reroute(from location: CLLocation) async {
        guard let destCoord = destinationCoordinate else { return }
        isRerouting     = true
        stepInstruction = "Ricalcolo percorso…"
        speak("Ricalcolo percorso")

        let destItem  = MKMapItem(placemark: MKPlacemark(coordinate: destCoord))
        destItem.name = destinationName

        let req = MKDirections.Request()
        req.source        = MKMapItem(placemark: MKPlacemark(coordinate: location.coordinate))
        req.destination   = destItem
        req.transportType = .automobile
        req.requestsAlternateRoutes = false

        do {
            let resp = try await MKDirections(request: req).calculate()
            guard let route = resp.routes.first else {
                isRerouting = false; return
            }
            currentRoute = route
            steps        = route.steps.filter { !$0.instructions.isEmpty }
            stepIndex    = 0
            resetAnnouncements()
            isRerouting = false
            if let loc = locationMgr?.location { refreshStep(from: loc) }
        } catch {
            isRerouting = false
            print("[nav] reroute failed: \(error.localizedDescription)")
        }
    }

    private func parseManeuver(_ instruction: String) -> Int {
        let s = instruction.lowercased()
        if s.contains("arriv") || s.contains("destinazione") ||
           s.contains("hai raggiunto") || s.contains("you have arrived") { return 4 }
        if s.contains("inversione") || s.contains("u-turn") ||
           s.contains("gira indiet")                                       { return 3 }
        if s.contains("destra") || s.contains("right")                    { return 1 }
        if s.contains("sinistra") || s.contains("left")                   { return 2 }
        return 0
    }

    // MARK: – Private: audio

    private func configureAudio() {
        do {
            try AVAudioSession.sharedInstance().setCategory(
                .playback, mode: .voicePrompt,
                options: [.duckOthers, .mixWithOthers])
            try AVAudioSession.sharedInstance().setActive(true)
        } catch {
            print("[audio] session: \(error)")
        }
    }

    private func speak(_ text: String) {
        guard audioEnabled else { return }
        if synth.isSpeaking { synth.stopSpeaking(at: .word) }
        let u       = AVSpeechUtterance(string: text)
        u.voice     = AVSpeechSynthesisVoice(language: "it-IT")
        u.rate      = 0.52
        u.volume    = 0.9
        synth.speak(u)
    }

    private func triggerAudio(dist: Int, instruction: String) {
        let dir = audioPhrase(for: instruction)
        if      dist <= 15  && !annNow  { annNow  = true; speak(dir) }
        else if dist <= 50  && !ann50   { ann50   = true; speak("Fra 50 metri, \(dir.lowercased())") }
        else if dist <= 200 && !ann200  { ann200  = true; speak("Fra 200 metri, \(dir.lowercased())") }
        else if dist <= 500 && !ann500  { ann500  = true; speak("Fra 500 metri, \(dir.lowercased())") }
    }

    private func audioPhrase(for instruction: String) -> String {
        let s = instruction.lowercased()
        if s.contains("destra")     { return "Gira a destra" }
        if s.contains("sinistra")   { return "Gira a sinistra" }
        if s.contains("inversione") || s.contains("indiet") { return "Fai inversione a U" }
        if s.contains("arriv") || s.contains("destinazione") { return "Sei arrivato a destinazione" }
        return "Continua dritto"
    }

    private func resetAnnouncements() {
        ann500 = false; ann200 = false; ann50 = false; annNow = false
    }

    // MARK: – Private: recent destinations

    private func addRecent(_ item: MKMapItem) {
        let coord = item.placemark.coordinate
        recentDestinations.removeAll {
            let a = CLLocation(latitude: $0.lat, longitude: $0.lon)
            let b = CLLocation(latitude: coord.latitude, longitude: coord.longitude)
            return a.distance(from: b) < dedupeRadius
        }
        let r = RecentDestination(
            id:       UUID(),
            name:     item.name ?? "Destinazione",
            subtitle: item.placemark.title ?? "",
            lat:      coord.latitude,
            lon:      coord.longitude,
            date:     Date()
        )
        recentDestinations.insert(r, at: 0)
        if recentDestinations.count > maxRecents {
            recentDestinations = Array(recentDestinations.prefix(maxRecents))
        }
        saveRecents()
    }

    private func loadRecents() -> [RecentDestination] {
        guard let data = UserDefaults.standard.data(forKey: recentsKey),
              let list = try? JSONDecoder().decode([RecentDestination].self, from: data)
        else { return [] }
        return list
    }

    private func saveRecents() {
        guard let data = try? JSONEncoder().encode(recentDestinations) else { return }
        UserDefaults.standard.set(data, forKey: recentsKey)
    }
}
