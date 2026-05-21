import Foundation
import CoreLocation

/// Snapshot of location + heading captured at one instant.
struct GPSSnapshot {
    let coordinate: CLLocationCoordinate2D
    let speed: Double       // km/h (already converted from m/s)
    let heading: Double     // true heading, degrees 0–360
    let timestamp: Date
}
