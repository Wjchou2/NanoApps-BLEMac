import AVFoundation
import CoreBluetooth
import SwiftUI
import UniformTypeIdentifiers

enum AudioQuality: Int, CaseIterable {
    case high = 1
    case medium = 2
    case low = 3

    var sampleRate: UInt32 { 22_050 / UInt32(rawValue) }

    var label: String {
        switch self {
        case .high: "High (22.05 kHz)"
        case .medium: "Medium (11.025 kHz)"
        case .low: "Low (7.35 kHz)"
        }
    }
}

private enum AudioLoadError: LocalizedError {
    case tooLong(String), invalidAudio

    var errorDescription: String? {
        switch self {
        case .tooLong(let seconds): "Choose an audio clip up to \(seconds) seconds long."
        case .invalidAudio: "The selected file could not be decoded."
        }
    }
}

private func decodeAudio(_ url: URL, rate: Double, frameLimit: Int,
                         maxDurationLabel: String, channels: AVAudioChannelCount = 1) throws -> AVAudioPCMBuffer {
    let scoped = url.startAccessingSecurityScopedResource()
    defer { if scoped { url.stopAccessingSecurityScopedResource() } }

    let file = try AVAudioFile(forReading: url)
    let input = file.processingFormat
    guard file.length > 0, input.sampleRate > 0 else { throw AudioLoadError.invalidAudio }
    guard Double(file.length) / input.sampleRate <= Double(frameLimit) / rate,
          file.length <= AVAudioFramePosition(UInt32.max)
    else { throw AudioLoadError.tooLong(maxDurationLabel) }

    guard let source = AVAudioPCMBuffer(
        pcmFormat: input, frameCapacity: 8192
    ), let pcm = AVAudioFormat(
        commonFormat: .pcmFormatFloat32, sampleRate: rate,
        channels: channels, interleaved: false
    ), let output = AVAudioPCMBuffer(
        pcmFormat: pcm, frameCapacity: AVAudioFrameCount(frameLimit + 1)
    ), let converter = AVAudioConverter(from: input, to: pcm)
    else { throw AudioLoadError.invalidAudio }

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
    guard result != .error, output.frameLength > 0 else { throw AudioLoadError.invalidAudio }
    return output
}

enum ClassicQuality: CaseIterable {
    case low, medium, high, fullMono, fullStereo

    var sampleRate: UInt32 {
        switch self {
        case .low: AudioQuality.low.sampleRate
        case .medium: AudioQuality.medium.sampleRate
        case .high: AudioQuality.high.sampleRate
        case .fullMono, .fullStereo: 44_100
        }
    }

    var channels: UInt16 { self == .fullStereo ? 2 : 1 }

    var label: String {
        switch self {
        case .low: AudioQuality.low.label
        case .medium: AudioQuality.medium.label
        case .high: AudioQuality.high.label
        case .fullMono: "Full (44.1 kHz mono)"
        case .fullStereo: "Full (44.1 kHz stereo)"
        }
    }
}

private struct AudioClip {
    static let maxSeconds = 30.0

    let frames: Int
    let compressed: [UInt8]
    let checksum: UInt32
    let quality: AudioQuality

    init(url: URL, quality: AudioQuality) throws {
        self.quality = quality
        let rate = Double(quality.sampleRate)
        let limit = Int(rate * Self.maxSeconds)
        let output = try decodeAudio(url, rate: rate, frameLimit: limit,
                                         maxDurationLabel: "30")
        guard let samples = output.floatChannelData?[0] else { throw AudioLoadError.invalidAudio }
        frames = min(Int(output.frameLength), limit)
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
    enum AudioTransport: String, CaseIterable {
        case read = "GATT Read"
        case notify = "Notifications"
        case fast = "Fast Notifications"
    }

    private var service: CBMutableService?
    private var tx: CBMutableCharacteristic?
    private var rx: CBMutableCharacteristic?
    private var audioRead: CBMutableCharacteristic?
    private var manager: CBPeripheralManager!
    private var serviceRegistered = false
    private var advertisingClassic = true
    @Published private(set) var classicDiscoveryHint = "Starting Bluetooth…"
    private var subscribers: [UUID: CBCentral] = [:]
    private var readCentral: UUID?
    private var waitingForQueue = false
    private var clip: AudioClip?
    private var clipSubscriber: UUID?
    private var transferTimer: Timer?
    private var transferOffset = 0
    private var packetSequence: UInt8 = 0
    private var startSent = false
    private var playOnReceive = false
    private var cancelling = false
    private var lastProgressTime = 0.0

    @Published private(set) var isSendingAudio = false
    @Published private(set) var canSendAudio = false
    @Published private(set) var progress = 0.0
    @Published var audioTransport: AudioTransport = .read {
        didSet { refreshSendState() }
    }

    @Published private(set) var status = "Starting Bluetooth…"
    @Published private(set) var receivedMessage = ""
    @Published private(set) var notificationLimit = 0
    var byteLimit: Int { min(notificationLimit, 64) }
    var audioPacketOverhead: Int { audioTransport == .fast ? 2 : 6 }
    @Published private(set) var canSend = false

    override init() {
        super.init()
        manager = CBPeripheralManager(delegate: self, queue: nil)
    }

    func peripheralManagerDidUpdateState(_ peripheral: CBPeripheralManager) {
        if peripheral.state == .poweredOn {
            refreshAdvertising(peripheral)
            return
        }
        serviceRegistered = false
        service = nil
        tx = nil
        rx = nil
        audioRead = nil
        subscribers.removeAll()
        readCentral = nil
        finishTransfer()
        waitingForQueue = false
        refreshSendState()
        switch peripheral.state {
        case .poweredOn:
            return
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
        classicDiscoveryHint = status
    }

    func peripheralManager(_ peripheral: CBPeripheralManager, didAdd service: CBService, error: Error?) {
        guard service === self.service, peripheral.state == .poweredOn else { return }
        if let error {
            self.service = nil
            tx = nil
            rx = nil
            audioRead = nil
            status = "Could not register UART. \(error.localizedDescription)"
            return
        }
        serviceRegistered = true
        if !advertisingClassic {
            refreshAdvertising(peripheral)
        }
    }

    func setClassicDiscovery(_ enabled: Bool) {
        guard advertisingClassic != enabled else { return }
        advertisingClassic = enabled
        refreshAdvertising(manager)
    }

    private func refreshAdvertising(_ peripheral: CBPeripheralManager) {
        guard peripheral.state == .poweredOn else { return }
        if !advertisingClassic && !serviceRegistered {
            peripheral.stopAdvertising()
            guard service == nil else { return }
            let tx = CBMutableCharacteristic(
                type: CBUUID(string: "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"),
                properties: [.notify], value: nil, permissions: []
            )
            let rx = CBMutableCharacteristic(
                type: CBUUID(string: "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"),
                properties: [.write], value: nil, permissions: [.writeable]
            )
            let audioRead = CBMutableCharacteristic(
                type: CBUUID(string: "6E400004-B5A3-F393-E0A9-E50E24DCCA9E"),
                properties: [.read], value: nil, permissions: [.readable]
            )
            let service = CBMutableService(
                type: CBUUID(string: "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"), primary: true
            )
            service.characteristics = [tx, audioRead, rx]
            self.tx = tx
            self.rx = rx
            self.audioRead = audioRead
            self.service = service
            status = "Registering Nordic UART…"
            peripheral.removeAllServices()
            peripheral.add(service)
            return
        }
        let address = advertisingClassic ? ClassicIdentity.localAddress : nil
        let name = address.map { "NM-" + $0.replacingOccurrences(of: ":", with: "") } ?? "Nano Bluetooth Link"
        if advertisingClassic {
            classicDiscoveryHint = address.map { "On the nano, use Find Mac, or Scan BLE in Bluetooth Test. Mac address \($0)." }
                ?? "macOS did not provide the Mac’s Classic Bluetooth address."
        }
        peripheral.stopAdvertising()
        peripheral.startAdvertising([CBAdvertisementDataLocalNameKey: name])
    }

    func peripheralManagerDidStartAdvertising(_ peripheral: CBPeripheralManager, error: Error?) {
        if let error {
            status = "Could not advertise. \(error.localizedDescription)"
            if advertisingClassic {
                classicDiscoveryHint = ClassicIdentity.localAddress.map { "BLE announcement unavailable. Use Mac Address on the nano and enter \($0)." }
                    ?? status
            }
        } else if !advertisingClassic && subscribers.isEmpty {
            status = "Advertising. Connect from BLE Chat or Bluetooth Test on the iPod."
        }
    }

    func peripheralManager(
        _ peripheral: CBPeripheralManager,
        central: CBCentral,
        didSubscribeTo characteristic: CBCharacteristic
    ) {
        guard let tx, characteristic.uuid == tx.uuid else { return }
        subscribers[central.identifier] = central
        refreshSendState()
        status = "Connected. Send a message or audio."
    }

    func peripheralManager(
        _ peripheral: CBPeripheralManager,
        central: CBCentral,
        didUnsubscribeFrom characteristic: CBCharacteristic
    ) {
        guard let tx, characteristic.uuid == tx.uuid else { return }
        subscribers.removeValue(forKey: central.identifier)
        if readCentral == central.identifier {
            readCentral = nil
        }
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

    func peripheralManager(_ peripheral: CBPeripheralManager, didReceiveWrite requests: [CBATTRequest]) {
        guard let request = requests.first else { return }
        guard requests.count == 1, let rx, request.characteristic.uuid == rx.uuid,
              subscribers[request.central.identifier] != nil else {
            peripheral.respond(to: request, withResult: .requestNotSupported)
            return
        }
        guard request.offset == 0 else {
            peripheral.respond(to: request, withResult: .invalidOffset)
            return
        }
        guard let bytes = request.value, (1...20).contains(bytes.count),
              bytes.allSatisfy({ (32...126).contains($0) }) else {
            peripheral.respond(to: request, withResult: .invalidAttributeValueLength)
            return
        }
        receivedMessage = String(decoding: bytes, as: UTF8.self)
        peripheral.respond(to: request, withResult: .success)
    }

    func peripheralManager(_ peripheral: CBPeripheralManager, didReceiveRead request: CBATTRequest) {
        guard let audioRead, request.characteristic.uuid == audioRead.uuid else {
            peripheral.respond(to: request, withResult: .requestNotSupported)
            return
        }
        guard request.offset == 0 else {
            peripheral.respond(to: request, withResult: .invalidOffset)
            return
        }
        let id = request.central.identifier
        request.value = Data([0, 73]) // Idle
        if subscribers[id] != nil {
            if readCentral != id {
                readCentral = id
                refreshSendState()
                if !isSendingAudio {
                    status = "GATT read ready."
                }
            }
            if audioTransport == .read, clipSubscriber == id, let clip {
                request.value = nextReadRecord(clip)
            }
        }
        peripheral.respond(to: request, withResult: .success)
    }

    private func nextReadRecord(_ clip: AudioClip) -> Data {
        lastProgressTime = ProcessInfo.processInfo.systemUptime
        if cancelling {
            finishTransfer()
            status = "Audio transfer cancelled."
            return Data([0, 67])
        }
        if !startSent {
            startSent = true
            return startRecord(clip)
        }

        // The stock nano issues a Read Blob when a response reaches 22 bytes.
        let end = min(transferOffset + 15, clip.compressed.count)
        let packet = dataRecord(clip, end: end)
        transferOffset = end
        progress = Double(end) / Double(clip.compressed.count)
        if end == clip.compressed.count {
            finishTransfer()
            status = "Audio sent. Check playback on the iPod."
        }
        return packet
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
        notificationLimit = limits.min() ?? 0
        canSend = byteLimit > 0 && !waitingForQueue && !isSendingAudio
        let minimumPacket = audioTransport == .fast ? 12 : 10
        canSendAudio = canSend && subscribers.count == 1 &&
            (audioTransport == .read ? subscribers.keys.first == readCentral : byteLimit >= minimumPacket)
    }

    func chooseAudio(_ result: Result<URL, Error>, quality: AudioQuality, playOnReceive: Bool) {
        guard canSendAudio, let subscriber = subscribers.values.first else { return }
        do {
            clip = try AudioClip(url: result.get(), quality: quality)
        } catch {
            status = error.localizedDescription
            return
        }

        clipSubscriber = subscriber.identifier
        self.playOnReceive = playOnReceive
        progress = 0
        transferOffset = 0
        packetSequence = 0
        startSent = false
        cancelling = false
        lastProgressTime = ProcessInfo.processInfo.systemUptime
        isSendingAudio = true
        refreshSendState()
        status = audioTransport == .read ? "Waiting for the iPod to read audio…" : "Sending audio…"
        let interval = audioTransport == .read ? 1.0 : 0.015
        transferTimer = Timer.scheduledTimer(withTimeInterval: interval, repeats: true) { [weak self] _ in
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
        guard let tx, let clip, let id = clipSubscriber, let central = subscribers[id] else { return }

        if ProcessInfo.processInfo.systemUptime - lastProgressTime >= 15 {
            finishTransfer()
            status = "Bluetooth transfer stalled. Try sending the file again."
            return
        }
        guard audioTransport != .read else { return }

        if cancelling {
            guard !startSent || manager.updateValue(Data([0, 67]), for: tx, onSubscribedCentrals: [central])
            else { return }
            finishTransfer()
            status = "Audio transfer cancelled."
            return
        }

        if !startSent {
            guard manager.updateValue(startRecord(clip), for: tx, onSubscribedCentrals: [central])
            else { return }
            startSent = true
            lastProgressTime = ProcessInfo.processInfo.systemUptime
        }

        let packetPayload = central.maximumUpdateValueLength - audioPacketOverhead
        guard packetPayload > 0 else {
            finishTransfer()
            status = "The Bluetooth packet size is too small."
            return
        }

        // Leave time for the receiver between bursts and advance only when
        // CoreBluetooth accepts the notification into its transmit queue.
        for _ in 0..<8 {
            let end = min(transferOffset + packetPayload, clip.compressed.count)
            let packet = dataRecord(clip, end: end)
            guard manager.updateValue(packet, for: tx, onSubscribedCentrals: [central]) else { break }
            packetSequence = (packetSequence &+ 1) & 0x7f
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

    private func startRecord(_ clip: AudioClip) -> Data {
        if audioTransport == .fast {
            var packet = Data([0, 70])
            appendWord(UInt32(clip.frames), to: &packet)
            appendWord(clip.checksum, to: &packet)
            packet.append(UInt8(clip.quality.rawValue))
            packet.append(playOnReceive ? 1 : 0)
            return packet
        }
        var packet = Data([0, playOnReceive ? 83 : clip.quality == .high ? 65 : 81])
        appendWord(UInt32(clip.frames), to: &packet)
        appendWord(clip.checksum, to: &packet)
        if playOnReceive || clip.quality != .high {
            packet.append(UInt8(clip.quality.rawValue))
        }
        return packet
    }

    private func dataRecord(_ clip: AudioClip, end: Int) -> Data {
        var packet = Data([0, audioTransport == .fast ? 0x80 | packetSequence : 68])
        if audioTransport != .fast {
            appendWord(UInt32(transferOffset), to: &packet)
        }
        packet.append(contentsOf: clip.compressed[transferOffset..<end])
        return packet
    }

    private func appendWord(_ value: UInt32, to packet: inout Data) {
        var littleEndian = value.littleEndian
        withUnsafeBytes(of: &littleEndian) { packet.append(contentsOf: $0) }
    }

    func send(_ message: String) {
        guard canSend, let tx else { return }
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
private struct NanoBluetoothLink: App {
    private enum Tab { case ble, youtube, youtubeVideo }

    @StateObject private var uart = UARTPeripheral()
    @State private var selectedTab = Tab.youtube
    @State private var message = "Hello"
    @State private var choosingAudio = false
    @State private var quality = AudioQuality.high
    @State private var playOnReceive = false
    @State private var showingAudioSettings = false

    var body: some Scene {
        Window("Nano Bluetooth Link", id: "sender") {
            TabView(selection: $selectedTab) {
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
                    if !uart.receivedMessage.isEmpty {
                        LabeledContent("From Nano", value: uart.receivedMessage)
                            .textSelection(.enabled)
                    }
                    if uart.byteLimit > 0 {
                        Text("Up to \(uart.byteLimit) printable ASCII characters")
                            .font(.caption)
                            .foregroundStyle(.secondary)
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
                    DisclosureGroup("Audio Settings", isExpanded: $showingAudioSettings) {
                        VStack(alignment: .leading, spacing: 10) {
                            Picker("Audio transport", selection: $uart.audioTransport) {
                                ForEach(UARTPeripheral.AudioTransport.allCases, id: \.self) { transport in
                                    Text(transport.rawValue).tag(transport)
                                }
                            }
                            Picker("Audio quality", selection: $quality) {
                                ForEach(AudioQuality.allCases, id: \.self) { option in
                                    Text(option.label).tag(option)
                                }
                            }
                            Toggle("Start playing on receive", isOn: $playOnReceive)
                                .toggleStyle(.checkbox)
                            if uart.audioTransport != .read && uart.notificationLimit > uart.audioPacketOverhead {
                                Text("\(uart.notificationLimit) bytes per notification · \(uart.notificationLimit - uart.audioPacketOverhead) bytes of audio")
                                    .font(.caption)
                                    .foregroundStyle(.secondary)
                            }
                        }
                        .disabled(uart.isSendingAudio)
                        .padding(.top, 8)
                    }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
                .padding(22)
                .fileImporter(isPresented: $choosingAudio, allowedContentTypes: [.audio]) {
                    uart.chooseAudio($0, quality: quality, playOnReceive: playOnReceive)
                }
                .tabItem { Text("BLE") }
                .tag(Tab.ble)
                ScrollView {
                    ClassicMediaView(discoveryHint: uart.classicDiscoveryHint, destination: .youtubeAudio)
                }
                .tabItem { Text("YT Music") }
                .tag(Tab.youtube)
                ScrollView {
                    ClassicMediaView(discoveryHint: uart.classicDiscoveryHint, destination: .youtubeStreamer)
                }
                .tabItem { Text("YT Video") }
                .tag(Tab.youtubeVideo)
            }
            .onChange(of: selectedTab, initial: true) { _, tab in
                uart.setClassicDiscovery(tab != .ble)
            }
            .frame(width: 490, height: 440)
        }
        .windowResizability(.contentSize)
    }
}
