# NanoApps Bluetooth and H.264 SDK

These C modules use the iPod nano 7 firmware 1.1.2 (`39A10023`) interfaces. In an LVGL app, set `LV_SURFACE := 1` and enable the modules you need before including `sdk/hb_app.mk`.

| Makefile option | What it enables |
| --- | --- |
| `HB_BLE := scan` | Named BLE peripheral discovery |
| `HB_BLE := uart` | BLE discovery and Nordic UART notifications |
| `HB_CLASSIC := raw` | Bluetooth Classic pairing and raw L2CAP packets |
| `HB_CLASSIC := inbox`, `audio`, `video`, `youtube_audio`, or `youtube_video` | One Nano UART Sender transfer profile |
| `HB_H264 := 1` | Full-screen hardware H.264 playback from a file or a live stream. The Classic video profiles enable it automatically. |

## BLE

Include [`hb_ble.h`](sdk/hb_ble.h).

- `hb_ble_scan(devices, &count, filter)` scans for up to five seconds and fills an array of `HB_BLE_SCAN_LIMIT` entries. Each entry has a name, a typed address, and a device type hint when available. Pass `NULL` to accept all named results. A custom filter runs under the scan lock, so keep it short and avoid Bluetooth and UI calls inside it.
- `hb_ble_connect(address)` connects to Nordic UART TX and handles Nano UART Sender text and audio. `hb_ble_connect_notifications(address)` subscribes to the same characteristic and exposes the raw notification bytes.
- `hb_ble_poll(&update)` advances discovery and reads the latest notification. Run it on a UI timer using the returned `update.poll_ms` interval. Raw updates contain up to 64 bytes in `update.data`, with the byte count in `update.length`. `hb_ble_connected()` reports the link state and `hb_ble_ready()` reports when the TX subscription is active.
- `hb_ble_disconnect()` closes the link. In the app's unload hook, call `hb_ble_prepare_unload()` until it returns nonzero.

## Bluetooth Classic

Include [`hb_classic.h`](sdk/hb_classic.h). The Nano listens for an incoming L2CAP connection from one specified peer. Supply its six-byte Classic address in display order.

- **Raw packets.** Call `hb_classic_listen(psm, peer, callback, context)`. Once `hb_classic_connected()` is true, use `hb_classic_outgoing_mtu()` to size packets and `hb_classic_send(data, length)` to send them from the UI task. The receive callback runs on the Bluetooth task. Copy its packet before returning and process the copy on the UI task.
- **Mac sender profiles.** Call `hb_classic_listen_media(peer)` and run `hb_classic_poll()` on a UI timer to advance the transfer and playback. The `audio`, `video`, `youtube_audio`, and `youtube_video` profiles also need `hb_classic_init_media()` at app entry, before installing the app's unload hook. The `inbox` profile saves files to `/Data/ClassicInbox`.
- **Pairing.** After listening, call `hb_classic_enable_pairing()` for a new peer. Use `hb_classic_pair_code(&code)` to get the confirmation code and `hb_classic_pair_reply(code, confirm)` to pass on the user's decision. `hb_classic_confirmed_peer(address)` retrieves the confirmed address.
- **YouTube Audio.** Use `hb_classic_search_ready()` before calling `hb_classic_search_submit(query)`. Read the result list with `hb_classic_search_results(&list)`, then call `hb_classic_search_play(list.revision, index)` for the selected track. `hb_classic_search_cover()` retrieves cover art.
- **Cleanup.** Call `hb_classic_disconnect()` to stop the listener or connection. In the unload hook, call `hb_classic_prepare_unload()` until it returns nonzero.

## Hardware H.264 playback

Include [`hb_h264.h`](sdk/hb_h264.h). Call these methods on the UI task.

- **Saved clip.** Call `hb_h264_play_file("/Data/clip.nvh")` with an NVH1–NVH5 clip produced by Nano UART Sender. It opens the full-screen player and preserves the source file. A non-NULL return is an error message.
- **Live playback.** Call `hb_h264_init()`, then `hb_h264_live_begin(config, size, count, fps, rate, channels)` with the AVCC decoder configuration. When `hb_h264_live_room()` is true, feed one complete encoded group with `hb_h264_live_submit(data, length)`. Each group starts with a keyframe and contains up to 12 length-prefixed H.264 samples followed by interleaved μ-law audio.
- **Progress and completion.** Run `hb_h264_live_tick()` on a UI timer. It returns `0` while active, `1` when playback ends or is cancelled, and `-1` on error. Use `hb_h264_live_error()` for the error message and `hb_h264_live_buffer_ms()` for buffered playback time. For an unknown frame count, pass `0` at start and call `hb_h264_live_finish()` after submitting the final group.
- **Cleanup.** Call `hb_h264_stop()` to close playback. Standalone H.264 apps with their own unload hook should call `hb_h264_prepare_unload()` until it returns nonzero. The Classic video profiles handle decoder and audio cleanup through their existing app lifecycle.

## Test apps

Build the Nano apps from the repository root.

```sh
./start build bluetoothtest stream youtubeaudio videostream youtubestreamer audio_player
```

Run `./start package APP` with a target from the table to create its installable ZIP. Use [Nano UART Sender](downloads/Nano%20UART%20Sender.app) on the Mac for the transfer and streaming tests.

| Nano app | Build target | What to test |
| --- | --- | --- |
| [Bluetooth Test](apps/bluetoothtest) | `bluetoothtest` | Scan for a named BLE peripheral, connect, and receive Nordic UART text or audio. For Classic, pair with the Mac and send a file from **Classic Files**. |
| [Stream](apps/stream) | `stream` | Stream audio over Classic from the Mac's **Stream** tab. |
| [YouTube Audio](apps/youtubeaudio) | `youtubeaudio` | Search on the Nano, select a track, and stream its audio through the Mac's **YouTube Audio** tab. |
| [VideoStream](apps/videostream) | `videostream` | In the Mac's **Classic Files** tab, choose **VideoStream** as the destination and click **Send Video…** to test full-screen H.264 playback, including playback during transfer. |
| [YouTube Streamer](apps/youtubestreamer) | `youtubestreamer` | Send a YouTube video link from the Mac's **YouTube Streamer** tab and test H.264 playback on the Nano. |
| [Audio Player](apps/audio_player) | `audio_player` | Open a WAV received by Bluetooth Test from the Classic inbox. |
