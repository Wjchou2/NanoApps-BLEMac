import Foundation
import ImageIO
import IOBluetooth
import SwiftUI
import UniformTypeIdentifiers

enum ClassicIdentity {
    static var localAddress: String? {
        normalize(IOBluetoothHostController.default()?.addressAsString())
    }

    static func normalize(_ address: String?) -> String? {
        guard let address else { return nil }
        let normalized = address.trimmingCharacters(in: .whitespacesAndNewlines)
            .replacingOccurrences(of: "-", with: ":").uppercased()
        let parts = normalized.split(separator: ":", omittingEmptySubsequences: false)
        guard parts.count == 6,
              parts.allSatisfy({ $0.utf8.count == 2 && $0.utf8.allSatisfy {
                  (48...57).contains($0) || (65...70).contains($0)
              } }),
              !["00:00:00:00:00:00", "FF:FF:FF:FF:FF:FF", "02:00:00:00:00:00"].contains(normalized)
        else { return nil }
        return normalized
    }
}

private struct ClassicDeviceChoice: Identifiable {
    let address: String
    let name: String?

    var id: String { address }
    var title: String { name.map { "\($0) (\(address))" } ?? address }
}

enum ClassicDestination: String {
    case youtubeAudio = "YT Music"
    case youtubeStreamer = "YT Video"

    var sendsVideo: Bool { self == .youtubeStreamer }

    var psm: BluetoothL2CAPPSM {
        switch self {
        case .youtubeAudio: 0x1009
        case .youtubeStreamer: 0x100B
        }
    }
}

private enum VideoPlayback: String, CaseIterable {
    case afterTransfer = "After Transfer"
    case whileSending = "While Sending"
    case receiveTest = "Receive Test"

    static let playbackOptions: [Self] = [.afterTransfer, .whileSending]
}

private enum YouTubeAudio {
    private static let imaBlockBytes = 1024

    static func encodedBytesPerSecond(quality: ClassicQuality, adpcm: Bool) -> Int {
        let rate = Int(quality.sampleRate)
        let channels = Int(quality.channels)
        if adpcm {
            let samples = (imaBlockBytes - 4 * channels) * 2 / channels + 1
            return (rate * imaBlockBytes + samples - 1) / samples
        }
        return rate * channels
    }

    struct DownloadError: LocalizedError {
        let message: String
        var errorDescription: String? { message }
    }

    struct Song: Decodable {
        let id: String
        let title: String?
        var artist: String?
        let uploader: String?
        let channel: String?
        let thumbnail: URL?
        let thumbnails: [Thumbnail]?
        struct Thumbnail: Decodable { let url: URL }
        var videoURL: String { "https://www.youtube.com/watch?v=\(id)" }
        var url: String { "https://music.youtube.com/watch?v=\(id)" }
    }

    static func tracks(_ query: String, in folder: URL) async throws -> [Song] {
        async let songSearch = try? search(query, section: "songs", in: folder)
        async let videoSearch = try? search(query, section: "videos", in: folder)
        let (songs, videos) = await (songSearch, videoSearch)
        try Task.checkCancellation()
        if songs == nil && videos == nil {
            throw DownloadError(message: "YouTube Music search failed. Check the connection and yt-dlp.")
        }
        let ranked = (0..<8).flatMap { index in
            [songs, videos].compactMap { list in
                list.flatMap { index < $0.count ? $0[index] : nil }
            }
        }
        var seen = Set<String>()
        let tracks = Array(ranked.filter { seen.insert($0.id).inserted }.prefix(8))
        guard !tracks.isEmpty else {
            throw DownloadError(message: "No tracks found on YouTube Music.")
        }
        return await withAttributions(tracks, useArtist: true)
    }

    static func videos(_ query: String, in folder: URL) async throws -> [Song] {
        let results = try await search("ytsearch8:\(query)", in: folder, outputName: "videos")
        guard !results.isEmpty else { throw DownloadError(message: "No videos found on YouTube.") }
        return await withAttributions(results, useArtist: false)
    }

    private static func withAttributions(_ tracks: [Song], useArtist: Bool) async -> [Song] {
        await withTaskGroup(of: (Int, Song).self) { group in
            for (index, track) in tracks.enumerated() {
                group.addTask { (index, await withArtist(track, useArtist: useArtist)) }
            }
            var results = tracks
            for await (index, track) in group { results[index] = track }
            return results
        }
    }

    private static func withArtist(_ track: Song, useArtist: Bool) async -> Song {
        var track = track
        var name = (useArtist ? [track.artist, track.channel, track.uploader] : [track.channel, track.uploader])
            .compactMap { $0 }.first { !$0.isEmpty }
        if name?.isEmpty != false {
            var endpoint = URLComponents(string: "https://www.youtube.com/oembed")
            endpoint?.queryItems = [URLQueryItem(name: "url", value: "https://www.youtube.com/watch?v=\(track.id)"),
                                    URLQueryItem(name: "format", value: "json")]
            if let url = endpoint?.url,
               let (data, response) = try? await URLSession.shared.data(for: URLRequest(url: url, timeoutInterval: 4)),
               (response as? HTTPURLResponse)?.statusCode == 200,
               let metadata = try? JSONDecoder().decode(TrackAttribution.self, from: data) {
                name = metadata.author_name
            }
        }
        if useArtist, let name, name.hasSuffix(" - Topic") {
            track.artist = String(name.dropLast(" - Topic".count))
        } else {
            track.artist = name
        }
        return track
    }

    private struct TrackAttribution: Decodable { let author_name: String }

    private static func search(_ query: String, section: String, in folder: URL) async throws -> [Song] {
        var endpoint = URLComponents()
        endpoint.scheme = "https"
        endpoint.host = "music.youtube.com"
        endpoint.path = "/search"
        endpoint.queryItems = [URLQueryItem(name: "q", value: query)]
        endpoint.percentEncodedQuery = endpoint.percentEncodedQuery?.replacingOccurrences(of: "+", with: "%2B")
        endpoint.fragment = section
        guard let url = endpoint.url else { throw DownloadError(message: "Invalid search.") }
        return try await search(url.absoluteString, in: folder, outputName: section)
    }

    private static func search(_ source: String, in folder: URL, outputName: String) async throws -> [Song] {
        let output = folder.appendingPathComponent("\(outputName).json")
        try await runDownloader(["--flat-playlist", "--playlist-items", "1:8", "--dump-json", "--",
                                 source], in: folder, stdout: output)
        let lines = try String(contentsOf: output, encoding: .utf8).split(whereSeparator: \.isNewline)
        let decoder = JSONDecoder()
        var seen = Set<String>()
        let songs = lines.compactMap { line -> Song? in
            guard let song = try? decoder.decode(Song.self, from: Data(line.utf8)),
                  let title = song.title, !title.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
                  song.id.utf8.count == 11,
                  song.id.utf8.allSatisfy({ (48...57).contains($0) || (65...90).contains($0) ||
                                           (97...122).contains($0) || $0 == 45 || $0 == 95 }),
                  seen.insert(song.id).inserted else { return nil }
            return song
        }
        return Array(songs.prefix(8))
    }

    private static func videoURL(_ input: String) throws -> URL {
        let text = input.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let source = URL(string: text.contains("://") ? text : "https://" + text),
              source.scheme == "https" || source.scheme == "http",
              let host = source.host?.lowercased(),
              host == "youtu.be" || host == "youtube.com" || host.hasSuffix(".youtube.com")
        else { throw DownloadError(message: "Enter a YouTube video link.") }
        return source
    }

    static func download(_ input: String, into folder: URL, artwork: Bool = false) async throws -> URL {
        let source = try videoURL(input)
        let output = folder.appendingPathComponent("YouTube.%(ext)s").path
        let args = ["--no-playlist", "-f", "bestaudio[ext=m4a]/bestaudio", "-x", "--audio-format", "m4a"]
        try await runDownloader(args + (artwork ? ["--write-thumbnail"] : []) +
                                ["-o", output, "--", source.absoluteString], in: folder)
        let audio = folder.appendingPathComponent("YouTube.m4a")
        guard FileManager.default.fileExists(atPath: audio.path) else {
            throw DownloadError(message: "Audio download failed.")
        }
        return audio
    }

    static func downloadVideo(_ input: String, into folder: URL) async throws -> URL {
        let source = try videoURL(input)
        let output = folder.appendingPathComponent("YouTube.%(ext)s").path
        let formats = "bv[height<=480][vcodec^=avc1][ext=mp4]+ba[ext=m4a]/b[height<=480][vcodec^=avc1][ext=mp4]"
        try await runDownloader(["--no-playlist", "-f", formats, "--merge-output-format", "mp4",
                                 "-o", output, "--", source.absoluteString], in: folder)
        let video = folder.appendingPathComponent("YouTube.mp4")
        guard FileManager.default.fileExists(atPath: video.path) else {
            throw DownloadError(message: "Could not prepare an MP4 from this YouTube link.")
        }
        return video
    }

    private static func runDownloader(_ args: [String], in folder: URL, stdout: URL? = nil) async throws {
        let fm = FileManager.default
        let userBins = (try? fm.contentsOfDirectory(
            at: fm.homeDirectoryForCurrentUser.appendingPathComponent("Library/Python"),
            includingPropertiesForKeys: nil)) ?? []
        let project = Bundle.main.bundleURL.deletingLastPathComponent().deletingLastPathComponent()
        let localDownloader = project.appendingPathComponent("build/youtube-tools/bin/yt-dlp").path
        let candidates = ["/opt/homebrew/bin/yt-dlp", "/usr/local/bin/yt-dlp", localDownloader,
                          fm.homeDirectoryForCurrentUser.appendingPathComponent(".local/bin/yt-dlp").path]
            + userBins.map { $0.appendingPathComponent("bin/yt-dlp").path }
        guard let binary = candidates.first(where: fm.isExecutableFile(atPath:)) else {
            throw DownloadError(message: "Install yt-dlp, FFmpeg, and Deno with brew install yt-dlp ffmpeg deno.")
        }
        let log = (stdout ?? folder.appendingPathComponent("yt-dlp")).appendingPathExtension("log")
        fm.createFile(atPath: log.path, contents: nil)
        let output = try FileHandle(forWritingTo: log)
        let results = try stdout.map { url in
            fm.createFile(atPath: url.path, contents: nil)
            return try FileHandle(forWritingTo: url)
        }
        defer { try? output.close() }
        defer { try? results?.close() }
        let process = Process()
        process.executableURL = URL(fileURLWithPath: binary)
        var env = ProcessInfo.processInfo.environment
        let nodeRoot = fm.homeDirectoryForCurrentUser.appendingPathComponent(".nvm/versions/node")
        let nodeBins = ((try? fm.contentsOfDirectory(at: nodeRoot, includingPropertiesForKeys: nil)) ?? [])
            .sorted { $0.lastPathComponent.compare($1.lastPathComponent, options: .numeric) == .orderedDescending }
            .map { $0.appendingPathComponent("bin").path }
        let path = ["/opt/homebrew/bin", "/usr/local/bin"] + nodeBins + [env["PATH"] ?? "/usr/bin:/bin"]
        env["PATH"] = path.joined(separator: ":")
        process.environment = env
        process.arguments = ["--ignore-config", "--no-progress", "--no-warnings",
                             "--js-runtimes", "node", "--socket-timeout", "15", "--extractor-retries", "1"] + args
        process.standardOutput = results ?? FileHandle.nullDevice
        process.standardError = output
        try process.run()
        defer { if process.isRunning { process.terminate() } }
        while process.isRunning { try await Task.sleep(for: .milliseconds(150)) }
        try Task.checkCancellation()
        guard process.terminationStatus == 0 else {
            let details = (try? String(contentsOf: log, encoding: .utf8))?
                .split(whereSeparator: \.isNewline).last.map(String.init) ?? "YouTube request failed."
            throw DownloadError(message: String(details.prefix(230)))
        }
    }

    static func coverPixels(in folder: URL) -> Data? {
        guard let files = try? FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil),
              let source = files.lazy.filter({ $0.deletingPathExtension().lastPathComponent == "YouTube" &&
                  ["jpg", "jpeg", "png", "webp", "avif"].contains($0.pathExtension.lowercased()) })
                .compactMap({ CGImageSourceCreateWithURL($0 as CFURL, nil) }).first else { return nil }
        return artworkPixels(source, side: 160, fill: true)
    }

    static func resultArtwork(_ result: Song, video: Bool, side: Int = 96) async -> Data? {
        let fallback = URL(string: "https://i.ytimg.com/vi/\(result.id)/mqdefault.jpg")
        let high = URL(string: "https://i.ytimg.com/vi/\(result.id)/hqdefault.jpg")
        let square = result.thumbnails?.first { thumbnail in
            thumbnail.url.host?.hasSuffix(".googleusercontent.com") == true ||
                thumbnail.url.host?.hasSuffix(".ggpht.com") == true
        }?.url
        let urls = video ? [result.thumbnail, fallback, high, result.thumbnails?.last?.url] :
                           [square, result.thumbnail, result.thumbnails?.last?.url, fallback, high]
        var tried = Set<URL>()
        for url in urls.compactMap({ $0 }) where url.scheme == "https" && tried.insert(url).inserted {
            guard !Task.isCancelled,
                  let (bytes, response) = try? await URLSession.shared.data(for: URLRequest(url: url, timeoutInterval: 4)),
                  (response as? HTTPURLResponse)?.statusCode == 200,
                  let source = CGImageSourceCreateWithData(bytes as CFData, nil),
                  let pixels = artworkPixels(source, side: side, fill: !video) else { continue }
            return pixels
        }
        return nil
    }

    private static func artworkPixels(_ source: CGImageSource, side: Int, fill: Bool) -> Data? {
        guard let image = CGImageSourceCreateThumbnailAtIndex(source, 0, [
                  kCGImageSourceCreateThumbnailFromImageAlways: true,
                  kCGImageSourceCreateThumbnailWithTransform: true,
                  kCGImageSourceThumbnailMaxPixelSize: side * 3
              ] as CFDictionary),
              let context = CGContext(data: nil, width: side, height: side, bitsPerComponent: 8,
                                      bytesPerRow: side * 4, space: CGColorSpaceCreateDeviceRGB(),
                                      bitmapInfo: CGBitmapInfo.byteOrder32Big.rawValue |
                                          CGImageAlphaInfo.premultipliedLast.rawValue),
              let rgba = context.data?.assumingMemoryBound(to: UInt8.self) else { return nil }
        let dimension = fill ? min(image.width, image.height) : max(image.width, image.height)
        let scale = CGFloat(side) / CGFloat(dimension)
        let width = CGFloat(image.width) * scale
        let height = CGFloat(image.height) * scale
        context.interpolationQuality = .high
        context.draw(image, in: CGRect(x: (CGFloat(side) - width) / 2, y: (CGFloat(side) - height) / 2,
                                      width: width, height: height))
        var cover = Data(count: side * side * 2)
        cover.withUnsafeMutableBytes { (buffer: UnsafeMutableRawBufferPointer) in
            for pixel in 0..<(side * side) {
                let i = pixel * 4
                let rgb565 = (UInt16(rgba[i] >> 3) << 11) | (UInt16(rgba[i + 1] >> 2) << 5) |
                    UInt16(rgba[i + 2] >> 3)
                buffer[pixel * 2] = UInt8(truncatingIfNeeded: rgb565)
                buffer[pixel * 2 + 1] = UInt8(rgb565 >> 8)
            }
        }
        return cover
    }

    static func trackWAV(from input: URL, in folder: URL, quality: ClassicQuality,
                         adpcm: Bool) async throws -> Data {
        let output = folder.appendingPathComponent("Track.wav")
        let process = Process()
        process.executableURL = try HardwareVideo.encoderURL()
        let codec = adpcm ? ["-c:a", "adpcm_ima_wav", "-block_size", String(imaBlockBytes)] :
                            ["-c:a", "pcm_mulaw"]
        process.arguments = ["-v", "error", "-nostdin", "-i", input.path, "-map", "0:a:0", "-vn",
                             "-ac", String(quality.channels), "-ar", String(quality.sampleRate)] + codec +
                            ["-rf64", "never", "-fflags", "+bitexact", "-flags:a", "+bitexact", output.path]
        process.standardOutput = FileHandle.nullDevice
        process.standardError = FileHandle.nullDevice
        try Task.checkCancellation()
        try process.run()
        defer { if process.isRunning { process.terminate() } }
        while process.isRunning { try await Task.sleep(for: .milliseconds(100)) }
        try Task.checkCancellation()
        guard process.terminationStatus == 0 else {
            throw DownloadError(message: "Could not convert the track to WAV.")
        }
        let wav = try Data(contentsOf: output, options: .alwaysMapped)
        guard wav.count <= Int(UInt32.max) else {
            throw DownloadError(message: "This track exceeds the 4 GiB WAV limit at this quality.")
        }
        let headerBytes = adpcm ? 60 : 58
        guard wav.count > headerBytes, wav.withUnsafeBytes({ (header: UnsafeRawBufferPointer) in
            let length = Int(header.loadUnaligned(fromByteOffset: headerBytes - 4, as: UInt32.self).littleEndian)
            guard header.loadUnaligned(fromByteOffset: 4, as: UInt32.self).littleEndian == UInt32(wav.count - 8),
                  length > 0, wav.count - headerBytes == length + (length & 1) else { return false }
            if adpcm {
                let channels = Int(quality.channels)
                let samples = (imaBlockBytes - 4 * channels) * 2 / channels + 1
                let frames = Int(header.loadUnaligned(fromByteOffset: 48, as: UInt32.self).littleEndian)
                return header[20] == 0x11 && frames > 0 &&
                    (frames + samples - 1) / samples * imaBlockBytes == length
            }
            return header[20] == 7
        }) else {
            throw DownloadError(message: "Could not prepare a compatible WAV.")
        }
        return wav
    }
}

private final class ClassicFileChannelDelegate: NSObject, IOBluetoothL2CAPChannelDelegate {
    private weak var sender: ClassicSender?

    init(sender: ClassicSender) {
        self.sender = sender
    }

    func l2capChannelOpenComplete(_ channel: IOBluetoothL2CAPChannel!, status: IOReturn) {
        guard let channel else { return }
        sender?.fileChannelOpened(channel, status: status, delegate: self)
    }

    func l2capChannelClosed(_ channel: IOBluetoothL2CAPChannel!) {
        guard let channel else { return }
        sender?.fileChannelClosed(channel, delegate: self)
    }

    func l2capChannelData(_ channel: IOBluetoothL2CAPChannel!,
                          data: UnsafeMutableRawPointer!, length: Int) {
        // Artwork negotiation and video playback controls are four-byte messages.
        guard let channel, let data, (4...60).contains(length) else { return }
        sender?.fileChannelReceived(channel, data: Data(bytes: data, count: length), delegate: self)
    }

    func l2capChannelWriteComplete(_ channel: IOBluetoothL2CAPChannel!,
                                   refcon: UnsafeMutableRawPointer!, status: IOReturn) {
        guard let channel else { return }
        sender?.fileChannelWriteComplete(channel, status: status)
    }
}

private final class ClassicSender: NSObject, ObservableObject, IOBluetoothDeviceInquiryDelegate {
    private static let headerSize = 60
    private static let pacedAudioPacketSize = 336
    private static let pacedAudioBytesPerSecond = 40 * 1024
    private static let pacedStereoBytesPerSecond = 48 * 1024
    private static let pairingTimeout: TimeInterval = 45
    private var channel: IOBluetoothL2CAPChannel?
    private var channelDelegate: ClassicFileChannelDelegate?
    private var channelOpened = false
    private var pairingDevice: IOBluetoothDevice?
    private var pairingTimer: Timer?
    private var file: Data?
    private struct PendingWrite {
        let buffer: NSData
        let payloadBytes: Int
        let searchResponse: Bool
    }
    private var pending: PendingWrite?
    private var submitted = 0
    private var pumping = false
    private var retryScheduled = false
    private var retryCount = 0
    private var sent = 0
    private var percentSent = 0
    private var headerSent = false
    private var destination = ClassicDestination.youtubeAudio
    private var transferCover: Data?
    private var audioTransferToken: UInt32 = 0
    private var liveAudioBytesPerSecond = 0
    private var audioSendPaused = false
    private var testingAudioReceive = false
    private var macPacedAudio = false
    private var pacedAudioPackets = 0
    private var pacedAudioStartedAt: TimeInterval = 0
    private var pacedAudioEpoch: TimeInterval?
    private var progressiveVideo = false
    private var videoBytesSent = 0
    private var videoCreditLimit = 0
    private var videoCreditWaitSince: TimeInterval?
    private var chunkWaiter: CheckedContinuation<Void, Error>?
    private var transferError: String?
    private var videoTask: Task<Void, Never>?
    var youtubeSearchQuality = ClassicQuality.low
    private var youtubeResults: [YouTubeAudio.Song] = []
    private var activeVideoSelection: String?
    private var supportsResultArtwork = false
    @Published private var youtubeAudioProtocol = 0
    private var supportsVideoProgress = false
    private var supportsVideoADPCM = false
    private var youtubeVideoReady = false
    private var videoPausedAt: TimeInterval?
    @Published private(set) var supportsNativeVideo = false
    @Published private(set) var supportsAllNativeRates = false
    @Published private(set) var supportsIntermediateVideo = false
    @Published private(set) var supports22FPS = false
    var youtubeAudioPlayback = VideoPlayback.whileSending
    var youtubeVideoPlayback = VideoPlayback.afterTransfer
    var youtubeVideoQuality = HardwareVideo.Quality.medium
    var youtubeFrameRate = HardwareVideo.FrameRate.fps15
    var youtubeVideoAudioQuality = ClassicQuality.medium
    var youtubeConvertDuringTransfer = false
    private var inquiry: IOBluetoothDeviceInquiry?
    private static let rememberedAddressKey = "classic.lastNanoAddress"

    @Published private(set) var connectedOrOpening = false
    @Published private(set) var canSend = false
    @Published private(set) var videoProgress: Int?
    @Published private(set) var isConvertingVideo = false
    @Published private(set) var isPreparingAudio = false
    @Published private(set) var audioProgress: Int?
    @Published private(set) var videoStage = ""
    @Published private(set) var devices: [ClassicDeviceChoice] = []
    @Published private(set) var isSearching = false
    @Published private(set) var discoveryStatus = ""
    @Published private(set) var status = "Open the matching app on the nano, then click Connect."
    var isPairing: Bool { pairingDevice != nil }

    func youtubeTrackUsesADPCM(_ quality: ClassicQuality, playback: VideoPlayback) -> Bool {
        youtubeAudioProtocol >= 5 ||
            (youtubeAudioProtocol >= 4 && playback == .whileSending && quality.sampleRate == 44_100)
    }

    static var rememberedAddress: String? {
        UserDefaults.standard.string(forKey: rememberedAddressKey)
    }

    func refreshDevices() {
        for device in IOBluetoothDevice.pairedDevices() as? [IOBluetoothDevice] ?? [] {
            addDevice(device)
        }
        if let address = Self.rememberedAddress, !devices.contains(where: { $0.address == address }) {
            devices.insert(ClassicDeviceChoice(address: address, name: "Saved device"), at: 0)
        }
    }

    private func addDevice(_ device: IOBluetoothDevice) {
        guard let address = ClassicIdentity.normalize(device.addressString) else { return }
        let name = device.name?.isEmpty == false ? device.name : nil
        if let index = devices.firstIndex(where: { $0.address == address }) {
            if let name, devices[index].name != name {
                devices[index] = ClassicDeviceChoice(address: address, name: name)
            }
        } else {
            devices.append(ClassicDeviceChoice(address: address, name: name))
        }
    }

    private func remember(_ device: IOBluetoothDevice?) {
        guard let device, let address = ClassicIdentity.normalize(device.addressString) else { return }
        UserDefaults.standard.set(address, forKey: Self.rememberedAddressKey)
        addDevice(device)
    }

    func findDevices() {
        guard !isSearching, !connectedOrOpening else { return }
        devices.removeAll()
        refreshDevices()
        guard let search = IOBluetoothDeviceInquiry(delegate: self) else {
            discoveryStatus = "Bluetooth search is unavailable."
            return
        }
        search.inquiryLength = 8
        search.updateNewDeviceNames = true
        inquiry = search
        isSearching = true
        discoveryStatus = "Searching for nearby Classic devices…"
        let result = search.start()
        if result != kIOReturnSuccess {
            stopDiscovery()
            discoveryStatus = "Could not start Bluetooth search (\(result))."
        }
    }

    func stopDiscovery() {
        guard let search = inquiry else { return }
        inquiry = nil
        isSearching = false
        discoveryStatus = ""
        search.delegate = nil
        search.stop()
    }

    func deviceInquiryDeviceFound(_ sender: IOBluetoothDeviceInquiry!, device: IOBluetoothDevice!) {
        guard let inquiry, sender === inquiry, let device else { return }
        addDevice(device)
    }

    func deviceInquiryComplete(_ sender: IOBluetoothDeviceInquiry!, error: IOReturn, aborted: Bool) {
        guard let inquiry, sender === inquiry else { return }
        for device in sender.foundDevices() as? [IOBluetoothDevice] ?? [] {
            addDevice(device)
        }
        self.inquiry = nil
        isSearching = false
        if error != kIOReturnSuccess {
            discoveryStatus = "Bluetooth search ended (\(error))."
        } else {
            discoveryStatus = devices.isEmpty ? "No devices found. You can enter the nano’s address." : ""
        }
    }

    func connect(address: String, pairOnly: Bool, destination: ClassicDestination) {
        guard let normalized = ClassicIdentity.normalize(address),
              let device = IOBluetoothDevice(addressString: normalized) else {
            status = "Enter the nano’s Classic Bluetooth address."
            return
        }

        stopDiscovery()
        self.destination = destination
        let listenInstruction = "Open \(destination.rawValue) on the nano"
        if device.isPaired() {
            if pairOnly {
                remember(device)
                status = "Already paired. \(listenInstruction), then click Connect."
            } else {
                connectedOrOpening = true
                openChannel(to: device)
            }
            return
        }
        guard pairOnly else {
            status = "\(listenInstruction), then choose Pair in Device Setup."
            return
        }
        startPairing(with: device)
    }

    private func openChannel(to device: IOBluetoothDevice) {
        var newChannel: IOBluetoothL2CAPChannel?
        let delegate = ClassicFileChannelDelegate(sender: self)
        channelDelegate = delegate
        transferError = nil
        status = "Connecting to the nano…"
        let result = device.openL2CAPChannelAsync(&newChannel, withPSM: destination.psm, delegate: delegate)
        guard channelDelegate === delegate, !channelOpened else { return }
        guard result == kIOReturnSuccess, let newChannel else {
            clearFileChannel()
            status = "Could not start the Classic connection (\(result))."
            return
        }
        channel = newChannel
    }

    func stop() {
        if isPairing {
            finishPairing(with: "Stopped waiting. You can cancel any open macOS pairing prompt.")
            return
        }
        canSend = false
        videoProgress = nil
        videoTask?.cancel()
        videoTask = nil
        finishVideoChunk(CancellationError())
        progressiveVideo = false
        isConvertingVideo = false
        isPreparingAudio = false
        audioProgress = nil
        file = nil
        transferCover = nil
        status = "Disconnecting…"
        if let channel {
            if !channelOpened {
                clearFileChannel()
                status = "Classic connection cancelled."
            }
            let result = channel.close()
            if result != kIOReturnSuccess {
                status = "Could not close the Classic connection (\(result))."
            }
        }
    }

    private func finishPairing(with message: String) {
        pairingTimer?.invalidate()
        pairingTimer = nil
        pairingDevice = nil
        connectedOrOpening = false
        status = message
    }

    private func startPairing(with device: IOBluetoothDevice) {
        pairingDevice = device
        connectedOrOpening = true
        status = "Confirm the matching code in the macOS pairing dialog and on the nano."
        let deadline = ProcessInfo.processInfo.systemUptime + Self.pairingTimeout
        let timer = Timer(timeInterval: 0.5, repeats: true) { [weak self] timer in
            guard let self, self.pairingTimer === timer else { return }
            if device.isPaired() {
                self.remember(device)
                self.finishPairing(with: "Paired. Click Connect.")
            } else if ProcessInfo.processInfo.systemUptime >= deadline {
                self.finishPairing(with: "Pairing timed out. Keep \(self.destination.rawValue) open on the nano and try Pair again in Device Setup.")
            }
        }
        pairingTimer = timer
        RunLoop.main.add(timer, forMode: .common)

        // Let macOS own the pairing dialog. The legacy pairing object's
        // confirmation method is a no-op on this Mac.
        DispatchQueue.global(qos: .userInitiated).async { [weak self, weak timer] in
            let result = device.isConnected()
                ? device.requestAuthentication()
                : device.openConnection(nil, withPageTimeout: 0x2000, authenticationRequired: true)
            DispatchQueue.main.async { [weak self, weak timer] in
                guard let self, let timer, self.pairingTimer === timer else { return }
                if device.isPaired() {
                    self.remember(device)
                    self.finishPairing(with: "Paired. Click Connect.")
                } else if result != kIOReturnSuccess && !device.isConnected() {
                    self.finishPairing(with: "Could not start the authenticated Classic connection (\(result)).")
                }
            }
        }
    }

    func fileChannelOpened(_ opened: IOBluetoothL2CAPChannel, status result: IOReturn,
                           delegate: ClassicFileChannelDelegate) {
        guard channelDelegate === delegate else {
            if result == kIOReturnSuccess { opened.close() }
            return
        }
        guard result == kIOReturnSuccess else {
            clearFileChannel()
            status = "Could not open the Classic file channel (\(result))."
            return
        }
        // With a new baseband connection macOS can return a placeholder,
        // then deliver the usable channel as a different object here.
        channel = opened
        channelOpened = true
        canSend = true
        let device = opened.device
        remember(device)
        status = "Connected to \(device?.name ?? device?.addressString ?? "Bluetooth device")."
    }

    private func clearFileChannel() {
        youtubeResults = []
        activeVideoSelection = nil
        supportsResultArtwork = false
        youtubeAudioProtocol = 0
        audioSendPaused = false
        supportsVideoProgress = false
        supportsVideoADPCM = false
        supportsNativeVideo = false
        supportsAllNativeRates = false
        supportsIntermediateVideo = false
        supports22FPS = false
        youtubeVideoReady = false
        videoPausedAt = nil
        videoTask?.cancel()
        videoTask = nil
        finishVideoChunk(CancellationError())
        progressiveVideo = false
        isConvertingVideo = false
        isPreparingAudio = false
        audioProgress = nil
        channel = nil
        channelDelegate = nil
        channelOpened = false
        file = nil
        transferCover = nil
        pending = nil
        retryScheduled = false
        canSend = false
        videoProgress = nil
        connectedOrOpening = false
    }

    func fileChannelClosed(_ closed: IOBluetoothL2CAPChannel, delegate: ClassicFileChannelDelegate) {
        guard channelDelegate === delegate, closed === channel else { return }
        let progress = macPacedAudio && file != nil ? pacedAudioInterruption("disconnected") : nil
        _ = closed.setDelegate(nil)
        clearFileChannel()
        status = transferError ?? progress ?? "Classic device disconnected."
        transferError = nil
    }

    private func pacedAudioInterruption(_ reason: String) -> String {
        let label = testingAudioReceive ? "Receive Test" :
            (liveAudioBytesPerSecond > 0 ? "While Sending" : "After Transfer")
        let elapsed = Int(ProcessInfo.processInfo.systemUptime - pacedAudioStartedAt)
        let audioBytes = max(0, sent - (transferCover?.count ?? 0))
        return "\(label) \(reason) at \(percentSent)% · \(audioBytes) audio bytes · \(pacedAudioPackets) packets · \(elapsed)s."
    }

    func sendAudioFile(_ result: Result<URL, Error>, quality: ClassicQuality, playback: VideoPlayback) {
        guard canSend, let channel, destination == .youtubeAudio else { return }
        do {
            let url = try result.get()
            prepareStreamAudio(quality: quality, playback: playback, channel: channel) { _ in url }
        } catch {
            status = error.localizedDescription
        }
    }

    func sendVideoFile(_ result: Result<URL, Error>, playback: VideoPlayback,
                       quality: HardwareVideo.Quality, frameRate: HardwareVideo.FrameRate,
                       audioQuality: ClassicQuality, convertDuringTransfer: Bool) {
        guard canSend, let channel, destination == .youtubeStreamer else { return }
        do {
            let url = try result.get()
            prepareVideo(url, playback: playback, quality: quality, frameRate: frameRate,
                         audioQuality: audioQuality, convertDuringTransfer: convertDuringTransfer, channel: channel)
        } catch {
            status = error.localizedDescription
        }
    }

    func fileChannelReceived(_ channel: IOBluetoothL2CAPChannel, data: Data,
                             delegate: ClassicFileChannelDelegate) {
        guard channelDelegate === delegate else { return }
        if destination.sendsVideo, data == Data("NVQ6".utf8) {
            supportsAllNativeRates = true
            supportsNativeVideo = true
            supportsIntermediateVideo = true
            supports22FPS = true
            supportsVideoADPCM = true
            supportsResultArtwork = destination == .youtubeStreamer
            return
        }
        if destination.sendsVideo, data == Data("NVQ2".utf8) ||
            data == Data("NVQ3".utf8) || data == Data("NVQ4".utf8) || data == Data("NVQ5".utf8) {
            supportsNativeVideo = true
            if data != Data("NVQ2".utf8) { supportsIntermediateVideo = true }
            if data == Data("NVQ4".utf8) || data == Data("NVQ5".utf8) { supports22FPS = true }
            if data == Data("NVQ5".utf8) { supportsVideoADPCM = true }
            if destination == .youtubeStreamer { supportsResultArtwork = true }
            return
        }
        if destination.sendsVideo, data == Data("NVP1".utf8) {
            supportsVideoProgress = true
            if destination == .youtubeStreamer { supportsResultArtwork = true }
            return
        }
        if let currentChannel = self.channel, progressiveVideo, data.count == 8,
           data.prefix(4) == Data("NYG1".utf8) {
            guard currentChannel === channel ||
                  (currentChannel.localChannelID == channel.localChannelID &&
                   currentChannel.remoteChannelID == channel.remoteChannelID) else { return }
            let limit = data.withUnsafeBytes { Int($0.loadUnaligned(fromByteOffset: 4, as: UInt32.self).littleEndian) }
            guard limit >= videoCreditLimit else { return }
            if limit > videoCreditLimit {
                videoCreditLimit = limit
                videoCreditWaitSince = nil
            }
            pump(currentChannel)
            return
        }
        if data == Data("NYH1".utf8) {
            supportsResultArtwork = true
            return
        }
        guard let currentChannel = self.channel else { return }
        if currentChannel !== channel {
            guard currentChannel.localChannelID == channel.localChannelID,
                  currentChannel.remoteChannelID == channel.remoteChannelID else { return }
        }
        if destination == .youtubeAudio, data.count == 4, data.prefix(3) == Data("NYL".utf8),
           (0x33...0x35).contains(data[3]) {
            let revision = Int(data[3] - 0x30)
            if revision > youtubeAudioProtocol { youtubeAudioProtocol = revision }
            supportsResultArtwork = true
            return
        }
        if destination == .youtubeAudio, data.count == 8,
           data.prefix(4) == Data("NYZ2".utf8) || data.prefix(4) == Data("NYZ3".utf8) {
            let token = data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 4, as: UInt32.self).littleEndian }
            guard file != nil, liveAudioBytesPerSecond > 0, token == audioTransferToken else { return }
            let paused = data[3] == 0x32
            guard paused != audioSendPaused else { return }
            audioSendPaused = paused
            if paused {
                status = "Waiting for the nano to resume audio…"
            } else {
                status = "Streaming audio… \(percentSent)%"
                pump(currentChannel)
            }
            return
        }
        if destination.sendsVideo {
            if progressiveVideo, data.prefix(4) == Data("NVF1".utf8), data.count > 4 {
                let detail = String(decoding: data.dropFirst(4).prefix { $0 != 0 }, as: UTF8.self)
                let message = "Nano: \(detail)"
                transferError = message
                stop()
                status = message
                return
            }
            if data == Data("NYZ1".utf8) {
                videoPausedAt = videoPausedAt ?? ProcessInfo.processInfo.systemUptime
                status = "Playback paused on the nano."
                return
            }
            if data == Data("NYZ0".utf8) {
                videoCreditWaitSince = nil
                videoPausedAt = nil
                status = "Sending video to the nano…"
                pump(currentChannel)
                return
            }
        }
        if destination == .youtubeStreamer, data == Data("NYV1".utf8) {
            youtubeVideoReady = true
            if file == nil && !isConvertingVideo {
                canSend = true
                status = "Ready for another video."
            }
            return
        }
        if data.count >= 15, data.prefix(4) == Data("NYP1".utf8),
           data.dropFirst(15).allSatisfy({ $0 == 0 }) {
            let id = String(decoding: data[4..<15], as: UTF8.self)
            if destination == .youtubeStreamer, activeVideoSelection == id {
                if !headerSent && pending == nil {
                    sendSearchStatus("Preparing selected video...", channel: currentChannel, accepted: true)
                }
                return
            }
            guard canSend else { return }
            guard let song = youtubeResults.first(where: { $0.id == id }) else {
                sendSearchStatus("Search again to refresh the results.", channel: currentChannel)
                return
            }
            if destination == .youtubeStreamer {
                prepareVideo(nil, youtubeLink: song.videoURL, playback: youtubeVideoPlayback,
                             quality: youtubeVideoQuality, frameRate: youtubeFrameRate,
                             audioQuality: youtubeVideoAudioQuality,
                             convertDuringTransfer: youtubeConvertDuringTransfer,
                             channel: currentChannel, selectionID: id)
                return
            }
            prepareStreamAudio(quality: youtubeSearchQuality, playback: youtubeAudioPlayback,
                               channel: currentChannel, downloading: true, track: song) { folder in
                self.sendSearchStatus("Downloading selected song...", channel: currentChannel, accepted: true)
                self.status = "Downloading \(song.title ?? "song")…"
                return try await YouTubeAudio.download(song.url, into: folder, artwork: true)
            }
            return
        }
        guard canSend, data.prefix(4) == Data("NYQ1".utf8) else { return }
        if destination == .youtubeStreamer {
            supportsResultArtwork = true
        }
        let text = data.dropFirst(4).prefix { $0 != 0 }
        guard let query = String(data: text, encoding: .utf8), !query.isEmpty else {
            sendSearchStatus("Invalid search text.", channel: channel)
            return
        }
        searchYouTube(query)
    }

    func searchYouTube(_ input: String) {
        let query = input.trimmingCharacters(in: .whitespacesAndNewlines)
        guard canSend, let channel, !query.isEmpty else { return }
        let video = destination == .youtubeStreamer
        canSend = false
        audioProgress = nil
        isPreparingAudio = true
        let service = video ? "YouTube" : "YouTube Music"
        status = "Searching \(service) for \(query)…"
        sendSearchStatus("Searching \(service)...", channel: channel, accepted: true)
        videoTask = Task { @MainActor [weak self, weak channel] in
            guard let self, let channel else { return }
            let folder = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
            defer { try? FileManager.default.removeItem(at: folder) }
            do {
                try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
                let songs = try await (video ? YouTubeAudio.videos(query, in: folder) :
                                                YouTubeAudio.tracks(query, in: folder))
                try Task.checkCancellation()
                guard self.channel === channel else { return }
                let sendArtwork = self.supportsResultArtwork
                var artwork = [Data?](repeating: nil, count: songs.count)
                if sendArtwork {
                    self.status = "Getting result artwork…"
                    await withTaskGroup(of: (Int, Data?).self) { group in
                        for (index, song) in songs.enumerated() {
                            group.addTask { (index, await YouTubeAudio.resultArtwork(song, video: video)) }
                        }
                        for await (index, pixels) in group { artwork[index] = pixels }
                    }
                }
                var response = Data("NYL1".utf8)
                response.append(UInt8(songs.count))
                response.append(sendArtwork ? 1 : 0)
                response.append(Data(count: Self.headerSize - response.count))
                for song in songs {
                    var row = Data("NYR1".utf8)
                    row.append(contentsOf: song.id.utf8)
                    let title = song.title ?? ""
                    for character in title {
                        let bytes = String(character).utf8
                        if row.count + bytes.count >= Self.headerSize { break }
                        row.append(contentsOf: bytes)
                    }
                    row.append(Data(count: Self.headerSize - row.count))
                    if let artist = song.artist, !artist.isEmpty {
                        var attribution = Data("NYT1".utf8)
                        attribution.append(contentsOf: song.id.utf8)
                        for character in artist {
                            let bytes = String(character).utf8
                            if attribution.count + bytes.count >= Self.headerSize { break }
                            attribution.append(contentsOf: bytes)
                        }
                        attribution.append(Data(count: Self.headerSize - attribution.count))
                        response.append(attribution)
                    }
                    response.append(row)
                }
                if sendArtwork {
                    for (index, pixels) in artwork.enumerated() {
                        guard let pixels else { continue }
                        response.append(contentsOf: "NYI1".utf8)
                        response.append(UInt8(index))
                        response.append(Data(count: Self.headerSize - 5))
                        response.append(pixels)
                    }
                    response.append(contentsOf: "NYE1".utf8)
                    response.append(Data(count: Self.headerSize - 4))
                }
                let mtu = Int(channel.outgoingMTU)
                let packetSize = mtu > 0 ? mtu : Self.headerSize
                for start in stride(from: 0, to: response.count, by: packetSize) {
                    while self.channel === channel && self.pending != nil {
                        try await Task.sleep(for: .milliseconds(10))
                    }
                    try Task.checkCancellation()
                    guard self.channel === channel else { return }
                    let end = min(response.count, start + packetSize)
                    if end == response.count {
                        self.youtubeResults = songs
                        self.isPreparingAudio = false
                        self.videoTask = nil
                        self.canSend = true
                        self.status = video ? "Choose a video on the nano." : "Choose a song on the nano."
                    }
                    let result = self.enqueue(response.subdata(in: start..<end), payloadBytes: 0,
                                              to: channel, searchResponse: true)
                    guard result == kIOReturnSuccess else {
                        self.failSearchWrite(result)
                        return
                    }
                }
            } catch {
                guard self.channel === channel, !Task.isCancelled else { return }
                self.status = error.localizedDescription
                self.isPreparingAudio = false
                self.videoTask = nil
                self.canSend = true
                self.sendSearchStatus(self.status, channel: channel)
            }
        }
    }

    private func failSearchWrite(_ result: IOReturn) {
        let message = "Could not send search response (\(result)). Reconnect to try again."
        transferError = message
        stop()
        status = message
    }

    private func sendSearchStatus(_ message: String, channel: IOBluetoothL2CAPChannel, accepted: Bool = false) {
        guard pending == nil else { return }
        let ascii = message.unicodeScalars.map { $0.isASCII ? UInt8($0.value) : 32 }
        var packet = Data((accepted ? "NYA1" : "NYS1").utf8)
        packet.append(contentsOf: ascii.prefix(55))
        packet.append(Data(count: Self.headerSize - packet.count))
        let result = enqueue(packet, payloadBytes: 0, to: channel, searchResponse: true)
        if result != kIOReturnSuccess {
            failSearchWrite(result)
        }
    }

    private enum VideoPreparation: UInt8 {
        case downloading = 1, encoding, starting
    }

    private func reportVideoPreparation(_ stage: VideoPreparation, channel: IOBluetoothL2CAPChannel) {
        guard pending == nil, !headerSent else { return }
        if supportsVideoProgress {
            var packet = Data("NVT1".utf8)
            packet.append(stage.rawValue)
            packet.append(videoProgress.map(UInt8.init) ?? 255)
            packet.append(Data(count: Self.headerSize - packet.count))
            _ = enqueue(packet, payloadBytes: 0, to: channel, searchResponse: true)
        } else if destination == .youtubeStreamer {
            let status = videoProgress.map { "\(videoStage) \($0)%" } ?? videoStage
            sendSearchStatus(status, channel: channel, accepted: true)
        }
    }

    func sendYouTubeAudio(_ input: String, quality: ClassicQuality, playback: VideoPlayback) {
        guard canSend, let channel, destination == .youtubeAudio else { return }
        prepareStreamAudio(quality: quality, playback: playback, channel: channel, downloading: true) {
            try await YouTubeAudio.download(input, into: $0, artwork: true)
        }
    }

    private func prepareStreamAudio(quality: ClassicQuality, playback: VideoPlayback,
                                    channel: IOBluetoothL2CAPChannel, downloading: Bool = false,
                                    track: YouTubeAudio.Song? = nil,
                                    source: @escaping (URL) async throws -> URL) {
        if playback == .whileSending &&
            (youtubeAudioProtocol < 3 || (quality.sampleRate == 44_100 && youtubeAudioProtocol < 4)) {
            status = "Install the updated YT Music app for While Sending."
            sendSearchStatus(status, channel: channel)
            return
        }
        canSend = false
        isPreparingAudio = true
        audioProgress = nil
        let live = playback == .whileSending
        let adpcm = youtubeTrackUsesADPCM(quality, playback: playback)
        status = downloading ? "Downloading YouTube audio…" : "Preparing audio for the nano…"
        videoTask = Task { @MainActor [weak self, weak channel] in
            guard let self, let channel else { return }
            let folder = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
            defer {
                try? FileManager.default.removeItem(at: folder)
                if self.channel === channel {
                    self.isPreparingAudio = false
                    self.videoTask = nil
                }
            }
            do {
                try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
                let url = try await source(folder)
                let hasFileAccess = url.startAccessingSecurityScopedResource()
                defer { if hasFileAccess { url.stopAccessingSecurityScopedResource() } }
                self.status = "Preparing audio for the nano…"
                let conversion = Task.detached(priority: .userInitiated) {
                    let wav = try await YouTubeAudio.trackWAV(from: url, in: folder, quality: quality, adpcm: adpcm)
                    var cover: Data?
                    if downloading { cover = YouTubeAudio.coverPixels(in: folder) }
                    if cover == nil, let track {
                        cover = await YouTubeAudio.resultArtwork(track, video: false, side: 160)
                    }
                    let checksum = try Self.payloadChecksum(wav)
                    return (wav, cover, checksum)
                }
                let (bytes, cover, checksum) = try await withTaskCancellationHandler {
                    try await conversion.value
                } onCancel: {
                    conversion.cancel()
                }
                try Task.checkCancellation()
                guard self.channel === channel else { return }
                while self.channel === channel && self.pending != nil {
                    try await Task.sleep(for: .milliseconds(10))
                }
                guard self.channel === channel else { return }
                self.sendStreamAudio(bytes, from: url, playback: playback,
                                     channel: channel, cover: cover, checksum: checksum,
                                     liveAudioRate: live ? YouTubeAudio.encodedBytesPerSecond(quality: quality, adpcm: adpcm) : 0)
            } catch {
                if self.channel === channel && !Task.isCancelled {
                    self.status = error.localizedDescription
                    if self.destination == .youtubeAudio {
                        self.sendSearchStatus(self.status, channel: channel)
                    }
                    self.canSend = true
                }
            }
        }
    }

    private func sendStreamAudio(_ bytes: Data, from url: URL, playback: VideoPlayback, channel: IOBluetoothL2CAPChannel, cover: Data?,
                                 checksum: UInt32, liveAudioRate: Int) {
        let suffix: String
        switch playback {
        case .whileSending: suffix = ".wav"
        case .afterTransfer: suffix = ".nsa"
        case .receiveTest: suffix = ".ntx"
        }
        sendPayload(bytes, from: url, extension: suffix,
                    channel: channel, cover: cover, checksum: checksum, liveAudioRate: liveAudioRate)
    }

    func sendYouTubeVideo(_ link: String, playback: VideoPlayback, quality: HardwareVideo.Quality,
                          frameRate: HardwareVideo.FrameRate, audioQuality: ClassicQuality,
                          convertDuringTransfer: Bool) {
        guard canSend, let channel, destination == .youtubeStreamer else { return }
        prepareVideo(nil, youtubeLink: link, playback: playback, quality: quality,
                     frameRate: frameRate, audioQuality: audioQuality,
                     convertDuringTransfer: convertDuringTransfer, channel: channel)
    }

    private func prepareVideo(_ localURL: URL?, youtubeLink: String? = nil,
                              playback: VideoPlayback, quality: HardwareVideo.Quality,
                              frameRate: HardwareVideo.FrameRate, audioQuality: ClassicQuality,
                              convertDuringTransfer: Bool, channel: IOBluetoothL2CAPChannel,
                              selectionID: String? = nil) {
        let playLive = playback == .whileSending
        canSend = false
        activeVideoSelection = selectionID
        youtubeVideoReady = false
        videoPausedAt = nil
        videoProgress = nil
        status = "Checking video settings…"
        videoStage = "Checking video settings…"
        progressiveVideo = true
        isConvertingVideo = true
        headerSent = false
        videoBytesSent = 0
        videoCreditLimit = 0
        videoCreditWaitSince = nil
        videoTask = Task { @MainActor [weak self, weak channel] in
            guard let self, let channel else { return }
            var preparation = youtubeLink == nil ? VideoPreparation.encoding : .downloading
            let heartbeat = Task { @MainActor in
                while !Task.isCancelled && self.channel === channel {
                    self.reportVideoPreparation(preparation, channel: channel)
                    do { try await Task.sleep(for: .seconds(1)) }
                    catch { return }
                }
            }
            defer { heartbeat.cancel() }
            let workFolder = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
            defer { try? FileManager.default.removeItem(at: workFolder) }
            do {
                try FileManager.default.createDirectory(at: workFolder, withIntermediateDirectories: true)
                let url: URL
                while self.channel === channel && self.pending != nil {
                    try await Task.sleep(for: .milliseconds(10))
                }
                if quality.usesSmallGroups || frameRate == .fps22 {
                    func supported() -> Bool {
                        if frameRate == .fps22 && !self.supports22FPS { return false }
                        if quality.isIntermediate { return self.supportsIntermediateVideo }
                        return quality != .native || self.supportsNativeVideo
                    }
                    let deadline = ProcessInfo.processInfo.systemUptime + 2.5
                    while self.channel === channel && !supported() &&
                          ProcessInfo.processInfo.systemUptime < deadline {
                        try await Task.sleep(for: .milliseconds(50))
                    }
                    guard supported() else {
                        let message = frameRate == .fps22 && !self.supports22FPS ?
                            "Update the Nano video app to use 22 fps." :
                            "Update the Nano app or choose another video quality."
                        throw YouTubeAudio.DownloadError(message: message)
                    }
                }
                try Task.checkCancellation()
                guard self.channel === channel else { return }
                self.status = localURL.map { "Preparing \($0.lastPathComponent)…" } ?? "Downloading YouTube video…"
                self.videoStage = youtubeLink == nil ? "Checking video size and duration…" : "Downloading video and audio…"
                if selectionID != nil {
                    self.sendSearchStatus("Preparing selected video...", channel: channel, accepted: true)
                }
                if let youtubeLink {
                    url = try await YouTubeAudio.downloadVideo(youtubeLink, into: workFolder)
                } else if let localURL {
                    url = localURL
                } else {
                    throw YouTubeAudio.DownloadError(message: "Choose a video to send.")
                }
                let hasFileAccess = url.startAccessingSecurityScopedResource()
                defer { if hasFileAccess { url.stopAccessingSecurityScopedResource() } }
                try Task.checkCancellation()
                guard self.channel === channel else { return }
                preparation = .encoding
                self.videoStage = "Preparing H.264 video…"
                let effectiveRate = quality.effectiveFrameRate(frameRate, allNativeRates: self.supportsAllNativeRates)
                let estimated = try await HardwareVideo.estimatedFrames(url, frameRate: effectiveRate)
                let useADPCM = self.supportsVideoADPCM
                let staged = workFolder.appendingPathComponent("video.groups")
                var transferBytes: UInt32 = 0
                if !convertDuringTransfer {
                    self.videoProgress = estimated == nil ? nil : 0
                    self.reportVideoPreparation(preparation, channel: channel)
                    try Data().write(to: staged)
                    let writer = try FileHandle(forWritingTo: staged)
                    defer { try? writer.close() }
                    var frames = 0
                    var payloadBytes: UInt64 = 0
                    try await HardwareVideo.convertGroups(url, quality: quality, frameRate: effectiveRate,
                                                         audioQuality: audioQuality, playLive: playLive,
                                                         useADPCM: useADPCM) { data, group in
                        var length = UInt32(data.count).littleEndian
                        try writer.write(contentsOf: withUnsafeBytes(of: &length) { Data($0) })
                        try writer.write(contentsOf: data)
                        payloadBytes += UInt64(data.count)
                        guard payloadBytes <= UInt32.max else {
                            throw YouTubeAudio.DownloadError(message: "The video exceeds the Nano’s 4 GiB transfer limit.")
                        }
                        if group {
                            frames += Self.videoGroupFrames(data)
                            self.updateVideoProgress(frames: frames, total: estimated)
                        }
                    }
                    try writer.synchronize()
                    transferBytes = UInt32(payloadBytes)
                }
                func startTransfer() async throws {
                    preparation = .starting
                    self.videoStage = "Starting video transfer…"
                    self.videoProgress = nil
                    heartbeat.cancel()
                    self.reportVideoPreparation(preparation, channel: channel)
                    var header = Data("NCL3".utf8)
                    for word in [transferBytes, estimated.map(UInt32.init) ?? 0] {
                        var littleEndian = word.littleEndian
                        withUnsafeBytes(of: &littleEndian) { header.append(contentsOf: $0) }
                    }
                    header.append(Data(count: Self.headerSize - header.count))
                    while self.channel === channel && self.pending != nil {
                        try await Task.sleep(for: .milliseconds(10))
                    }
                    try Task.checkCancellation()
                    guard self.channel === channel else { throw CancellationError() }
                    try await self.sendVideoChunk(header, header: true, channel: channel)
                    self.videoStage = convertDuringTransfer ? "Converting and sending H.264…" : "Sending H.264…"
                    self.videoProgress = estimated != nil || !convertDuringTransfer ? 0 : nil
                }
                if convertDuringTransfer {
                    var framesSent = 0
                    try await HardwareVideo.convertGroups(url, quality: quality, frameRate: effectiveRate,
                                                         audioQuality: audioQuality, playLive: playLive,
                                                         useADPCM: useADPCM) { data, group in
                        if !self.headerSent { try await startTransfer() }
                        try await self.sendVideoChunk(data, header: false, channel: channel)
                        if group {
                            framesSent += Self.videoGroupFrames(data)
                            self.updateVideoProgress(frames: framesSent, total: estimated)
                        }
                    }
                } else {
                    try await startTransfer()
                    let reader = try FileHandle(forReadingFrom: staged)
                    defer { try? reader.close() }
                    let total = try reader.seekToEnd()
                    try reader.seek(toOffset: 0)
                    while let length = try reader.read(upToCount: 4), !length.isEmpty {
                        guard length.count == 4 else { throw CocoaError(.fileReadCorruptFile) }
                        let count = length.withUnsafeBytes { Int($0.loadUnaligned(as: UInt32.self).littleEndian) }
                        guard let chunk = try reader.read(upToCount: count), chunk.count == count else {
                            throw CocoaError(.fileReadCorruptFile)
                        }
                        try await self.sendVideoChunk(chunk, header: false, channel: channel)
                        self.videoProgress = min(99, Int(try reader.offset() * 100 / total))
                    }
                }
                self.status = "Video sent. Leave \(self.destination.rawValue) open to finish playback."
                self.videoStage = "Transfer complete"
                self.videoProgress = 100
                if self.destination == .youtubeStreamer { self.canSend = self.youtubeVideoReady }
            } catch {
                heartbeat.cancel()
                if self.channel === channel && !Task.isCancelled {
                    self.status = error.localizedDescription
                    self.videoProgress = nil
                    self.videoStage = ""
                    if self.headerSent {
                        self.transferError = self.status
                        _ = channel.close()
                    } else {
                        if self.destination == .youtubeStreamer {
                            while self.channel === channel && self.pending != nil && !Task.isCancelled {
                                try? await Task.sleep(for: .milliseconds(10))
                            }
                            if self.channel === channel && !Task.isCancelled {
                                self.canSend = true
                                self.sendSearchStatus(self.status, channel: channel)
                            }
                        } else {
                            self.canSend = true
                        }
                    }
                }
            }
            if self.channel === channel {
                self.activeVideoSelection = nil
                self.progressiveVideo = false
                self.isConvertingVideo = false
                self.videoTask = nil
            }
        }
    }

    private static func videoGroupFrames(_ data: Data) -> Int {
        data.withUnsafeBytes { Int($0.loadUnaligned(fromByteOffset: 4, as: UInt32.self).littleEndian) }
    }

    private func updateVideoProgress(frames: Int, total: Int?) {
        if let total {
            videoProgress = min(99, frames * 100 / total)
        }
    }

    private func sendVideoChunk(_ bytes: Data, header: Bool,
                                channel: IOBluetoothL2CAPChannel) async throws {
        try Task.checkCancellation()
        guard self.channel === channel, progressiveVideo else { throw CancellationError() }
        guard header || videoBytesSent + bytes.count <= Int(UInt32.max) else {
            throw YouTubeAudio.DownloadError(message: "The video exceeded the device’s 4 GiB transfer range.")
        }
        try await withCheckedThrowingContinuation { continuation in
            chunkWaiter = continuation
            if header {
                let result = enqueue(bytes, payloadBytes: 0, to: channel)
                if result != kIOReturnSuccess { failWrite(result) }
            } else {
                file = bytes
                sent = 0
                submitted = 0
                retryCount = 0
                pump(channel)
            }
        }
    }

    private func finishVideoChunk(_ error: Error? = nil) {
        guard let waiter = chunkWaiter else { return }
        chunkWaiter = nil
        if let error { waiter.resume(throwing: error) }
        else { waiter.resume() }
    }

    private nonisolated static func payloadChecksum(_ bytes: Data) throws -> UInt32 {
        var hash: UInt32 = 2_166_136_261
        try bytes.withUnsafeBytes { (buffer: UnsafeRawBufferPointer) in
            for start in stride(from: 0, to: buffer.count, by: 65_536) {
                try Task.checkCancellation()
                for byte in buffer[start..<min(start + 65_536, buffer.count)] {
                    hash = (hash ^ UInt32(byte)) &* 16_777_619
                }
            }
        }
        return hash
    }

    private func sendPayload(_ bytes: Data, from url: URL, extension suffix: String,
                             channel: IOBluetoothL2CAPChannel,
                             cover: Data? = nil, checksum: UInt32? = nil, liveAudioRate: Int = 0) {
        let checksum = checksum ?? bytes.reduce(UInt32(2_166_136_261)) { ($0 ^ UInt32($1)) &* 16_777_619 }
        testingAudioReceive = destination == .youtubeAudio && suffix == ".ntx"
        macPacedAudio = destination == .youtubeAudio
        liveAudioBytesPerSecond = macPacedAudio ? liveAudioRate : 0
        audioSendPaused = false
        let pacedLive = liveAudioBytesPerSecond > 0
        pacedAudioPackets = 0
        pacedAudioStartedAt = ProcessInfo.processInfo.systemUptime
        pacedAudioEpoch = nil
        audioTransferToken = pacedLive ? UInt32.random(in: 1...UInt32.max) : 0
        // The WAV length excludes the optional cover and the transfer token.
        let magic = pacedLive ? (cover == nil ? "NYW3" : "NYC3") : (cover == nil ? "NCL1" : "NYC1")
        var header = Data(magic.utf8)
        for word in [UInt32(bytes.count), checksum] {
            var littleEndian = word.littleEndian
            withUnsafeBytes(of: &littleEndian) { header.append(contentsOf: $0) }
        }
        let stem = url.deletingPathExtension().lastPathComponent
        let nameBytes = pacedLive ? 44 : 48
        let name = Array(stem.utf8.prefix(nameBytes - suffix.utf8.count)) + Array(suffix.utf8)
        header.append(contentsOf: name)
        header.append(Data(count: nameBytes - name.count))
        if pacedLive {
            var token = audioTransferToken.littleEndian
            withUnsafeBytes(of: &token) { header.append(contentsOf: $0) }
        }
        file = bytes
        transferCover = cover
        sent = 0
        submitted = 0
        retryCount = 0
        percentSent = 0
        headerSent = false
        canSend = false
        status = "Sending \(url.lastPathComponent)…"
        videoProgress = destination.sendsVideo ? 0 : nil
        audioProgress = 0
        let result = enqueue(header, payloadBytes: 0, to: channel)
        if result != kIOReturnSuccess {
            failWrite(result)
        }
    }

    private func enqueue(_ bytes: Data, payloadBytes: Int, to channel: IOBluetoothL2CAPChannel,
                         searchResponse: Bool = false) -> IOReturn {
        let buffer = bytes as NSData
        pending = PendingWrite(buffer: buffer, payloadBytes: payloadBytes, searchResponse: searchResponse)
        let result = channel.writeAsync(UnsafeMutableRawPointer(mutating: buffer.bytes),
                                        length: UInt16(buffer.length), refcon: nil)
        if result != kIOReturnSuccess {
            pending = nil
        }
        return result
    }

    private func failWrite(_ result: IOReturn) {
        let message = macPacedAudio && file != nil ? pacedAudioInterruption("write failed (\(result))") :
            "Classic write failed (\(result)). Reconnect to try again."
        file = nil
        transferCover = nil
        videoProgress = nil
        audioProgress = nil
        status = message
        transferError = message
        finishVideoChunk(NSError(domain: NSMachErrorDomain, code: Int(result),
                                 userInfo: [NSLocalizedDescriptionKey: status]))
    }

    func fileChannelWriteComplete(_ written: IOBluetoothL2CAPChannel, status result: IOReturn) {
        guard written === channel, let packet = pending else { return }
        pending = nil
        if packet.searchResponse {
            if result != kIOReturnSuccess { failSearchWrite(result) }
            return
        }
        guard result == kIOReturnSuccess else {
            failWrite(result)
            return
        }

        headerSent = true
        if progressiveVideo && packet.payloadBytes == 0 {
            finishVideoChunk()
            return
        }
        guard let file else { return }
        let artworkBytes = transferCover?.count ?? 0
        if macPacedAudio && packet.payloadBytes > 0 && sent >= artworkBytes {
            pacedAudioPackets += 1
        }
        sent += packet.payloadBytes
        if progressiveVideo {
            if sent == file.count {
                self.file = nil
                videoBytesSent += sent
                finishVideoChunk()
            } else {
                pump(written)
            }
            return
        }
        let total = file.count + artworkBytes
        if sent == total {
            self.file = nil
            transferCover = nil
            switch destination {
            case .youtubeAudio:
                audioProgress = 100
                canSend = true
                if testingAudioReceive {
                    status = "Audio sent. Check the nano for the test result."
                } else if macPacedAudio && liveAudioBytesPerSecond == 0 {
                    status = "Audio sent. The nano will verify the file before playing."
                } else {
                    status = "Audio sent. Search again on the nano when playback ends."
                }
            case .youtubeStreamer:
                break
            }
            return
        }
        let percent = sent * 100 / total
        if percent != percentSent {
            percentSent = percent
            audioProgress = percent
            if !audioSendPaused {
                let label = testingAudioReceive ? "Receive Test (Mac paced)" :
                    (liveAudioBytesPerSecond > 0 ? "Streaming audio" :
                        (macPacedAudio ? "Sending audio (Mac paced)" : "Sending audio"))
                status = "\(label)… \(percent)%"
            }
        }
        pump(written)
    }

    private func pump(_ channel: IOBluetoothL2CAPChannel) {
        guard videoPausedAt == nil, !audioSendPaused, !pumping, !retryScheduled, headerSent, let file else { return }
        pumping = true
        defer { pumping = false }

        let mtu = Int(channel.outgoingMTU)
        let packetSize = mtu > 0 ? mtu : Self.headerSize
        let cover = progressiveVideo ? nil : transferCover
        let artworkBytes = cover?.count ?? 0
        let total = file.count + artworkBytes
        while self.file != nil, submitted < total, pending == nil {
            let start = submitted
            let pacePayload = macPacedAudio && start >= artworkBytes
            let payloadSize = pacePayload ? min(packetSize, Self.pacedAudioPacketSize) : packetSize
            var end = min(start + payloadSize, start < artworkBytes ? artworkBytes : total)
            if pacePayload {
                let now = ProcessInfo.processInfo.systemUptime
                let cap = liveAudioBytesPerSecond > Self.pacedAudioBytesPerSecond ?
                    Self.pacedStereoBytesPerSecond : Self.pacedAudioBytesPerSecond
                let bytesPerSecond = liveAudioBytesPerSecond > 0
                    ? min(Double(cap), Double(liveAudioBytesPerSecond) * 1.05) : Double(cap)
                let packetTime = Double(end - start) / bytesPerSecond
                let endTime = Double(end - artworkBytes) / bytesPerSecond
                // Allow at least one packet to catch up. Otherwise a packet
                // longer than the 20 ms window is postponed on every retry.
                let catchUp = max(0, 0.02 - packetTime)
                let deadline = max((pacedAudioEpoch ?? now) + endTime, now - catchUp)
                pacedAudioEpoch = deadline - endTime
                if deadline > now {
                    schedulePump(channel, after: deadline - now)
                    return
                }
            }
            if progressiveVideo {
                end = min(end, videoCreditLimit - videoBytesSent)
                if end <= start {
                    let now = ProcessInfo.processInfo.systemUptime
                    let waitingSince = videoCreditWaitSince ?? now
                    videoCreditWaitSince = waitingSince
                    if now - waitingSince >= 20 {
                        let message = "The nano hasn’t freed video buffer space for 20 seconds. Reconnect to retry."
                        transferError = message
                        stop()
                        status = message
                    } else {
                        status = "Waiting for space in the nano’s video buffer…"
                        schedulePump(channel, after: 0.1)
                    }
                    return
                }
                videoCreditWaitSince = nil
                if status != "Sending video to the nano…" {
                    status = "Sending video to the nano…"
                }
            }
            submitted = end
            let packet: Data
            if let cover, start < artworkBytes {
                packet = cover.subdata(in: start..<end)
            } else {
                packet = file.subdata(in: (start - artworkBytes)..<(end - artworkBytes))
            }
            let result = enqueue(packet, payloadBytes: end - start, to: channel)
            if result == kIOReturnSuccess {
                retryCount = 0
                continue
            }
            submitted = start
            guard result == kIOReturnNoResources || result == kIOReturnNoSpace || result == kIOReturnBusy else {
                failWrite(result)
                return
            }
            retryCount += 1
            if retryCount <= 50 {
                schedulePump(channel, after: 0.01)
            } else {
                failWrite(result)
            }
            return
        }
    }

    private func schedulePump(_ channel: IOBluetoothL2CAPChannel, after delay: TimeInterval) {
        retryScheduled = true
        DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak self, weak channel] in
            guard let self, let channel, self.channel === channel else { return }
            self.retryScheduled = false
            self.pump(channel)
        }
    }
}

struct ClassicMediaView: View {
    let discoveryHint: String
    let destination: ClassicDestination
    @StateObject private var sender = ClassicSender()
    @State private var choosing = false
    @State private var showingDeviceSetup = false
    @State private var quality = ClassicQuality.low
    @AppStorage("classic.youtubeAudioPlayback") private var youtubeAudioPlayback = VideoPlayback.whileSending
    @State private var youtubeLink = ""
    @State private var youtubeQuery = ""
    @FocusState private var youtubeSearchFocused: Bool
    @AppStorage("classic.videoPlayback") private var videoPlayback = VideoPlayback.afterTransfer
    @State private var videoQuality = HardwareVideo.Quality.medium
    @State private var videoAudioQuality = ClassicQuality.medium
    @State private var convertDuringTransfer = false
    @State private var frameRate = HardwareVideo.FrameRate.fps15
    @State private var nanoAddress = ClassicSender.rememberedAddress ?? "CC:25:EF:C6:92:F7"

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack {
                VStack(alignment: .leading, spacing: 3) {
                    Text(sender.devices.first { $0.address == nanoAddress }?.name ?? "Nano")
                        .font(.headline)
                    if !nanoAddress.isEmpty {
                        Text(nanoAddress)
                            .font(.caption.monospacedDigit())
                            .foregroundStyle(.secondary)
                    }
                }
                Spacer()
                Button("Device Setup…") { showingDeviceSetup.toggle() }
                    .disabled(sender.connectedOrOpening)
                    .popover(isPresented: $showingDeviceSetup) { deviceSetup }
            }
            Text(sender.status)
                .font(.callout)
                .foregroundStyle(.secondary)
            HStack {
                if sender.isPairing {
                    Button("Stop Waiting") { sender.stop() }
                } else if sender.connectedOrOpening {
                    Button("Disconnect") { sender.stop() }
                } else {
                    Button("Connect") { sender.connect(address: nanoAddress, pairOnly: false, destination: destination) }
                        .disabled(nanoAddress.isEmpty)
                }
            }
            HStack {
                TextField(destination.sendsVideo ? "Search YouTube" : "Search YouTube Music", text: $youtubeQuery)
                    .textFieldStyle(.roundedBorder)
                    .focused($youtubeSearchFocused)
                    .onSubmit(submitYouTubeSearch)
                Button("Search", action: submitYouTubeSearch)
                    .disabled(!sender.canSend || youtubeQuery.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
            }
            Text(destination.sendsVideo ? "Search here or on the nano, then pick a video on the nano." :
                 "Search here or on the nano, then pick a song on the nano.")
                .font(.caption)
                .foregroundStyle(.secondary)
            HStack {
                TextField("YouTube link", text: $youtubeLink)
                    .textFieldStyle(.roundedBorder)
                    .onSubmit(sendLink)
                Button("Send Link", action: sendLink)
                    .disabled(!sender.canSend || youtubeLink.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                Button("Send File…") { choosing = true }
                    .disabled(!sender.canSend)
            }
            if destination.sendsVideo {
                Picker("Start playback", selection: $videoPlayback) {
                    ForEach(VideoPlayback.playbackOptions, id: \.self) { option in
                        Text(option.rawValue).tag(option)
                    }
                }
                HStack {
                    Picker("Video quality", selection: $videoQuality) {
                        ForEach(HardwareVideo.Quality.allCases, id: \.self) { option in
                            Text(option.label)
                                .tag(option)
                        }
                    }
                    let rates: [HardwareVideo.FrameRate] = videoQuality == .native && !sender.supportsAllNativeRates ?
                        [.fps22, .fps30] : HardwareVideo.FrameRate.allCases
                    Picker("Frame rate", selection: Binding(get: {
                        videoQuality.effectiveFrameRate(frameRate, allNativeRates: sender.supportsAllNativeRates)
                    },
                                                             set: { frameRate = $0 })) {
                        ForEach(rates, id: \.self) { option in
                            Text("\(option.rawValue) fps").tag(option)
                        }
                    }
                }
                if frameRate == .fps22 && sender.connectedOrOpening && !sender.supports22FPS {
                    Text("Update the Nano video app to use 22 fps.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                } else if videoQuality == .native {
                    Text(sender.supportsNativeVideo ? "Native display resolution" :
                         "Requires the updated Nano video app.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                } else if videoQuality.isIntermediate && sender.connectedOrOpening && !sender.supportsIntermediateVideo {
                    Text("Requires the updated Nano video app.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Picker("Audio quality", selection: $videoAudioQuality) {
                    ForEach(ClassicQuality.allCases, id: \.self) { option in
                        Text(option.label).tag(option)
                    }
                }
                Toggle("Convert during transfer", isOn: $convertDuringTransfer)
                    .disabled(sender.isConvertingVideo)
                Text(videoPlayback == .afterTransfer ? "Hardware H.264 · plays after transfer" :
                    "Hardware H.264 · starts after buffering")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                if sender.isConvertingVideo && sender.videoProgress == nil {
                    ProgressView(sender.videoStage)
                }
                if let progress = sender.videoProgress {
                    ProgressView(value: Double(progress), total: 100) {
                        Text(sender.videoStage)
                    } currentValueLabel: {
                        Text("\(progress)%").monospacedDigit()
                    }
                }
            } else {
                Picker("Start playback", selection: $youtubeAudioPlayback) {
                    ForEach(VideoPlayback.allCases, id: \.self) { Text($0.rawValue).tag($0) }
                }
                .pickerStyle(.menu)
                Picker("Audio quality", selection: $quality) {
                    ForEach(ClassicQuality.allCases, id: \.self) { option in
                        Text(option.label).tag(option)
                    }
                }
                .pickerStyle(.menu)
                .onChange(of: quality, initial: true) { _, value in sender.youtubeSearchQuality = value }
                let adpcm = sender.youtubeTrackUsesADPCM(quality, playback: youtubeAudioPlayback)
                let bytesPerSecond = YouTubeAudio.encodedBytesPerSecond(quality: quality, adpcm: adpcm)
                let sizeLabel = String(format: "%.2f MB per minute", Double(bytesPerSecond) * 60 / 1_000_000)
                Text(adpcm ? "ADPCM · \(sizeLabel)" : sizeLabel)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                if let progress = sender.audioProgress {
                    ProgressView(value: Double(progress), total: 100)
                } else if sender.isPreparingAudio {
                    ProgressView().controlSize(.small)
                }
            }
        }
        .onChange(of: youtubeAudioPlayback, initial: true) { _, value in sender.youtubeAudioPlayback = value }
        .onChange(of: videoPlayback, initial: true) { _, value in sender.youtubeVideoPlayback = value }
        .onChange(of: videoQuality, initial: true) { _, value in sender.youtubeVideoQuality = value }
        .onChange(of: frameRate, initial: true) { _, value in sender.youtubeFrameRate = value }
        .onChange(of: videoAudioQuality, initial: true) { _, value in sender.youtubeVideoAudioQuality = value }
        .onChange(of: convertDuringTransfer, initial: true) { _, value in sender.youtubeConvertDuringTransfer = value }
        .onChange(of: sender.canSend) { _, ready in
            if ready && youtubeQuery.isEmpty {
                youtubeSearchFocused = true
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .padding(22)
        .fileImporter(isPresented: $choosing, allowedContentTypes: destination.sendsVideo
                      ? HardwareVideo.fileTypes : [.audio]) { result in
            if destination.sendsVideo {
                sender.sendVideoFile(result, playback: videoPlayback, quality: videoQuality,
                                     frameRate: frameRate, audioQuality: videoAudioQuality,
                                     convertDuringTransfer: convertDuringTransfer)
            } else {
                sender.sendAudioFile(result, quality: quality, playback: youtubeAudioPlayback)
            }
        }
    }

    private func sendLink() {
        let link = youtubeLink.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !link.isEmpty else { return }
        if destination.sendsVideo {
            sender.sendYouTubeVideo(link, playback: videoPlayback, quality: videoQuality,
                                   frameRate: frameRate, audioQuality: videoAudioQuality,
                                   convertDuringTransfer: convertDuringTransfer)
        } else {
            sender.sendYouTubeAudio(link, quality: quality, playback: youtubeAudioPlayback)
        }
    }

    private func submitYouTubeSearch() {
        sender.searchYouTube(youtubeQuery)
    }

    private var deviceSetup: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(discoveryHint)
                .font(.callout)
                .foregroundStyle(.secondary)
                .textSelection(.enabled)
            HStack {
                Menu("Choose Device") {
                    ForEach(sender.devices) { device in
                        Button(device.title) { nanoAddress = device.address }
                    }
                }
                .disabled(sender.devices.isEmpty)
                if sender.isSearching {
                    Button("Stop Search") { sender.stopDiscovery() }
                } else {
                    Button("Find Devices") { sender.findDevices() }
                }
            }
            if !sender.discoveryStatus.isEmpty {
                Text(sender.discoveryStatus)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            TextField("Nano Bluetooth address", text: $nanoAddress)
                .textFieldStyle(.roundedBorder)
            Button("Pair") {
                showingDeviceSetup = false
                sender.connect(address: nanoAddress, pairOnly: true, destination: destination)
            }
            .disabled(nanoAddress.isEmpty)
        }
        .padding(16)
        .frame(width: 340)
        .onAppear { sender.refreshDevices() }
        .onDisappear { sender.stopDiscovery() }
    }
}
