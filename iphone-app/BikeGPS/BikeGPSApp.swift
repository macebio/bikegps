import SwiftUI

/// Application state container – owns all singleton manager instances.
/// Uses explicit init() to guarantee deterministic initialization order.
@MainActor
final class AppState: ObservableObject {
    let locationMgr:   LocationManager
    let bleMgr:        BLEManager
    let navigationMgr: NavigationManager
    let transmitter:   GPSTransmitter

    init() {
        let loc = LocationManager()
        let ble = BLEManager()
        let nav = NavigationManager(locationMgr: loc)
        locationMgr   = loc
        bleMgr        = ble
        navigationMgr = nav
        transmitter   = GPSTransmitter(locationMgr: loc, bleMgr: ble, navMgr: nav)
    }
}

@main
struct BikeGPSApp: App {

    @StateObject private var appState = AppState()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(appState.bleMgr)
                .environmentObject(appState.locationMgr)
                .environmentObject(appState.navigationMgr)
                .onAppear    { appState.transmitter.start() }
                .onDisappear { appState.transmitter.stop() }
        }
    }
}
