import CoreBluetooth
import Combine

/// Manages the CoreBluetooth central role, auto-reconnect, and NUS packet TX.
final class BLEManager: NSObject, ObservableObject {

    // ── Nordic UART Service UUIDs ─────────────────────────────────────────────
    static let nusService = CBUUID(string: "6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
    static let nusRx      = CBUUID(string: "6E400002-B5A3-F393-E0A9-E50E24DCCA9E") // phone→ESP
    static let nusTx      = CBUUID(string: "6E400003-B5A3-F393-E0A9-E50E24DCCA9E") // ESP→phone

    // ── Published state ───────────────────────────────────────────────────────
    @Published var isConnected = false
    @Published var statusText  = "Scanning…"

    // ── Private ───────────────────────────────────────────────────────────────
    private var central:     CBCentralManager!
    private var peripheral:  CBPeripheral?
    private var rxChar:      CBCharacteristic?
    private var mtuPayload   = 182     // conservative default (185 - 3 ATT header)
    private let bleQueue     = DispatchQueue(label: "com.bikegps.ble")

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: bleQueue,
                                   options: [CBCentralManagerOptionShowPowerAlertKey: true])
    }

    // MARK: – Public send API

    /// Sends `data` over BLE UART, fragmenting if needed.
    /// Safe to call from any thread.
    func send(_ data: Data) {
        bleQueue.async { [weak self] in self?._send(data) }
    }

    private func _send(_ data: Data) {
        guard let p = peripheral, let rx = rxChar, isConnected else { return }

        // How many payload bytes fit in one write-without-response?
        let mtu = p.maximumWriteValueLength(for: .withoutResponse)
        // Reserve space for "PKT:NNN/NNN:" prefix (up to 14 bytes)
        let chunkSize = max(64, mtu - 14)

        if data.count <= mtu {
            p.writeValue(data, for: rx, type: .withoutResponse)
            return
        }

        // Fragment
        var offset   = 0
        var fragIdx  = 1
        let total    = Int(ceil(Double(data.count) / Double(chunkSize)))

        while offset < data.count {
            let end     = min(offset + chunkSize, data.count)
            let chunk   = data[offset..<end]
            let prefix  = "PKT:\(fragIdx)/\(total):".data(using: .utf8)!
            let packet  = prefix + chunk

            p.writeValue(packet, for: rx, type: .withoutResponse)

            offset  += chunk.count
            fragIdx += 1

            // Small inter-fragment gap to avoid peripheral RX buffer overflow
            Thread.sleep(forTimeInterval: 0.006)
        }
    }

    // MARK: – Scanning

    private func startScan() {
        guard central.state == .poweredOn else { return }
        DispatchQueue.main.async { self.statusText = "Scanning…" }
        // Scansiona tutti i dispositivi: il filtro per UUID manca l'ESP32
        // quando il nome è nel scan response e l'UUID nell'advertising primario è assente.
        central.scanForPeripherals(withServices: nil,
                                   options: [CBCentralManagerScanOptionAllowDuplicatesKey: false])
    }
}

// MARK: – CBCentralManagerDelegate
extension BLEManager: CBCentralManagerDelegate {

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:   startScan()
        case .poweredOff:  DispatchQueue.main.async { self.statusText = "BT Off" }
        case .unauthorized: DispatchQueue.main.async { self.statusText = "BT Denied" }
        default:           break
        }
    }

    func centralManager(_ central: CBCentralManager,
                        didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any],
                        rssi RSSI: NSNumber) {
        guard self.peripheral == nil else { return }

        // Filtra per nome "BikeGPS" oppure per UUID NUS nei servizi advertised
        let name = peripheral.name ?? (advertisementData[CBAdvertisementDataLocalNameKey] as? String ?? "")
        let services = advertisementData[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID] ?? []
        guard name == "BikeGPS" || services.contains(BLEManager.nusService) else { return }

        self.peripheral = peripheral
        central.stopScan()
        DispatchQueue.main.async { self.statusText = "Connecting…" }
        central.connect(peripheral, options: nil)
    }

    func centralManager(_ central: CBCentralManager,
                        didConnect peripheral: CBPeripheral) {
        DispatchQueue.main.async { self.statusText = "Connected" ; self.isConnected = true }
        peripheral.delegate = self
        peripheral.discoverServices([BLEManager.nusService])
    }

    func centralManager(_ central: CBCentralManager,
                        didFailToConnect peripheral: CBPeripheral,
                        error: Error?) {
        print("[ble] failed to connect: \(error?.localizedDescription ?? "?")")
        self.peripheral = nil
        rxChar = nil
        startScan()
    }

    func centralManager(_ central: CBCentralManager,
                        didDisconnectPeripheral peripheral: CBPeripheral,
                        error: Error?) {
        print("[ble] disconnected")
        DispatchQueue.main.async { self.isConnected = false ; self.statusText = "Disconnected" }
        self.peripheral = nil
        rxChar = nil
        // Auto-reconnect after a brief pause
        DispatchQueue.global().asyncAfter(deadline: .now() + 2) { [weak self] in
            self?.startScan()
        }
    }
}

// MARK: – CBPeripheralDelegate
extension BLEManager: CBPeripheralDelegate {

    func peripheral(_ peripheral: CBPeripheral,
                    didDiscoverServices error: Error?) {
        guard error == nil else { return }
        for service in peripheral.services ?? [] where service.uuid == BLEManager.nusService {
            peripheral.discoverCharacteristics([BLEManager.nusRx, BLEManager.nusTx], for: service)
        }
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didDiscoverCharacteristicsFor service: CBService,
                    error: Error?) {
        guard error == nil else { return }
        for char in service.characteristics ?? [] {
            switch char.uuid {
            case BLEManager.nusRx:
                rxChar = char
            case BLEManager.nusTx:
                peripheral.setNotifyValue(true, for: char)
            default:
                break
            }
        }
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didUpdateValueFor characteristic: CBCharacteristic,
                    error: Error?) {
        // Handle any data sent by the ESP32 → phone direction here if needed
    }
}
