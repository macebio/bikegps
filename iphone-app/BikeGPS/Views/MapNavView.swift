import SwiftUI
import MapKit
import CoreLocation

// MARK: – Map Tab container

struct MapTab: View {
    @EnvironmentObject var navMgr:      NavigationManager
    @EnvironmentObject var locationMgr: LocationManager

    @State private var pendingCoord:    CLLocationCoordinate2D? = nil
    @State private var pendingName      = ""
    @State private var pendingSubtitle  = ""
    @State private var showConfirmCard  = false

    var body: some View {
        ZStack(alignment: .bottom) {
            BikeMapView(
                navMgr:          navMgr,
                locationMgr:     locationMgr,
                pendingCoord:    pendingCoord,
                onCrosshairPick: { coord, name, subtitle in
                    pendingCoord    = coord
                    pendingName     = name
                    pendingSubtitle = subtitle
                    showConfirmCard = true          // idempotent on update calls
                }
            )
            .ignoresSafeArea()

            VStack(spacing: 0) {
                // Nav banner when navigating
                if navMgr.isNavigating {
                    NavBanner(navMgr: navMgr)
                        .padding(.horizontal, 12)
                        .padding(.bottom, 6)
                }

                // Destination confirmation card (appears after tap/drag)
                if showConfirmCard, let coord = pendingCoord {
                    ConfirmCard(
                        name:     pendingName.isEmpty
                                    ? coordinateLabel(coord)
                                    : pendingName,
                        subtitle: pendingSubtitle,
                        onConfirm: {
                            let pl   = MKPlacemark(coordinate: coord)
                            let item = MKMapItem(placemark: pl)
                            item.name = pendingName.isEmpty ? "Posizione" : pendingName
                            Task { await navMgr.startNavigation(to: item) }
                            clearCrosshair()
                        },
                        onCancel: clearCrosshair
                    )
                    .padding(.horizontal, 12)
                    .padding(.bottom, 8)
                    .transition(.move(edge: .bottom).combined(with: .opacity))
                }
            }
            .animation(.spring(response: 0.3), value: showConfirmCard)
            .animation(.spring(response: 0.3), value: navMgr.isNavigating)
        }
        .preferredColorScheme(.dark)
    }

    private func clearCrosshair() {
        pendingCoord    = nil
        pendingName     = ""
        pendingSubtitle = ""
        showConfirmCard = false
    }

    private func coordinateLabel(_ c: CLLocationCoordinate2D) -> String {
        String(format: "%.5f°, %.5f°", c.latitude, c.longitude)
    }
}

// MARK: – Destination confirmation card

struct ConfirmCard: View {
    let name:      String
    let subtitle:  String
    let onConfirm: () -> Void
    let onCancel:  () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 10) {
                Image(systemName: "scope")
                    .foregroundColor(.red)
                    .font(.title3)
                VStack(alignment: .leading, spacing: 2) {
                    Text(name)
                        .font(.callout.weight(.semibold))
                        .foregroundColor(.primary)
                        .lineLimit(1)
                    if !subtitle.isEmpty {
                        Text(subtitle)
                            .font(.caption)
                            .foregroundColor(.secondary)
                            .lineLimit(1)
                    }
                }
                Spacer()
                Button(action: onCancel) {
                    Image(systemName: "xmark")
                        .foregroundColor(.secondary)
                        .padding(6)
                }
            }
            Button(action: onConfirm) {
                Label("Naviga qui", systemImage: "arrow.triangle.turn.up.right.circle.fill")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
        }
        .padding(14)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 14))
    }
}

// MARK: – MKMapView wrapper

struct BikeMapView: UIViewRepresentable {
    @ObservedObject var navMgr:      NavigationManager
    @ObservedObject var locationMgr: LocationManager
    /// When set to nil, removes the crosshair annotation from the map.
    var pendingCoord: CLLocationCoordinate2D?
    /// Fired on tap (immediately with empty name) then again after reverse geocode,
    /// and again after the user finishes dragging.
    var onCrosshairPick: (CLLocationCoordinate2D, String, String) -> Void

    func makeCoordinator() -> Coordinator {
        Coordinator(onCrosshairPick: onCrosshairPick)
    }

    func makeUIView(context: Context) -> MKMapView {
        let map               = MKMapView()
        map.showsUserLocation = true
        map.userTrackingMode  = .follow
        map.showsCompass      = true
        map.showsScale        = true
        map.delegate          = context.coordinator

        // Single tap → place crosshair at that exact point
        let tap = UITapGestureRecognizer(
            target: context.coordinator,
            action: #selector(Coordinator.handleTap(_:)))
        tap.numberOfTapsRequired = 1
        map.addGestureRecognizer(tap)

        context.coordinator.map = map
        return map
    }

    func updateUIView(_ map: MKMapView, context: Context) {
        context.coordinator.onCrosshairPick = onCrosshairPick

        // When pending cleared (cancel / confirm), remove the crosshair pin
        if pendingCoord == nil {
            let pins = map.annotations.compactMap { $0 as? CrosshairPin }
            if !pins.isEmpty {
                map.removeAnnotations(pins)
                context.coordinator.crosshairPin = nil
            }
        }

        syncOverlays(map)
        syncDestinationPin(map)
    }

    // ── Route polyline ────────────────────────────────────────

    private func syncOverlays(_ map: MKMapView) {
        let existing = map.overlays.compactMap { $0 as? MKPolyline }
        if let newRoute = navMgr.currentRoute {
            if existing.first !== newRoute.polyline {
                map.removeOverlays(existing)
                map.addOverlay(newRoute.polyline, level: .aboveRoads)
            }
        } else if !existing.isEmpty {
            map.removeOverlays(existing)
        }
    }

    // ── Destination pin ───────────────────────────────────────

    private func syncDestinationPin(_ map: MKMapView) {
        let pins = map.annotations.compactMap { $0 as? DestPin }
        if navMgr.isNavigating, let coord = navMgr.destinationCoordinate {
            if let first = pins.first {
                first.coordinate = coord
                first.title      = navMgr.destinationName
            } else {
                let pin        = DestPin()
                pin.coordinate = coord
                pin.title      = navMgr.destinationName
                map.addAnnotation(pin)
            }
        } else if !pins.isEmpty {
            map.removeAnnotations(pins)
        }
    }

    // MARK: – Coordinator

    final class Coordinator: NSObject, MKMapViewDelegate {
        var onCrosshairPick: (CLLocationCoordinate2D, String, String) -> Void
        weak var map: MKMapView?
        var crosshairPin: CrosshairPin?          // strong ref so we can move it

        init(onCrosshairPick: @escaping (CLLocationCoordinate2D, String, String) -> Void) {
            self.onCrosshairPick = onCrosshairPick
        }

        // ── Tap: place / move crosshair ───────────────────────
        @objc func handleTap(_ gesture: UITapGestureRecognizer) {
            guard gesture.state == .ended, let map else { return }
            let point = gesture.location(in: map)

            // Skip tap if it lands directly on an existing annotation view
            for ann in map.annotations where !(ann is MKUserLocation) {
                if let view = map.view(for: ann), view.frame.insetBy(dx: -8, dy: -8).contains(point) {
                    return
                }
            }

            let coord = map.convert(point, toCoordinateFrom: map)
            placeCrosshair(at: coord, on: map)
        }

        func placeCrosshair(at coord: CLLocationCoordinate2D, on map: MKMapView) {
            // Move existing pin or create a new one
            if let pin = crosshairPin {
                pin.coordinate = coord
            } else {
                let pin        = CrosshairPin()
                pin.coordinate = coord
                map.addAnnotation(pin)
                crosshairPin = pin
            }

            // Fire immediately with coordinates so card appears without waiting
            onCrosshairPick(coord, "", "")

            // Then update with a real place name
            reverseGeocode(coord: coord) { [weak self] name, subtitle in
                self?.onCrosshairPick(coord, name, subtitle)
            }
        }

        // ── Drag: re-geocode after drop ───────────────────────
        func mapView(_ mapView: MKMapView,
                     annotationView view: MKAnnotationView,
                     didChange newState: MKAnnotationView.DragState,
                     fromOldState oldState: MKAnnotationView.DragState) {
            guard newState == .ending,
                  let pin = view.annotation as? CrosshairPin else { return }
            let coord = pin.coordinate
            // Show coordinates immediately while geocoding
            onCrosshairPick(coord, "", "")
            reverseGeocode(coord: coord) { [weak self] name, subtitle in
                self?.onCrosshairPick(coord, name, subtitle)
            }
        }

        // ── Helpers ───────────────────────────────────────────
        private func reverseGeocode(coord: CLLocationCoordinate2D,
                                    completion: @escaping (String, String) -> Void) {
            let loc = CLLocation(latitude: coord.latitude, longitude: coord.longitude)
            CLGeocoder().reverseGeocodeLocation(loc) { placemarks, _ in
                DispatchQueue.main.async {
                    let name     = placemarks?.first?.name
                        ?? placemarks?.first?.locality
                        ?? "Posizione"
                    let subtitle = placemarks?.first?.thoroughfare ?? ""
                    completion(name, subtitle)
                }
            }
        }

        // ── Overlay renderer ──────────────────────────────────
        func mapView(_ mapView: MKMapView,
                     rendererFor overlay: MKOverlay) -> MKOverlayRenderer {
            guard let poly = overlay as? MKPolyline else {
                return MKOverlayRenderer(overlay: overlay)
            }
            let r         = MKPolylineRenderer(polyline: poly)
            r.strokeColor = UIColor.systemBlue
            r.lineWidth   = 5
            r.lineCap     = .round
            return r
        }

        // ── Annotation views ──────────────────────────────────
        func mapView(_ mapView: MKMapView,
                     viewFor annotation: MKAnnotation) -> MKAnnotationView? {
            if annotation is CrosshairPin {
                let id   = "crosshair"
                let view = mapView.dequeueReusableAnnotationView(withIdentifier: id)
                    as? CrosshairAnnotationView
                    ?? CrosshairAnnotationView(annotation: annotation,
                                               reuseIdentifier: id)
                view.annotation = annotation
                return view
            }
            if annotation is DestPin {
                let id   = "dest"
                let view = mapView.dequeueReusableAnnotationView(withIdentifier: id)
                    as? MKMarkerAnnotationView
                    ?? MKMarkerAnnotationView(annotation: annotation,
                                              reuseIdentifier: id)
                view.annotation      = annotation
                view.markerTintColor = .systemRed
                view.glyphImage      = UIImage(systemName: "flag.fill")
                view.canShowCallout  = true
                return view
            }
            return nil
        }
    }
}

// MARK: – Crosshair annotation view (draggable ⊕)

final class CrosshairAnnotationView: MKAnnotationView {
    override init(annotation: MKAnnotation?, reuseIdentifier: String?) {
        super.init(annotation: annotation, reuseIdentifier: reuseIdentifier)
        let cfg = UIImage.SymbolConfiguration(pointSize: 38, weight: .regular)
        image          = UIImage(systemName: "scope", withConfiguration: cfg)?
                             .withTintColor(.systemRed, renderingMode: .alwaysOriginal)
        centerOffset   = .zero          // scope centre sits exactly at the coordinate
        isDraggable    = true
        canShowCallout = false
    }
    required init?(coder: NSCoder) { fatalError() }
}

// MARK: – Annotation pin classes

final class CrosshairPin: MKPointAnnotation {}
final class DestPin:      MKPointAnnotation {}
