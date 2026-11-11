import AVFoundation
import CoreBluetooth
import SwiftUI
import UniformTypeIdentifiers

private struct AudioClip {
    static let sampleRate = 22_050.0
    static let maxSeconds = 30.0

    let frames: Int
    let compressed: [UInt8]
    let checksum: UInt32

    enum LoadError: LocalizedError {
        case tooLong, invalidAudio

        var errorDescription: String? {
            switch self {
            case .tooLong: return "Choose an audio file that is 30 seconds or shorter."
            case .invalidAudio: return "The selected file could not be decoded."
            }
        }
    }

    init(url: URL) throws {
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }

        let file = try AVAudioFile(forReading: url)
        let input = file.processingFormat
        guard file.length > 0, input.sampleRate > 0 else { throw LoadError.invalidAudio }
        guard Double(file.length) / input.sampleRate <= Self.maxSeconds,
              file.length <= AVAudioFramePosition(UInt32.max)
        else { throw LoadError.tooLong }

        guard let source = AVAudioPCMBuffer(
            pcmFormat: input, frameCapacity: 8192
        ), let mono = AVAudioFormat(
            commonFormat: .pcmFormatFloat32, sampleRate: Self.sampleRate,
            channels: 1, interleaved: false
        ), let output = AVAudioPCMBuffer(
            pcmFormat: mono, frameCapacity: AVAudioFrameCount(Self.sampleRate * Self.maxSeconds) + 1
        ), let converter = AVAudioConverter(from: input, to: mono)
        else { throw LoadError.invalidAudio }

        var readError: Error?
        var conversionError: NSError?
        let result = converter.convert(to: output, error: &conversionError) { requested, status in
            let remaining = file.length - file.framePosition
            guard remaining > 0 else {
                status.pointee = .endOfStream
                return nil
            }
            let count = min(Int64(requested), Int64(source.frameCapacity), remaining)
            do {
                try file.read(into: source, frameCount: AVAudioFrameCount(count))
                if source.frameLength > 0 {
                    status.pointee = .haveData
                    return source
                }
            } catch {
                readError = error
            }
            status.pointee = .endOfStream
            return nil
        }
        if let readError { throw readError }
        if let conversionError { throw conversionError }
        guard result != .error, output.frameLength > 0,
              let samples = output.floatChannelData?[0]
        else { throw LoadError.invalidAudio }

        frames = min(Int(output.frameLength), Int(Self.sampleRate * Self.maxSeconds))
        compressed = Self.encode(samples, count: frames)
        checksum = compressed.reduce(UInt32(2_166_136_261)) { ($0 ^ UInt32($1)) &* 16_777_619 }
    }

    // IMA ADPCM with a fresh predictor and index for each clip.
    private static func encode(_ samples: UnsafePointer<Float>, count: Int) -> [UInt8] {
        let steps = [
            7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
            34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
            157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
            598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707,
            1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
            5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635,
            13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
        ]
        let indexChange = [-1, -1, -1, -1, 2, 4, 6, 8]
        var packed = [UInt8](repeating: 0, count: (count + 1) / 2)
        var predictor = 0
        var index = 0
        for i in 0..<count {
            let sample = samples[i].isFinite ? min(1, max(-1, samples[i])) : 0
            let difference = Int(sample * 32767) - predictor
            var remaining = abs(difference)
            let step = steps[index]
            var delta = step >> 3
            var code = difference < 0 ? 8 : 0
            for bit in [4, 2, 1] {
                let part = (step * bit) >> 2
                if remaining >= part {
                    remaining -= part
                    delta += part
                    code |= bit
                }
            }
            predictor = min(32767, max(-32768, predictor + (difference < 0 ? -delta : delta)))
            index = min(88, max(0, index + indexChange[code & 7]))
            packed[i / 2] |= UInt8(code) << ((i & 1) * 4)
        }
        return packed
    }
}

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
    private var clip: AudioClip?
    private var clipSubscriber: UUID?
    private var transferTimer: Timer?
    private var transferOffset = 0
    private var startSent = false
    private var cancelling = false
    private var lastProgressTime = 0.0

    @Published private(set) var isSendingAudio = false
    @Published private(set) var canSendAudio = false
    @Published private(set) var progress = 0.0

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
        finishTransfer()
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
        if clipSubscriber == central.identifier {
            finishTransfer()
            status = "Disconnected during audio transfer."
            return
        }
        refreshSendState()
        if subscribers.isEmpty {
            status = "Waiting for the iPod to subscribe again."
        }
    }

    func peripheralManagerIsReady(toUpdateSubscribers peripheral: CBPeripheralManager) {
        waitingForQueue = false
        refreshSendState()
        if isSendingAudio {
            pumpTransfer()
        } else if canSend {
            status = "Ready to send. Try again."
        }
    }

    private func refreshSendState() {
        let limits = subscribers.values.map(\.maximumUpdateValueLength)
        byteLimit = limits.min().map { min($0, 64) } ?? 0
        canSend = byteLimit > 0 && !waitingForQueue && !isSendingAudio
        canSendAudio = canSend && byteLimit >= 10 && subscribers.count == 1
    }

    func chooseAudio(_ result: Result<URL, Error>) {
        guard canSendAudio, let subscriber = subscribers.values.first else { return }
        do {
            clip = try AudioClip(url: result.get())
        } catch {
            status = error.localizedDescription
            return
        }

        clipSubscriber = subscriber.identifier
        progress = 0
        transferOffset = 0
        startSent = false
        cancelling = false
        lastProgressTime = ProcessInfo.processInfo.systemUptime
        isSendingAudio = true
        refreshSendState()
        status = "Sending audio…"
        transferTimer = Timer.scheduledTimer(withTimeInterval: 0.015, repeats: true) { [weak self] _ in
            self?.pumpTransfer()
        }
        pumpTransfer()
    }

    func cancelAudio() {
        guard isSendingAudio else { return }
        cancelling = true
        status = "Cancelling audio transfer…"
        pumpTransfer()
    }

    private func finishTransfer() {
        transferTimer?.invalidate()
        transferTimer = nil
        clip = nil
        clipSubscriber = nil
        isSendingAudio = false
        refreshSendState()
    }

    private func pumpTransfer() {
        guard let clip, let id = clipSubscriber, let central = subscribers[id] else { return }

        if ProcessInfo.processInfo.systemUptime - lastProgressTime >= 15 {
            finishTransfer()
            status = "Bluetooth transfer stalled. Try sending the file again."
            return
        }

        if cancelling {
            guard !startSent || manager.updateValue(Data([0, 67]), for: tx, onSubscribedCentrals: [central])
            else { return }
            finishTransfer()
            status = "Audio transfer cancelled."
            return
        }

        if !startSent {
            var start = Data([0, 65])
            appendWord(UInt32(clip.frames), to: &start)
            appendWord(clip.checksum, to: &start)
            guard manager.updateValue(start, for: tx, onSubscribedCentrals: [central]) else { return }
            startSent = true
            lastProgressTime = ProcessInfo.processInfo.systemUptime
        }

        let packetPayload = min(central.maximumUpdateValueLength, 244) - 6
        guard packetPayload > 0 else {
            finishTransfer()
            status = "The Bluetooth packet size is too small."
            return
        }

        // Leave time for the receiver between bursts and advance only when
        // CoreBluetooth accepts the notification into its transmit queue.
        for _ in 0..<8 {
            let end = min(transferOffset + packetPayload, clip.compressed.count)
            var packet = Data([0, 68])
            appendWord(UInt32(transferOffset), to: &packet)
            packet.append(contentsOf: clip.compressed[transferOffset..<end])
            guard manager.updateValue(packet, for: tx, onSubscribedCentrals: [central]) else { break }
            transferOffset = end
            lastProgressTime = ProcessInfo.processInfo.systemUptime
            if end == clip.compressed.count {
                finishTransfer()
                progress = 1
                status = "Audio queued. Check playback on the iPod."
                return
            }
        }
        progress = Double(transferOffset) / Double(clip.compressed.count)
    }

    private func appendWord(_ value: UInt32, to packet: inout Data) {
        var littleEndian = value.littleEndian
        withUnsafeBytes(of: &littleEndian) { packet.append(contentsOf: $0) }
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
    @State private var choosingAudio = false

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
                HStack {
                    Button("Send Audio…") { choosingAudio = true }
                        .disabled(!uart.canSendAudio)
                    if uart.isSendingAudio {
                        ProgressView(value: uart.progress)
                        Button("Cancel") { uart.cancelAudio() }
                    } else {
                        Text("Up to 30 seconds")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
                if uart.byteLimit > 0 {
                    Text("Up to \(uart.byteLimit) printable ASCII characters")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            .padding(22)
            .frame(width: 420)
            .frame(minHeight: 155)
            .fileImporter(isPresented: $choosingAudio, allowedContentTypes: [.audio]) {
                uart.chooseAudio($0)
            }
        }
        .windowResizability(.contentSize)
    }
}
