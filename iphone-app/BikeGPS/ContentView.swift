import SwiftUI
import MapKit

struct ContentView: View {

    @EnvironmentObject var bleMgr:      BLEManager
    @EnvironmentObject var locationMgr: LocationManager
    @EnvironmentObject var navMgr:      NavigationManager

    @State private var showSearch = false

    // MARK: – Root: TabView

    var body: some View {
        TabView {
            gpsTab
                .tabItem { Label("GPS", systemImage: "speedometer") }

            MapTab()
                .tabItem { Label("Mappa", systemImage: "map.fill") }
        }
        .preferredColorScheme(.dark)
        .accentColor(.cyan)
    }

    // MARK: – GPS Tab

    private var gpsTab: some View {
        ZStack {
            Color.black.ignoresSafeArea()

            VStack(spacing: 0) {

                // ── Header bar ─────────────────────────────────────────────
                HStack(spacing: 8) {
                    Circle()
                        .fill(bleMgr.isConnected ? Color.green : Color.orange)
                        .frame(width: 10, height: 10)
                    Text(bleMgr.statusText)
                        .font(.system(size: 14, weight: .medium, design: .monospaced))
                        .foregroundColor(.white.opacity(0.85))
                    Spacer()
                    if let loc = locationMgr.location {
                        Text(String(format: "±%.0f m", loc.horizontalAccuracy))
                            .font(.caption2)
                            .foregroundColor(.secondary)
                    }
                    Button {
                        navMgr.audioEnabled.toggle()
                    } label: {
                        Image(systemName: navMgr.audioEnabled
                              ? "speaker.wave.2.fill"
                              : "speaker.slash.fill")
                            .font(.system(size: 15))
                            .foregroundColor(navMgr.audioEnabled ? .cyan : .secondary)
                    }
                    .padding(.leading, 4)
                }
                .padding(.horizontal, 16)
                .padding(.vertical, 10)
                .background(Color(white: 0.08))

                Spacer()

                // ── Speed display ──────────────────────────────────────────
                VStack(spacing: 2) {
                    Text(speedString)
                        .font(.system(size: 96, weight: .bold, design: .rounded))
                        .foregroundColor(.white)
                        .monospacedDigit()
                    Text("km/h")
                        .font(.title3)
                        .foregroundColor(.secondary)
                }

                Spacer()

                // ── Heading ────────────────────────────────────────────────
                HStack(spacing: 20) {
                    CompassView(heading: headingDegrees)
                        .frame(width: 80, height: 80)

                    VStack(alignment: .leading, spacing: 6) {
                        Label(headingLabel, systemImage: "arrow.up.circle")
                            .font(.callout)
                            .foregroundColor(.white.opacity(0.8))
                        Label(coordString, systemImage: "mappin.circle")
                            .font(.caption)
                            .foregroundColor(.secondary)
                            .lineLimit(1)
                            .minimumScaleFactor(0.7)
                    }
                }
                .padding(.horizontal, 24)

                Spacer()

                // ── Navigation banner (when active) ────────────────────────
                if navMgr.isNavigating {
                    NavBanner(navMgr: navMgr)
                        .padding(.horizontal, 16)
                        .padding(.bottom, 8)
                }

                // ── Location permission prompt ─────────────────────────────
                if locationMgr.authStatus == .denied ||
                   locationMgr.authStatus == .restricted {
                    PermissionBanner()
                }

                // ── Search / nav control bar ───────────────────────────────
                SearchBarRow(isNavigating: navMgr.isNavigating) {
                    showSearch = true
                }
                .padding(.horizontal, 16)
                .padding(.bottom, 12)
            }
        }
        .sheet(isPresented: $showSearch) {
            SearchSheet(navMgr: navMgr, isPresented: $showSearch)
        }
    }

    // MARK: – Computed helpers

    private var speedString: String {
        guard let loc = locationMgr.location else { return "--" }
        return "\(Int(max(0, loc.speed * 3.6)))"
    }

    private var headingDegrees: Double {
        locationMgr.heading?.trueHeading ?? 0
    }

    private var headingLabel: String {
        let deg  = headingDegrees
        let dirs = ["N","NE","E","SE","S","SW","W","NW"]
        let idx  = Int((deg + 22.5) / 45) % 8
        return "\(dirs[idx])  \(String(format: "%.0f°", deg))"
    }

    private var coordString: String {
        guard let loc = locationMgr.location else { return "-- , --" }
        return String(format: "%.5f°  %.5f°",
                      loc.coordinate.latitude,
                      loc.coordinate.longitude)
    }
}

// MARK: – Navigation banner

struct NavBanner: View {
    @ObservedObject var navMgr: NavigationManager

    private var maneuverIcon: String {
        switch navMgr.maneuver {
        case 0:  return "arrow.up"
        case 1:  return "arrow.turn.up.right"
        case 2:  return "arrow.turn.up.left"
        case 3:  return "arrow.uturn.left"
        case 4:  return "mappin.circle.fill"
        default: return "location.fill"
        }
    }

    private var distanceText: String {
        if navMgr.maneuver == 4 { return "Sei arrivato!" }
        let d = navMgr.distanceToStep
        if d >= 1000 { return String(format: "fra %.1f km", Double(d) / 1000.0) }
        return "fra \(d) m"
    }

    var body: some View {
        HStack(spacing: 12) {
            if navMgr.isRerouting {
                ProgressView()
                    .tint(.white)
                    .frame(width: 32)
            } else {
                Image(systemName: maneuverIcon)
                    .font(.title2)
                    .foregroundColor(.white)
                    .frame(width: 32)
            }

            VStack(alignment: .leading, spacing: 2) {
                Text(navMgr.stepInstruction)
                    .font(.callout.weight(.semibold))
                    .foregroundColor(.white)
                    .lineLimit(2)
                if !navMgr.isRerouting {
                    Text(distanceText)
                        .font(.caption)
                        .foregroundColor(.white.opacity(0.75))
                }
            }

            Spacer()

            Button {
                navMgr.stopNavigation()
            } label: {
                Image(systemName: "xmark.circle.fill")
                    .font(.title2)
                    .foregroundColor(.red.opacity(0.9))
            }
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .background(
            RoundedRectangle(cornerRadius: 12)
                .fill(Color.blue.opacity(0.75))
        )
    }
}

// MARK: – Search bar row

struct SearchBarRow: View {
    let isNavigating: Bool
    let onTap: () -> Void

    var body: some View {
        Button(action: onTap) {
            HStack(spacing: 10) {
                Image(systemName: "magnifyingglass")
                    .foregroundColor(.secondary)
                Text(isNavigating ? "Cambia destinazione" : "Dove vuoi andare?")
                    .foregroundColor(.secondary)
                Spacer()
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 12)
            .background(Color(white: 0.15))
            .clipShape(RoundedRectangle(cornerRadius: 10))
        }
    }
}

// MARK: – Search sheet

struct SearchSheet: View {
    @ObservedObject var navMgr: NavigationManager
    @Binding var isPresented:   Bool
    @State private var query = ""
    @FocusState private var fieldFocused: Bool

    var body: some View {
        NavigationView {
            VStack(spacing: 0) {

                // Search field
                HStack(spacing: 8) {
                    Image(systemName: "magnifyingglass").foregroundColor(.secondary)
                    TextField("Inserisci destinazione…", text: $query)
                        .focused($fieldFocused)
                        .submitLabel(.search)
                        .onSubmit { performSearch() }
                    if !query.isEmpty {
                        Button { query = "" } label: {
                            Image(systemName: "xmark.circle.fill").foregroundColor(.secondary)
                        }
                    }
                }
                .padding(12)
                .background(Color(white: 0.12))

                Button(action: performSearch) {
                    Label("Cerca", systemImage: "arrow.right.circle.fill")
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 12)
                }
                .buttonStyle(.borderedProminent)
                .padding(.horizontal, 16)
                .padding(.top, 10)

                Divider().padding(.top, 10)

                // Content area
                Group {
                    if navMgr.isSearching {
                        VStack(spacing: 12) {
                            ProgressView()
                            Text("Ricerca in corso…").font(.caption).foregroundColor(.secondary)
                        }
                        .frame(maxWidth: .infinity, maxHeight: .infinity)

                    } else if let err = navMgr.errorMessage {
                        VStack(spacing: 8) {
                            Image(systemName: "exclamationmark.triangle")
                                .font(.largeTitle).foregroundColor(.orange)
                            Text(err).foregroundColor(.orange).multilineTextAlignment(.center)
                        }
                        .frame(maxWidth: .infinity, maxHeight: .infinity).padding()

                    } else if !navMgr.searchResults.isEmpty {
                        // Search results
                        List(navMgr.searchResults, id: \.self) { item in
                            Button {
                                Task {
                                    await navMgr.startNavigation(to: item)
                                    if navMgr.isNavigating { isPresented = false }
                                }
                            } label: {
                                VStack(alignment: .leading, spacing: 3) {
                                    Text(item.name ?? "Senza nome")
                                        .foregroundColor(.primary)
                                        .font(.callout.weight(.medium))
                                    if let title = item.placemark.title {
                                        Text(title).font(.caption)
                                            .foregroundColor(.secondary).lineLimit(2)
                                    }
                                }
                                .padding(.vertical, 2)
                            }
                        }
                        .listStyle(.plain)

                    } else if query.isEmpty && !navMgr.recentDestinations.isEmpty {
                        // Recent destinations
                        List {
                            Section {
                                ForEach(navMgr.recentDestinations) { recent in
                                    Button {
                                        Task {
                                            await navMgr.startNavigation(to: recent.toMapItem())
                                            if navMgr.isNavigating { isPresented = false }
                                        }
                                    } label: {
                                        HStack(spacing: 10) {
                                            Image(systemName: "clock.arrow.circlepath")
                                                .foregroundColor(.secondary)
                                                .font(.callout)
                                            VStack(alignment: .leading, spacing: 2) {
                                                Text(recent.name)
                                                    .foregroundColor(.primary)
                                                    .font(.callout.weight(.medium))
                                                if !recent.subtitle.isEmpty {
                                                    Text(recent.subtitle)
                                                        .font(.caption)
                                                        .foregroundColor(.secondary)
                                                        .lineLimit(1)
                                                }
                                            }
                                        }
                                        .padding(.vertical, 2)
                                    }
                                }
                            } header: {
                                Text("Recenti").font(.caption).foregroundColor(.secondary)
                            }
                        }
                        .listStyle(.plain)

                    } else {
                        Text("Cerca una destinazione sopra")
                            .font(.callout).foregroundColor(.secondary)
                            .frame(maxWidth: .infinity, maxHeight: .infinity)
                    }
                }
            }
            .navigationTitle("Destinazione")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Chiudi") { isPresented = false }
                }
            }
        }
        .onAppear { fieldFocused = true }
    }

    private func performSearch() {
        Task { await navMgr.search(query: query) }
    }
}

// MARK: – CompassView

struct CompassView: View {
    let heading: Double
    var body: some View {
        ZStack {
            Circle().stroke(Color.white.opacity(0.15), lineWidth: 1)
            Capsule()
                .fill(LinearGradient(colors: [.red, .orange],
                                     startPoint: .top, endPoint: .bottom))
                .frame(width: 6, height: 30)
                .offset(y: -10)
                .rotationEffect(.degrees(heading))
            Circle().fill(Color.white.opacity(0.6)).frame(width: 6, height: 6)
        }
    }
}

// MARK: – Permission banner

struct PermissionBanner: View {
    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: "location.slash.fill").foregroundColor(.orange)
            Text("Location access denied. Open Settings → Privacy → Location.")
                .font(.caption).foregroundColor(.orange)
        }
        .padding(12)
        .background(Color.orange.opacity(0.12))
        .clipShape(RoundedRectangle(cornerRadius: 8))
        .padding(.horizontal, 16).padding(.bottom, 12)
    }
}

#Preview {
    ContentView()
        .environmentObject(BLEManager())
        .environmentObject(LocationManager())
        .environmentObject(NavigationManager(locationMgr: LocationManager()))
}
