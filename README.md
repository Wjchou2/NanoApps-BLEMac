AI was used in the making of this project. 

Built on top of [NanoApps](https://github.com/nfzerox/NanoApps/tree/main)!

# NanoApps Bluetooth and Media

Send audio, video, and text between a Mac and a seventh-generation iPod nano. **YT Music** and **YT Video** use Bluetooth Classic for media transfers. **BLE Chat** and **Bluetooth Test** use Bluetooth Low Energy for text and short audio tests. The Mac handles YouTube search, downloads, and conversion. The Nano handles buffering, sound, and hardware H.264 playback.

## Download and Install NanoApps

The supported device is an **iPod nano 7** running firmware **1.1.2 (`39A10023`)**. The 2012 and 2015 hardware revisions have separate first-install images. The companion app runs on an Apple silicon Mac with macOS 15 or later.

### If NanoApps is already installed

1. Connect the Nano over USB and open its disk in Finder. Create an `Apps` folder at the root if needed.
2. Unzip [YT Music](downloads/youtubeaudio.zip), [YT Video](downloads/youtubestreamer.zip), or [BLE Chat](downloads/blechat.zip). Copy each complete `.nanoapp` folder into the Nano’s `Apps` folder. An app folder contains `app.hbapp`, `icon.bin`, and `manifest.napp`.
3. When updating from the old names **YouTube Audio** or **YouTube Streamer**, move those old folders off the Nano. Each old and new folder shares an app identity. The command-line installer handles the rename automatically.
4. Eject the Nano and return to Home. If Home still shows an old icon or title, restart it to reload the icon cache.
5. Open [**Nano Bluetooth Link.app**](downloads/Nano%20Bluetooth%20Link.app/). For local media conversion, install FFmpeg. For YouTube features, also install `yt-dlp` and Node.js. The current sender invokes `yt-dlp` with the Node.js runtime.

```sh
brew install ffmpeg yt-dlp node
```

### First install on a stock Nano

1. Back up the Nano and identify its hardware revision. Download the matching `NanoApps-2012-Model.ipsw` or `NanoApps-2015-Model.ipsw` and `NanoApps-disk.zip` from the same release. [Project releases](https://github.com/Wjchou2/NanoApps-BLEMac/releases).
2. With the Nano connected, select it in Finder. Hold **Option** while clicking **Check for Update**, then select the matching IPSW. Wait for the update to complete and for the Nano to reconnect.
3. Unzip `NanoApps-disk.zip`. Merge its `Apps` and `Library` contents into the corresponding folders at the root of the Nano’s disk. The runtime is `Library/resident.hbapp`. Copy the optional `Data` sample files individually if you already keep app data there.
4. Eject the Nano so it can load the resident and register the apps. If you are replacing an existing resident, restart the Nano.

If the release files are unavailable, the source installer below can build the matching firmware and guide you through the Finder update.

### Build or update from source

Run these commands from the repo root.

```sh
./start deps --check
./start package youtubeaudio
./start package youtubestreamer
./start --local install youtubeaudio youtubestreamer blechat
./start --local install --resident-only
```

`package` creates a standalone ZIP in `build/packages/`. `install` with explicit targets deploys only those apps and can guide a first-time firmware setup. `--resident-only` updates the runtime while preserving the installed app catalog. To take a local app out of future **Install all apps** runs, move its folder outside `apps/`. A leftover `build/<name>.hbapp` can still be found by the packager. To uninstall an app from the device, remove its `.nanoapp` folder from the Nano’s `Apps/` directory and eject.

Build the Mac executable with Swift on macOS if you are changing the companion app. Its bundle name and executable are specified in the [app’s Info.plist](downloads/Nano%20Bluetooth%20Link.app/Contents/Info.plist).

```sh
swiftc -O -o /tmp/NanoBluetoothLink \
  tools/macos/nano_bluetooth_link.swift \
  tools/macos/nano_classic_sender.swift \
  tools/macos/nano_video_sender.swift
```

## Quick Usage

### Send music or video

1. Enable Bluetooth on the Nano. Open **YT Music** or **YT Video** there, and select the matching tab in **Nano Bluetooth Link**.
2. On a first connection, use **Find Mac** on the Nano if needed and **Device Setup** on the Mac to choose the Nano. If discovery misses it, enter its Classic Bluetooth address in **Device Setup**. Start pairing and confirm the matching code on both devices before clicking **Connect**. The apps remember the peer for later sessions.
3. Pick a quality and **Start playback** mode. Use **Send File…** for a local audio file in **YT Music** or a local video file in **YT Video**. **Send Link** accepts a YouTube URL. To browse, search from either screen and pick the result on the Nano.
4. **After Transfer** writes the encoded media to temporary storage before playback. **While Sending** starts after buffering and continues as the Mac sends more data. YT Video’s **Convert during transfer** setting overlaps conversion and sending once the encoder produces a complete group.
5. Use the Nano’s playback controls for pause and stop. Tap the video screen to reveal its controls. A buffered pause can take a moment to drain the samples already submitted to the mixer.

YT Music offers 7.35, 11.025, 22.05, and 44.1 kHz mono, plus 44.1 kHz stereo. YT Video has presets from **128 × 72** through **432 × 240** and offers **5, 10, 15, 22, and 30 FPS** where the Nano advertises support. The video tab has its own audio quality setting. If a clip stutters during transfer, replay it **After Transfer** at the same quality to see whether the hitch persists with all the data on the device.

### Send a BLE message

Select **BLE** on the Mac to advertise **Nano Bluetooth Link**, then open **BLE Chat** on the Nano and tap **Connect**. It looks for that advertised name. Send text from the Mac and type a reply on the Nano. Replies display under **From Nano** and support up to 20 printable ASCII characters. A Nano-to-Mac write routine was verified against the firmware, and **BLE Chat** still needs a successful live round-trip test. If automatic discovery or connection fails, **Bluetooth Test → Scan BLE** lets you see whether the Mac is appearing under the expected name.

To send a short clip over BLE, connect from **Bluetooth Test** and use **Send Audio…** in the Mac’s **BLE** tab. This path accepts clips up to 30 seconds and offers GATT Read, Notifications, and Fast Notifications.

### App overview

| Nano app | Build target | Purpose |
| --- | --- | --- |
| [YT Music](apps/youtubeaudio) | `youtubeaudio` | Search, browse cover art, and play local audio or audio from a YouTube link over Classic. |
| [YT Video](apps/youtubestreamer) | `youtubestreamer` | Search or send a link or file, then play H.264 with audio on the hardware display layer when available. |
| [BLE Chat](apps/blechat) | `blechat` | A small two-way text app for the Mac’s BLE tab. |
| [Bluetooth Test](apps/bluetoothtest) | `bluetoothtest` | Classic inquiry, BLE scanning, Nordic UART notifications, and short BLE audio tests. |
| [Audio Player](apps/audio_player) | `audio_player` | Browse WAV files saved in the Classic inbox by the earlier file receiver. |
| [Audio Probe](apps/audio_probe) and [Video Probe](apps/video_probe) | `audio_probe`, `video_probe` | Exercise the native mixer, decoder, frame buffers, and LCD layer independently of media transfer. |

The Mac companion now has **BLE**, **YT Music**, and **YT Video** tabs. The older **Classic Files** and **Stream** tabs were folded into the file and link actions in the media tabs. The standalone **VideoStream** target was retired. The older [Stream target](apps/stream) remains as a Classic audio example.

## SDK Changes

The C SDK calls private RetailOS interfaces mapped on firmware `39A10023`. In an LVGL app, set `LV_SURFACE := 1`, choose the modules you need, and include [`sdk/hb_app.mk`](sdk/hb_app.mk).

| Makefile option | Enables |
| --- | --- |
| `HB_BLE := scan` | Named BLE peripheral discovery. |
| `HB_BLE := uart` | Nordic UART notifications, BLE audio, and the chat write API. |
| `HB_CLASSIC := raw` | Pairing, a Classic L2CAP listener, and packet callbacks. |
| `HB_CLASSIC := inbox` | File reception to `/Data/ClassicInbox`. |
| `HB_CLASSIC := audio` or `video` | The earlier Classic media profiles. |
| `HB_CLASSIC := youtube_audio` or `youtube_video` | The current search and media profiles, including linked PCM support. |
| `HB_H264 := 1` | Standalone full-screen hardware H.264 playback. It is enabled automatically by the Classic video profiles. |
| `HB_VIDEO_LAYER := 1` | Access to the hardware video layer for apps that use it. |

### BLE and Classic

- **BLE discovery and messaging.** Use `hb_ble_scan(devices, &count, filter)` with room for `HB_BLE_SCAN_LIMIT` entries. A filter runs under the scan lock. Use `hb_ble_connect()` for text and audio, `hb_ble_connect_notifications()` for raw TX notifications, or `hb_ble_connect_chat()` for text with RX writes. Call `hb_ble_poll()` on the UI task at the returned interval. Chat sends use `hb_ble_chat_ready()` and `hb_ble_chat_send()`. Call `hb_ble_prepare_unload()` until it succeeds. [BLE API](sdk/hb_ble.h).
- **Classic pairing and raw packets.** Use `hb_classic_listen(psm, peer, callback, context)` with the six-byte Classic address in display order. Use `hb_classic_enable_pairing()`, `hb_classic_pair_code()`, and `hb_classic_pair_reply()` for a new peer. Copy receive data before the Bluetooth callback returns. Send from the UI task with `hb_classic_send()`, using `hb_classic_outgoing_mtu()` to size packets. [Classic API](sdk/hb_classic.h).
- **Media and search.** Call `hb_classic_init_media()` at app entry before installing the app’s unload hook, then `hb_classic_listen_media(peer)` and `hb_classic_poll()` on a UI timer. Search apps use `hb_classic_search_submit()`, `hb_classic_search_results()`, `hb_classic_search_artwork()`, and `hb_classic_search_play()`. Pass the result revision when asking to play a selection. The audio profile also exposes cover art, elapsed time, pause, and stop. The video profile exposes preparation and transfer progress. Call `hb_classic_disconnect()` and continue polling `hb_classic_prepare_unload()` until teardown finishes. [Media implementation](sdk/hb_classic.c).

### Audio mixer and hardware video

- **PCM.** The linked backend publishes a descriptor only after its PCM buffer is filled. The mixer’s reader position determines when a buffer is safe to reuse and how far playback has advanced. The two media profiles use it through the audio integration that the SDK builds from the Entrain directory. YT Music’s linked audio modes are enabled by the `/Data/YouTube-gapless-saved` and `/Data/YouTube-gapless-live` test files. YT Video tries its bounded descriptor pool when the hardware and available decoder memory qualify. [Linked PCM](sdk/hb_linked_pcm.c) · [Mixer integration](apps/entrain/platform/audio_device.c).
- **Saved H.264.** Call `hb_h264_init()` and `hb_h264_play_file(path)` for a supported NVH1–NVH5 clip. The original file stays in place. For the media receiver’s spooled groups, the SDK supplies a separate playback entry point. [H.264 API](sdk/hb_h264.h).
- **Live H.264.** Call `hb_h264_live_begin(config, size, count, fps, rate, channels, group_frames)` with AVCC SPS/PPS data. Check `hb_h264_live_room()` before submitting a group with `hb_h264_live_submit()`. Each group begins with a keyframe and carries length-prefixed compressed samples followed by audio. Set the ADPCM flag in `channels` for independently decodable IMA ADPCM audio groups. Supported group lengths are 4 frames for the native modes and 12 for the older path. For an unknown total, pass zero and finalize with `hb_h264_live_set_final_count()` for a short last group or `hb_h264_live_finish()` after a full group. [Decoder implementation](sdk/hb_h264.c).
- **Polling and teardown.** Call `hb_h264_live_tick()` on the UI task. It returns `0` while active, `1` for completion or cancellation, and `-1` on failure. Read `hb_h264_live_error()` for details. `hb_h264_stop()` can defer while scanout still owns a frame. Standalone apps with a custom unload hook call `hb_h264_prepare_unload()` until it succeeds. [Video layer API](sdk/hb_video_layer.h).

Encoding, quality presets, YouTube lookup, and the 20 ms packet-pacing fix run in the Mac app. The C SDK handles the Nano side of the transport and playback. [Mac audio and Classic sender](tools/macos/nano_classic_sender.swift) · [Mac video encoder](tools/macos/nano_video_sender.swift).

## Research

### 1. Finding the Bluetooth code in RetailOS

The project started with a small question. Could the Nano list nearby Bluetooth devices beyond the accessories shown in Settings? Bluetooth Test initially reported **Bluetooth client unavailable**. The firmware’s Settings controller provided a known working path. It created and used a Bluetooth client with its own discovery lifecycle and filtered the resulting devices for profiles the stock UI understood.

The stock firmware container held an encrypted `osos` image and readable UI resources. The resources gave us strings such as **Searching**, **Devices**, and **Not Paired** to trace back to the controller. The Nano supplied `39A10023-text.bin`, `39A10023-scan-tables.bin`, and `39A10023-discovery-tables.bin` during testing. The first is a snapshot of already-running code. The smaller exports contain runtime addresses and bytes for the relevant tables. An early export froze the device, so subsequent probes captured targeted regions. [Early Bluetooth Test research notes and address tables](apps/bluetoothtest/README.md) · [Firmware image analyzer](tools/analyze_nano7_firmware.py).

### 2. Classic inquiry, BLE advertising, and missing devices

We compared the Settings results with a direct Classic inquiry. Earbuds and speakers appeared first. Using the stock discovery agent’s any-service mask removed a profile filter and eventually showed the Mac. Classic inquiry still depends on the peer being discoverable at that moment. A phone, watch, or computer can be visible through one Bluetooth mechanism and absent from a scan using another.

BLE scanning came from the same firmware session used by Fitness. **BLE on, scan idle, 0 cached entries** exposed a lifecycle problem. The controller could scan successfully and then clear its reports when the request stopped. Bluetooth Test now copies results while scanning, under the firmware mutex, then stops its request. It reads the typed address, local name, and service hints. Private addresses and unnamed entries were common. An older device name lingering in the results could reflect the report cache. The displayed names depend on what peripherals advertise, so the app filters unnamed entries for selection. [BLE scanner](sdk/hb_ble.c) · [Apple’s Nano specifications](https://support.apple.com/en-me/112039).

On iOS we used nRF Connect and LightBlue to separate peripheral configuration from Nano behavior. The advertiser makes the peripheral discoverable. The GATT server supplies the service and characteristics after connection. An enabled server with the wrong UUID, a missing Notify property, or an advertiser without a visible name can fail at a different step. Apple’s Core Bluetooth documentation also explains why an iOS app’s background advertising can lose its local name. [Apple’s peripheral-role guide](https://developer.apple.com/library/archive/documentation/NetworkingInternetWeb/Conceptual/CoreBluetooth_concepts/PerformingCommonPeripheralRoleTasks/PerformingCommonPeripheralRoleTasks.html) · [Apple’s iOS background advertising notes](https://developer.apple.com/library/archive/documentation/NetworkingInternetWeb/Conceptual/CoreBluetooth_concepts/CoreBluetoothBackgroundProcessingForIOSApps/PerformingTasksWhileYourAppIsInTheBackground.html).

### 3. From a BLE link to text and short audio clips

A connected link was only the first milestone. Failures such as **GATT table mismatch**, **GATT error 1**, and **UART TX subscribe error 1** helped separate service discovery, characteristic discovery, and subscription. The working client discovers the service and the Notify characteristic, then lets the firmware find and write its Client Characteristic Configuration Descriptor. It finally displayed incoming text.

This project’s UART service uses the full UUID `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`. Under the same base UUID, `0003` is the Mac-to-Nano Notify characteristic, `0002` accepts a Nano-to-Mac write with response, and `0004` is the additional read characteristic for clip transfer. Text tests used printable ASCII, for example `48 65 6C 6C 6F` for **Hello**. The Mac app made it practical to send notifications when the iOS peripheral editor offered limited controls for updating a running characteristic. BLE Chat reuses discovery and subscription, then uses the firmware’s ATT write command for replies. Its connection problem remains a separate test item. [BLE implementation](sdk/hb_ble.c) · [Mac BLE service](tools/macos/nano_bluetooth_link.swift).

The first audio path sent compressed short clips in notifications. We tested larger values, fewer bytes of application framing, and GATT reads. The transport must respect the size the peer and implementation accept. Short GATT read responses also avoid triggering the Nano’s Read Blob behavior. Longer packets can reduce per-packet overhead, while acknowledgement timing, queue pressure, and the firmware’s read or notification path still affect throughput. BLE remains useful for short clips. Classic became the media transport for longer files and video. [BLE audio receiver](sdk/hb_ble_audio.c) · [Apple’s BLE data-transfer example](https://developer.apple.com/documentation/corebluetooth/transferring-data-between-bluetooth-low-energy-devices).

### 4. Classic L2CAP, pairing, and storage

Seeing the Mac in an inquiry result gave us an address, and pairing plus a usable L2CAP channel took more work. Early tests hit a Mac service publication failure, firmware error `0x0070`, pairing error `31`, an in-use system pairing session `0xA3`, and a close reason `0x01AF` after pairing. We tested both connection directions. The working design has the Nano listen for the chosen Mac and the Mac open the channel. Both sides can remember the peer. The Nano handles the matching-code prompt within the app, and the Mac advertises a small BLE discovery beacon for its Classic address. This is how the media apps can find the Mac without leaving Bluetooth Settings open. [Classic implementation](sdk/hb_classic.c) · [Mac Classic sender](tools/macos/nano_classic_sender.swift).

The first file test used a 256 KiB limit. Splitting the transport into chunks helped only once the receiver could write chunks to disk in order. We added an inbox and a separate Audio Player, and diagnosed a file that arrived but could not be opened. The media paths grew into bounded receive rings and temporary spools. Saved audio uses `/Data/.Stream-audio.tmp`, and saved video uses `/Data/.VideoStream-clip.tmp`. After Transfer verifies the completed input and plays it from storage. While Sending keeps a working set in RAM. Storage lets the transfer outgrow RAM, while decoder surfaces and active PCM still require RAM. Current packet and file formats have size limits, including a 32-bit transfer range for video. [Classic receiver](sdk/hb_classic.c) · [Audio Player](apps/audio_player) · [H.264 file definitions](sdk/hb_h264.h).

### 5. YouTube search, artwork, and the Mac workflow

The first search experiment verified a YouTube Data API key with a simple search. Its results differed from the YouTube Music catalog the user expected. The search flow later moved to `yt-dlp` on the Mac. YT Music merges YouTube Music song and video results, deduplicates IDs, and extracts audio for playback. YT Video searches YouTube videos. The Mac fills in artist or uploader information where possible and supplies thumbnails for the Nano. The user can advance through one result at a time and select it on the Nano. Landscape layout is used while typing.

Playing a selected result required a reliable request from the Nano back to the Mac. Early writes returned `0x1D8` and `0x19E`. Later builds restored search requests and added status and readiness messages, an explicit selection path, result artwork, and transfer progress. The thumbnail failures were distinct from decoding failures. Keeping artwork bounded and exchanging explicit capability and result records let the apps deal with missing images and different builds. The older Stream and VideoStream workflows led to the current file and link actions in the two media tabs. [Mac search and sender](tools/macos/nano_classic_sender.swift) · [Nano search and playback UI](apps/bluetoothtest/bluetoothtest.c).

### 6. Hardware H.264, display layers, and compression

Early video playback moved too many pixels and gave poor quality and frame rate. The Nano has a hardware H.264 decoder. The Mac now encodes compatible compressed samples with FFmpeg and sends those. The Nano hardware decodes them, so the Mac can prepare the next group while Bluetooth sends a finished one. Conversion still selects resolution and FPS, produces compatible keyframes and decoder configuration, and converts the audio. Protocol changes initially produced errors such as **Classic header has 112 bytes expected 60**, invalid transfer headers, and a failure to decode H.264. Matching the header format, advertising capabilities, and validating group lengths and checksums made those steps diagnosable. A transfer heartbeat also lets the Nano show preparation progress during a long Mac encode. [Mac H.264 encoder](tools/macos/nano_video_sender.swift) · [Video protocol and progress](sdk/hb_classic.c).

The built-in Videos app was a reference point for smooth playback. The LCD showed the movie while a screenshot showed a black video region with controls over it. That observation suggested a separate presentation path. Video Probe compared display behavior and exercised individual frames and short sequences. Some early attempts flickered. The working approach sends NV12 planes through the hardware layer and keeps UI controls on the Homebrew UI side. It reduces CPU color conversion and redraw work where the hardware path is available. Tests confirmed the project could present moving video on that layer. Apple’s exact internal buffering and scheduling are still partly inferred. Apple lists a 240 × 432 panel and stock H.264 support up to 720 × 576 at 30 FPS. That is a useful reference for the hardware, while sustainable Bluetooth playback also depends on bitrate, buffering, and our player. [Video Probe](apps/video_probe) · [Hardware video layer](sdk/hb_video_layer.c) · [Apple’s Nano specifications](https://support.apple.com/en-me/112039).

Full-resolution 432 × 240 at 30 FPS made decoder allocation failures easier to reproduce. Encoded data, decoded frames, display buffers, and hardware decoder scratch space compete for different parts of the heap. The decoder needs a sufficiently large contiguous region. A reboot can restore that region even when the previous session appeared to have enough total free RAM. Smaller frame groups, allocation headroom, the direct display path, independent FPS controls, and lower audio bandwidth each addressed part of the problem. A clip that stuttered **After Transfer** also showed that Bluetooth throughput alone could not explain every playback hitch. [Decoder and heap guards](sdk/hb_h264.c) · [Encoder quality presets](tools/macos/nano_video_sender.swift).

### 7. Transport stalls, packet timing, and audio bandwidth

Early live audio needed enough bandwidth to keep the playhead moving. IMA ADPCM reduced bytes per second compared with sending full PCM, and the video profiles gained independently decodable ADPCM blocks per group. Encoded audio lives in a receive ring until consumed. For video, the Nano grants credit as space becomes available. For audio, the Mac can pace packets and pause at a high-water mark until the Nano drains enough data. These limits protect the receiver while allowing a lead buffer. [Classic flow control](sdk/hb_classic.c) · [Audio encoding and pacing](tools/macos/nano_classic_sender.swift).

At 44.1 kHz mono, uncompressed 16-bit PCM takes about **5.29 MB per minute**. The 1,024-byte IMA ADPCM blocks in YT Music bring that to about **1.33 MB per minute**. At 22.05 kHz mono the same format takes about **0.66 MB per minute**, and 44.1 kHz stereo takes about **2.66 MB per minute**. These are encoded audio estimates before transport overhead, cover art, or video. [Audio size calculation](tools/macos/nano_classic_sender.swift).

Once Full Mono worked, the lower rates exposed a separate Mac scheduling bug. At the lower wire bitrates, a packet could represent more than 20 ms of audio. The old catch-up calculation moved its deadline forward on every retry, leaving the sender stuck before the first audio packet. The corrected calculation allows at least one packet to become due before limiting catch-up. A scheduling simulation passed all five quality presets, including short packets and an artificial delay. [Mac pacing loop](tools/macos/nano_classic_sender.swift).

### 8. Reboots, mixer ownership, and seamless handoffs

Repeated reboots gave the clearest evidence when one variable changed at a time. Audio-only runs sometimes rebooted near the same percentage, including a reproducible 59% case. Video could also reboot after a completed transfer, across quality settings. We logged transfer positions and mixer state to alternating text snapshots, took trace captures, and compared receiving alone, normal playback, silent video, mixer silence, and audio-only video tests. One silent run at **320 × 176, 22 FPS, 44.1 kHz mono** reached **3,369 / 3,369 frames** with the mixer bypassed. Together with the YT Music failures, that pointed at the audio backend shared by both apps. The built-in Music app was a reference for clean playback. The useful evidence came from comparing stock mixer ownership and callback behavior, and the exact stock media pipeline remains partly unmapped. [Video diagnostics](sdk/hb_h264.c) · [Classic audio diagnostics](sdk/hb_classic.c).

The first stable change was to treat submitted descriptors and buffers as immutable until RetailOS released them. Two 1.28-second blocks per batch reduced the gaps while leaving a recognizable **2.56-second** seam. The repeated constructor calls also turned out to allocate a small native object on every rebuild. The firmware destructor releases that object, so reusing or freeing descriptors correctly matters over a long track. Simply resetting a descriptor, or modifying the slot while the mixer still owns its source, creates a different failure.

The descriptor’s duration field feeds a counter in the mixer’s output rate. An incorrect duration can leave the voice counting after its source samples have run out, producing a patch of silence at the join. The refill probe used the mixer’s actual read cursor to verify the handoff. The player checks the active descriptor, reader, buffer bounds, and lead time before publishing a new link. [Mixer timing and descriptor notes](apps/entrain/platform/audio_device.c).

Audio Probe isolated the missing piece by tracking the live PCM reader and validating that it had moved to the next buffer before refilling another. In the successful run, a **22.05 kHz** tone feeding the **44.1 kHz** mixer observed **32/32 blocks**, completed **29/29 refills** without misses, and measured a longest refill of **590 µs**. The linked player uses that ownership check, stages decoded samples, and publishes a next pointer only once the target is ready. The implementation was added first to saved music, then to While Sending with partial input and rebuffering. [Audio Probe](apps/audio_probe) · [Linked PCM implementation](sdk/hb_linked_pcm.c).

Video then adopted a fixed descriptor pool because a live stream can start with an unknown frame count and the decoder needs most of the available memory. Video timing follows samples consumed by the mixer, so it holds position during a real underrun or pause and resumes from the same timeline. The model tests covered descriptor wraparound, pause, rebuffering, and the final short block. The full playback matrix across resolutions and both transfer modes still needs hardware coverage. [Video playback integration](sdk/hb_h264.c) · [Device mixer backend](apps/entrain/platform/audio_device.c).

### 9. Packaging, Home refresh, and launch failures

One GitHub push was rejected because it contained local firmware downloads and 2012/2015 firmware cache images above GitHub’s 100 MiB per-file limit. Those files are generated separately from app source. The release tooling validates a matched resident and firmware pair, then publishes the IPSWs and disk ZIP as release artifacts. [Release builder](tools/build_release.py) · [GitHub’s file-size limits](https://docs.github.com/en/repositories/working-with-files/managing-large-files/about-large-files-on-github).

Installable Nano apps have stable IDs, a relocatable executable, a checksummed manifest, and a separate Home icon. The custom music and video art initially filled the whole 112 × 112 tile. The packager now fits it to the stock disc footprint. When Home continued showing the previous artwork, we verified the new disk files, restarted the Nano, and confirmed both replacement images in the running Home bitmap cache. The rename to **YT Music** and **YT Video** preserved the stable app IDs. [App installer](tools/app_install.py) · [Icon packager](tools/build_apps.py) · [Home icon cache](sdk/hb_silver_icon.c).

Intermittent launches are a separate issue. The current build check measured compiled images of roughly **232 KiB** for YT Music and **259 KiB** for YT Video. Runtime allocation is larger because it includes zero-initialized buffers and loader headroom. A tap can also be rejected while the previous app still owns sound, decoder, or Bluetooth resources. A successful restart can clear either condition. The traces available so far showed successful launches with a 4 MiB contiguous free block. A trace of a failed tap can distinguish `APPBUSY` from `HEAVYNO` or a later entry failure. Capture it before restarting if the issue recurs. [Resident launcher](system/silver_resident/silver_resident.c) · [Relocatable app loader](sdk/hb_reloc_file.c).

```sh
./start --local trace 96
./start --local trace 96 --previous
```

### 10. Reproducing and extending the work

For an audio glitch, compare a single track at the same quality in both playback modes and record whether the Mac is still sending. For a video glitch, keep the clip, FPS, resolution, and audio settings fixed and compare While Sending with After Transfer. The probes let us isolate the mixer and display from Bluetooth. Useful device artifacts include `Data/YouTubeAudio-playback.txt`, `Data/YouTubeAudio-playback-backup.txt`, `Data/NativeVideo-decoder.txt`, `Data/VideoStream-end.txt`, and the alternating `Data/YouTubeStreamer-heap-*.txt` snapshots. Diagnostic filenames retain the names used by earlier builds.

The sender protocol separates Bluetooth transport from media encoding and Nano playback. That made a Raspberry Pi companion a plausible direction. The Mac implementation uses Core Bluetooth and IOBluetooth. A Pi client would need its own device discovery, pairing, and L2CAP integration while carrying the same media records. The Mac app is the current release companion. [Mac transport and sender](tools/macos/nano_classic_sender.swift).

[Licenses and upstream credits](LICENSE).
