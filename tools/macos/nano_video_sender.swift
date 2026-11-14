import AVFoundation
import Foundation
import UniformTypeIdentifiers

enum HardwareVideo {
    static let fileTypes: [UTType] = [.movie, .mpeg4Movie, .quickTimeMovie]
    private static let maxSampleBytes = 16 * 1024
    private static let adpcmChannelFlag: UInt32 = 0x8000_0000

    enum Quality: CaseIterable {
        case double, compact, medium, large, native

        var label: String {
            switch self {
            case .double: "128 × 72 · Double"
            case .compact: "192 × 112"
            case .medium: "256 × 144"
            case .large: "320 × 176"
            case .native: "432 × 240"
            }
        }

        var dimensions: (width: Int, height: Int) {
            switch self {
            case .double: (128, 72)
            case .compact: (192, 112)
            case .medium: (256, 144)
            case .large: (320, 176)
            case .native: (432, 240)
            }
        }

        var usesSmallGroups: Bool { self != .double }
        var isIntermediate: Bool { usesSmallGroups && self != .native }
        var crf: Int { self == .double ? 13 : 24 }

        func bitrateLimit(live: Bool) -> [String] {
            guard usesSmallGroups else { return [] }
            let cap: Int
            switch self {
            case .compact: cap = live ? 160 : 320
            case .medium: cap = live ? 225 : 480
            case .large: cap = live ? 320 : 650
            case .native: cap = live ? 500 : 950
            case .double: return []
            }
            return ["-maxrate", "\(cap)k", "-bufsize", "\(min(cap / 2, 110))k"]
        }

        func effectiveFrameRate(_ selected: FrameRate, allNativeRates: Bool) -> FrameRate {
            self == .native && !allNativeRates && selected != .fps22 ? .fps30 : selected
        }
    }

    enum FrameRate: Int, CaseIterable {
        case fps5 = 5, fps10 = 10, fps15 = 15, fps22 = 22, fps30 = 30
    }

    private enum VideoError: LocalizedError {
        case noEncoder, encode, format

        var errorDescription: String? {
            switch self {
            case .noEncoder: "Hardware H.264 needs FFmpeg. Install it with brew install ffmpeg."
            case .encode: "FFmpeg could not encode the video for the nano."
            case .format: "FFmpeg returned an unsupported H.264 clip."
            }
        }
    }

    static func estimatedFrames(_ url: URL, frameRate: FrameRate) async throws -> Int? {
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        let asset = AVURLAsset(url: url)
        guard let track = try await asset.loadTracks(withMediaType: .video).first else {
            throw VideoError.format
        }
        let frames = ceil(try await track.load(.timeRange).duration.seconds * Double(frameRate.rawValue))
        return frames.isFinite && frames > 0 && frames < Double(UInt32.max) ? Int(frames) : nil
    }

    // FFmpeg closes each H.264 segment before opening the next one. Audio is
    // written alongside the segments, so the sender can read both as they land.
    static func convertGroups(_ url: URL, quality: Quality, frameRate: FrameRate,
                              audioQuality: ClassicQuality, playLive: Bool, useADPCM: Bool = false,
                              emit: (Data, Bool) async throws -> Void) async throws {
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        let fps = frameRate.rawValue
        let groupFrames = quality.usesSmallGroups ? 4 : 12
        let asset = AVURLAsset(url: url)
        let hasAudio = try await !asset.loadTracks(withMediaType: .audio).isEmpty
        let rate = Int(audioQuality.sampleRate)
        let channels = Int(audioQuality.channels)
        let folder = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: folder) }
        let audioFormat = useADPCM ? "s16le" : "mulaw"
        let audio = folder.appendingPathComponent("audio.\(audioFormat)")
        let process = Process()
        defer { if process.isRunning { process.terminate() } }
        process.executableURL = try encoderURL()
        process.standardOutput = FileHandle.nullDevice
        process.standardError = FileHandle.nullDevice
        let bitrateLimit = quality.bitrateLimit(live: playLive)
        let encoderOptions = "keyint=\(groupFrames):scenecut=0:bframes=0:ref=1:slices=1:repeat-headers=1"
            + ":sliced-threads=0" + (quality.usesSmallGroups ? ":force-cfr=1" : "")
        let threads = min(4, ProcessInfo.processInfo.activeProcessorCount)
        var args = [
            "-v", "error", "-nostdin", "-i", url.path, "-map", "0:v:0", "-an", "-vf", videoFilter(frameRate, quality: quality),
            "-c:v", "libx264", "-threads", String(threads), "-thread_type", "frame",
            "-profile:v", "baseline", "-level:v", "3.0",
            "-pix_fmt", "yuv420p", "-tune", "zerolatency", "-crf", String(quality.crf),
            "-color_range", "tv", "-colorspace", "smpte170m",
        ] + bitrateLimit + [
            "-x264-params", encoderOptions,
            "-f", "segment", "-segment_time", String(Double(groupFrames) / Double(fps)),
            "-segment_time_delta", String(0.5 / Double(fps)), "-reset_timestamps", "1",
            "-segment_format", "h264", folder.appendingPathComponent("%08d.h264").path
        ]
        if hasAudio {
            args += ["-map", "0:a:0", "-vn", "-af", "aresample=\(rate):async=1:first_pts=0",
                     "-ac", String(channels), "-ar", String(rate),
                     "-c:a", useADPCM ? "pcm_s16le" : "pcm_mulaw",
                     "-flush_packets", "1", "-f", audioFormat, audio.path]
        }
        process.arguments = args
        try process.run()
        func segment(_ index: Int) -> URL {
            folder.appendingPathComponent(String(format: "%08d.h264", index))
        }
        var config: Data?
        var frames = 0
        var index = 0
        var audioHandle: FileHandle?
        defer { try? audioHandle?.close() }
        while true {
            try Task.checkCancellation()
            let active = process.isRunning
            if !active && process.terminationStatus != 0 { throw VideoError.encode }
            if active && !FileManager.default.fileExists(atPath: segment(index + 1).path) {
                try await Task.sleep(for: .milliseconds(25))
                continue
            }
            guard FileManager.default.fileExists(atPath: segment(index).path) else { break }
            let clip = try extractAnnexB(Data(contentsOf: segment(index)))
            guard clip.samples.count <= groupFrames else { throw VideoError.format }
            if let config {
                guard config == clip.config else { throw VideoError.format }
            } else {
                config = clip.config
                let magic = quality.usesSmallGroups ? (playLive ? "NVL3" : "NVD3") :
                                                 (playLive ? "NVL2" : "NVD2")
                var header = Data(magic.utf8)
                for value in [clip.config.count, fps, rate] { appendWord(UInt32(value), to: &header) }
                appendWord(UInt32(channels) | (useADPCM ? adpcmChannelFlag : 0), to: &header)
                header.append(clip.config)
                try await emit(header, false)
            }
            let nextFrame = frames + clip.samples.count
            guard nextFrame < Int(UInt32.max) else { throw VideoError.format }
            let bytesPerAudioFrame = channels * (useADPCM ? 2 : 1)
            let audioFrames = nextFrame * rate / fps - frames * rate / fps
            let audioEnd = nextFrame * rate / fps * bytesPerAudioFrame
            let audioCount = audioFrames * bytesPerAudioFrame
            var sound = Data()
            if hasAudio {
                while process.isRunning {
                    let attributes = try? FileManager.default.attributesOfItem(atPath: audio.path)
                    let size = (attributes?[.size] as? NSNumber)?.intValue ?? 0
                    if size >= audioEnd { break }
                    try await Task.sleep(for: .milliseconds(25))
                }
                if audioHandle == nil { audioHandle = try FileHandle(forReadingFrom: audio) }
                sound = try audioHandle?.read(upToCount: audioCount) ?? Data()
            }
            if sound.count < audioCount {
                if useADPCM {
                    // A short audio track gets PCM silence through the video end.
                    sound.removeLast(sound.count % bytesPerAudioFrame)
                }
                sound.append(Data(repeating: useADPCM ? 0 : 0xff, count: audioCount - sound.count))
            }
            var body = Data()
            for sample in clip.samples {
                appendWord(UInt32(sample.count), to: &body)
                body.append(sample)
            }
            body.append(useADPCM ? encodeADPCM(sound, frames: audioFrames, channels: channels) : sound)
            var group = Data()
            appendWord(UInt32(body.count), to: &group)
            appendWord(UInt32(clip.samples.count), to: &group)
            appendWord(body.reduce(2_166_136_261) { ($0 ^ UInt32($1)) &* 16_777_619 }, to: &group)
            group.append(body)
            try await emit(group, true)
            try FileManager.default.removeItem(at: segment(index))
            frames = nextFrame
            index += 1
        }
        guard frames > 0 else { throw VideoError.format }
        while process.isRunning { try await Task.sleep(for: .milliseconds(25)) }
        if process.terminationStatus != 0 { throw VideoError.encode }
        var end = Data()
        appendWord(0, to: &end)
        appendWord(UInt32(frames), to: &end)
        appendWord(0, to: &end)
        try await emit(end, false)
    }

    private static let imaSteps = [
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
        34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
        157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
        598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707,
        1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
        5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635,
        13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
    ]
    private static let imaIndexChange = [-1, -1, -1, -1, 2, 4, 6, 8]

    // One independent IMA WAV block per video group. Four bytes of codes per
    // channel carry eight frames. A video group's frame count trims the padding.
    private static func encodeADPCM(_ pcm: Data, frames: Int, channels: Int) -> Data {
        guard frames > 0 else { return Data() }
        let groups = (frames - 1 + 7) / 8
        var output = Data(count: 4 * channels * (1 + groups))
        pcm.withUnsafeBytes { (samples: UnsafeRawBufferPointer) in
            output.withUnsafeMutableBytes { (encoded: UnsafeMutableRawBufferPointer) in
                for channel in 0..<channels {
                    func sample(_ frame: Int) -> Int {
                        let offset = (frame * channels + channel) * 2
                        return Int(Int16(littleEndian: samples.loadUnaligned(fromByteOffset: offset, as: Int16.self)))
                    }
                    var predictor = sample(0)
                    // Seed the step from the first difference to avoid spending
                    // the first few samples adapting after every group boundary.
                    let firstDifference = frames > 1 ? abs(sample(1) - predictor) : 0
                    var stepIndex = imaSteps.firstIndex { $0 >= firstDifference } ?? (imaSteps.count - 1)
                    let header = channel * 4
                    encoded[header] = UInt8(truncatingIfNeeded: predictor)
                    encoded[header + 1] = UInt8(truncatingIfNeeded: predictor >> 8)
                    encoded[header + 2] = UInt8(stepIndex)
                    for group in 0..<groups {
                        let offset = (group + 1) * 4 * channels + channel * 4
                        for index in 0..<8 {
                            let frame = group * 8 + index + 1
                            guard frame < frames else { break }
                            let error = sample(frame) - predictor
                            let step = imaSteps[stepIndex]
                            let half = step >> 1
                            let quarter = step >> 2
                            var remaining = abs(error)
                            var magnitude = 0
                            var delta = step >> 3
                            if remaining >= step {
                                remaining -= step
                                magnitude |= 4
                                delta += step
                            }
                            if remaining >= half {
                                remaining -= half
                                magnitude |= 2
                                delta += half
                            }
                            if remaining >= quarter {
                                magnitude |= 1
                                delta += quarter
                            }
                            let code = magnitude | (error < 0 ? 8 : 0)
                            predictor = min(Int(Int16.max), max(Int(Int16.min), predictor + (error < 0 ? -delta : delta)))
                            stepIndex = min(imaSteps.count - 1, max(0, stepIndex + imaIndexChange[magnitude]))
                            encoded[offset + index / 2] |= UInt8(code << (4 * (index % 2)))
                        }
                    }
                }
            }
        }
        return output
    }

    private static func videoFilter(_ frameRate: FrameRate, quality: Quality) -> String {
        let fps = "fps=\(frameRate.rawValue):start_time=0:round=up:eof_action=pass"
        let (width, height) = quality.dimensions
        let dimensions = "\(width):\(height)"
        let scale = "scale=\(dimensions):force_original_aspect_ratio=increase:force_divisible_by=2:"
            + "out_color_matrix=bt601:out_range=tv"
        let padding = quality.usesSmallGroups ? "" : ",pad=128:128:0:28"
        let fit = "crop=\(dimensions)\(padding),setsar=1"
        return "\(fps),\(scale),\(fit)"
    }

    static func encoderURL() throws -> URL {
        let path = ProcessInfo.processInfo.environment["PATH"] ?? ""
        let directories = ["/opt/homebrew/bin", "/usr/local/bin", "/usr/bin"]
            + path.split(separator: ":").map(String.init)
        for directory in directories where directory.hasPrefix("/") {
            let url = URL(fileURLWithPath: directory).appendingPathComponent("ffmpeg")
            if FileManager.default.isExecutableFile(atPath: url.path) {
                return url
            }
        }
        throw VideoError.noEncoder
    }

    private static func extractAnnexB(_ encoded: Data) throws -> (config: Data, samples: [Data]) {
        var sps: Data?
        var pps: Data?
        var frames: [Data] = []
        var start: Int?
        var zeroCount = 0
        for index in 0..<encoded.count {
            let byte = encoded[index]
            if byte == 1 && zeroCount >= 2 {
                if let start {
                    try collect(encoded, from: start, to: index - zeroCount,
                                sps: &sps, pps: &pps, frames: &frames)
                }
                start = index + 1
            }
            zeroCount = byte == 0 ? zeroCount + 1 : 0
        }
        guard let start else { throw VideoError.format }
        try collect(encoded, from: start, to: encoded.count - zeroCount,
                    sps: &sps, pps: &pps, frames: &frames)
        guard let sps, let pps, sps.count >= 5, pps.count >= 2, sps[1] == 66,
              sps[3] <= 30, 11 + sps.count + pps.count <= 128, !frames.isEmpty
        else { throw VideoError.format }

        var avcc = Data([1, sps[1], sps[2], sps[3], 255, 225, 0, UInt8(sps.count)])
        avcc.append(sps)
        avcc.append(contentsOf: [1, 0, UInt8(pps.count)])
        avcc.append(pps)

        let samples = frames.map { frame in
            var length = UInt32(frame.count).bigEndian
            var sample = withUnsafeBytes(of: &length) { Data($0) }
            sample.append(frame)
            return sample
        }
        return (avcc, samples)
    }

    private static func collect(_ encoded: Data, from start: Int, to end: Int,
                                sps: inout Data?, pps: inout Data?, frames: inout [Data]) throws {
        guard start < end else { throw VideoError.format }
        switch encoded[start] & 31 {
        case 7:
            let set = encoded.subdata(in: start..<end)
            guard sps == nil || sps == set else { throw VideoError.format }
            sps = set
        case 8:
            let set = encoded.subdata(in: start..<end)
            guard pps == nil || pps == set else { throw VideoError.format }
            pps = set
        case 1, 5:
            let keyframe = encoded[start] & 31 == 5
            guard end - start + 4 <= maxSampleBytes,
                  !frames.isEmpty || keyframe else { throw VideoError.format }
            frames.append(encoded.subdata(in: start..<end))
        default:
            break
        }
    }

    private static func appendWord(_ value: UInt32, to bytes: inout Data) {
        var word = value.littleEndian
        withUnsafeBytes(of: &word) { bytes.append(contentsOf: $0) }
    }
}
