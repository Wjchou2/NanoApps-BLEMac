import CoreBluetooth
import SwiftUI

private final class UARTPeripheral: NSObject, ObservableObject, CBPeripheralManagerDelegate {
    private let service = CBMutableService(
        type: CBUUID(string: "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"),
        primary: true
    )
    private let tx = CBMutableCharacteristic(
        type: CBUUID(string: "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"),
        properties: [.notify],
        value: nil,
        permissions: []
    )
    private var manager: CBPeripheralManager!
    private var subscribers: [UUID: CBCentral] = [:]
    private var waitingForQueue = false

    @Published private(set) var status = "Starting Bluetooth…"
    @Published private(set) var byteLimit = 0
    @Published private(set) var canSend = false

    override init() {
        super.init()
        service.characteristics = [tx]
        manager = CBPeripheralManager(delegate: self, queue: nil)
    }

    func peripheralManagerDidUpdateState(_ peripheral: CBPeripheralManager) {
        subscribers.removeAll()
        waitingForQueue = false
        refreshSendState()
        switch peripheral.state {
        case .poweredOn:
            status = "Registering Nordic UART…"
            peripheral.add(service)
        case .poweredOff:
            status = "Turn on Bluetooth in System Settings."
        case .unauthorized:
            status = "Allow Bluetooth access in Privacy & Security."
        case .unsupported:
            status = "Bluetooth peripheral mode is unavailable on this Mac."
        case .resetting, .unknown:
            status = "Waiting for Bluetooth…"
        @unknown default:
            status = "Bluetooth is unavailable."
        }
    }

    func peripheralManager(_ peripheral: CBPeripheralManager, didAdd service: CBService, error: Error?) {
        if let error {
            status = "Could not register UART. \(error.localizedDescription)"
            return
        }
        peripheral.startAdvertising([CBAdvertisementDataLocalNameKey: "Nano UART"])
    }

    func peripheralManagerDidStartAdvertising(_ peripheral: CBPeripheralManager, error: Error?) {
        if let error {
            status = "Could not advertise. \(error.localizedDescription)"
        } else if subscribers.isEmpty {
            status = "Advertising Nano UART. Scan and connect on the iPod."
        }
    }

    func peripheralManager(
        _ peripheral: CBPeripheralManager,
        central: CBCentral,
        didSubscribeTo characteristic: CBCharacteristic
    ) {
        guard characteristic.uuid == tx.uuid else { return }
        subscribers[central.identifier] = central
        refreshSendState()
        status = "Subscribed. Ready to send."
    }

    func peripheralManager(
        _ peripheral: CBPeripheralManager,
        central: CBCentral,
        didUnsubscribeFrom characteristic: CBCharacteristic
    ) {
        guard characteristic.uuid == tx.uuid else { return }
        subscribers.removeValue(forKey: central.identifier)
        refreshSendState()
        if subscribers.isEmpty {
            status = "Waiting for the iPod to subscribe again."
        }
    }

    func peripheralManagerIsReady(toUpdateSubscribers peripheral: CBPeripheralManager) {
        waitingForQueue = false
        refreshSendState()
        if canSend {
            status = "Ready to send. Try again."
        }
    }

    private func refreshSendState() {
        let limits = subscribers.values.map(\.maximumUpdateValueLength)
        byteLimit = limits.min().map { min($0, 64) } ?? 0
        canSend = byteLimit > 0 && !waitingForQueue
    }

    func send(_ message: String) {
        guard canSend else { return }
        let bytes = message.utf8
        guard !bytes.isEmpty, bytes.allSatisfy({ (32...126).contains($0) }) else {
            status = "Enter printable ASCII text."
            return
        }
        guard bytes.count <= byteLimit else {
            status = "Keep the message to \(byteLimit) characters or fewer."
            return
        }
        if manager.updateValue(Data(bytes), for: tx, onSubscribedCentrals: Array(subscribers.values)) {
            status = "Notification queued. Check the iPod."
        } else {
            waitingForQueue = true
            refreshSendState()
            status = "Waiting for Bluetooth to accept another message…"
        }
    }
}

@main
private struct NanoUARTSender: App {
    @StateObject private var uart = UARTPeripheral()
    @State private var message = "Hello"

    var body: some Scene {
        Window("Nano UART Sender", id: "sender") {
            VStack(alignment: .leading, spacing: 16) {
                Text(uart.status)
                    .font(.callout)
                    .foregroundStyle(.secondary)
                HStack {
                    TextField("Message", text: $message)
                        .textFieldStyle(.roundedBorder)
                        .onSubmit { uart.send(message) }
                    Button("Send") { uart.send(message) }
                        .disabled(!uart.canSend || message.isEmpty)
                }
                if uart.byteLimit > 0 {
                    Text("Up to \(uart.byteLimit) printable ASCII characters")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            .padding(22)
            .frame(width: 420)
            .frame(minHeight: 125)
        }
        .windowResizability(.contentSize)
    }
}
