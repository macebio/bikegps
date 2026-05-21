import CoreLocation
import Combine

/// Wraps CLLocationManager; publishes the latest location and heading.
final class LocationManager: NSObject, ObservableObject {

    // ── Published state ───────────────────────────────────────────────────────
    @Published var location:    CLLocation?
    @Published var heading:     CLHeading?
    @Published var authStatus:  CLAuthorizationStatus = .notDetermined

    // ── Private ───────────────────────────────────────────────────────────────
    private let manager = CLLocationManager()

    override init() {
        super.init()
        manager.delegate                     = self
        manager.desiredAccuracy              = kCLLocationAccuracyBestForNavigation
        manager.distanceFilter               = kCLDistanceFilterNone
        manager.headingFilter                = 1.0       // update every 1°
        manager.allowsBackgroundLocationUpdates = true
        manager.pausesLocationUpdatesAutomatically = false
        manager.activityType                 = .otherNavigation
        manager.requestAlwaysAuthorization()
    }

    func start() {
        manager.startUpdatingLocation()
        manager.startUpdatingHeading()
    }

    func stop() {
        manager.stopUpdatingLocation()
        manager.stopUpdatingHeading()
    }

    /// Best-effort snapshot.  Returns nil if no fix yet.
    var snapshot: GPSSnapshot? {
        guard let loc = location else { return nil }
        let spd = max(0, loc.speed) * 3.6          // m/s → km/h
        let hdg = heading?.trueHeading ?? max(0, loc.course)
        return GPSSnapshot(
            coordinate: loc.coordinate,
            speed:      spd,
            heading:    hdg,
            timestamp:  loc.timestamp
        )
    }
}

// MARK: – CLLocationManagerDelegate
extension LocationManager: CLLocationManagerDelegate {

    func locationManager(_ manager: CLLocationManager,
                         didUpdateLocations locations: [CLLocation]) {
        guard let last = locations.last else { return }
        // Ignore stale fixes (> 5 s old)
        guard abs(last.timestamp.timeIntervalSinceNow) < 5 else { return }
        DispatchQueue.main.async { self.location = last }
    }

    func locationManager(_ manager: CLLocationManager,
                         didUpdateHeading newHeading: CLHeading) {
        DispatchQueue.main.async { self.heading = newHeading }
    }

    func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
        DispatchQueue.main.async {
            self.authStatus = manager.authorizationStatus
            if manager.authorizationStatus == .authorizedAlways ||
               manager.authorizationStatus == .authorizedWhenInUse {
                self.start()
            }
        }
    }

    func locationManager(_ manager: CLLocationManager,
                         didFailWithError error: Error) {
        print("[location] error: \(error.localizedDescription)")
    }
}
