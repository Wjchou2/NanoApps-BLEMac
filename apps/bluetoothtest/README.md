# Bluetooth discovery on the iPod nano 7

The NanoApps SDK has no discovery or device-list API. The resident's Bluetooth
fix adjusts A2DP audio timestamps after connection. The stock iPod already
searches for accessories in **Settings → Bluetooth**, which gives us a firmware
path to trace.
[Apple's iPod nano user guide](https://support.apple.com/en-us/docs/ipod/133772)

## Test Bluetooth on hardware

Install the ZIP built by `./start package bluetoothtest` on a NanoApps iPod
running firmware 1.1.2 (`39A10023`).

- **Scan Classic** runs the former **Scan nearby** test. Turn on Bluetooth
  in Settings and put a Classic accessory in pairing mode. The app requests
  Classic inquiry with the firmware's any-service mask and lists named devices
  from the live results. The firmware owns the scan through completion, even
  when you leave the app. If it asks you to open Settings first, visit
  **Settings → Bluetooth** once to initialize the client.
- **Scan BLE** requests discovery through the stock BLE session with an
  empty service filter. Turn on Bluetooth in Settings and make a BLE
  accessory advertise. Tap **Scan BLE** and wait five seconds. The app takes
  snapshots during the scan and lists up to 24 named devices after the scan.
  Rows include the BLE address and a type hint when a recognized service
  is available. If the session is still opening, tap again.
- **Connect** opens a BLE link, discovers GATT services and characteristics,
  and attempts to subscribe to the Nordic UART TX characteristic. Set up the
  nRF Connect server before tapping **Connect**. The app displays the last
  notification as up to 64 printable ASCII characters. Tap **Disconnect**
  before leaving the app. The test also closes after 60 seconds without a
  notification while Bluetooth Test is on screen.

### Send text from nRF Connect on iOS

1. In **Peripheral → Add Server**, create and start a server with a primary
   service UUID `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`.
2. Add a characteristic with UUID `6E400003-B5A3-F393-E0A9-E50E24DCCA9E`
   and enable **Notify**. Keep your named, connectable advertiser running.
3. On the iPod, tap **Scan BLE**, select the iPhone's advertised name, and
   tap **Connect**. Wait for **Subscribed**.
4. In the iPhone server view, send a characteristic notification with
   `Hello` or the hex bytes `48 65 6C 6C 6F`. Bluetooth Test should show
   **Received** and the message.

The BLE link test worked on hardware. The GATT discovery and notification
receiver still need an end-to-end hardware run.

The earlier **BLE Test** could read the controller state, but it usually
showed an empty cache after returning from Fitness. The firmware clears the
cache when the scan stops. **Scan BLE** copies addresses before requesting
the stop, so the rows remain visible after the firmware clears its cache.

## Discovery tables from the earlier diagnostic build

**Export discovery tables** produced
`Data/bluetoothtest/39A10023-discovery-tables.bin`. The diagnostic staged the
tables in a 512-byte buffer and wrote it in one call.

The file starts with `BTD1`, a little-endian 32-bit region count, and one
pair of 32-bit values per region giving its runtime address and length. The
region bytes follow in order. These tables cover discovery callbacks,
Classic inquiry methods, the discovery agent and its result events, and the
discovery manager. Use them with the earlier firmware code export.
The Classic inquiry classifier dispatches through slot `+0x18` of the
discovery manager's secondary method table, at `0x087ec7d0`.

## Firmware exports from the diagnostic builds

**Export firmware code** produced
`Data/bluetoothtest/39A10023-text.bin`, a 6,813,924-byte raw snapshot from
`0x08000000` up to the
SDK's legacy app slot at `0x0867f8e4`. It captures live code, which can
include active NanoApps patches. The app reads that region after checking a
known firmware signature and writes one file to the user volume.

**Export scan details** produced
`Data/bluetoothtest/39A10023-scan-tables.bin`, an 880-byte file containing
four fixed firmware tables. The file begins with the
ASCII magic `BVT1`, a little-endian 32-bit table count, and four pairs of
32-bit source addresses and byte lengths. The table bytes follow in order.

| Table | Source address | Length |
| --- | --- | --- |
| Bluetooth client | `0x087ed34c` | `0xc0` |
| State query used by Settings | `0x087ef1e0` | `0x28` |
| Bluetooth device list model | `0x087b57e0` | `0xa0` |
| Bluetooth Settings controller | `0x087db260` | `0x1c0` |

## Firmware evidence

The stock 1.1.2 (`39A10023`) MSE contains an encrypted `osos` image. Its IMG1
header starts at file offset `0x0aeb5000`, with format byte `0x03`. The
`rsrc` section is readable as FAT starting at `0x003b2400` in the same file.
I extracted `/Resources/UI/SilverDB.en_GB.LE.bin` and decoded it with
[silverutil](https://github.com/spotlightishere/silverutil). The German database
uses the same IDs for the corresponding translated strings.

| Stock string | Resource ID | Little-endian bytes |
| --- | --- | --- |
| Searching | `0x0dad11e7` | `e7 11 ad 0d` |
| Searching Disabled | `0x0dad11e8` | `e8 11 ad 0d` |
| Devices | `0x0dad11e9` | `e9 11 ad 0d` |
| Not Paired | `0x0dad11f3` | `f3 11 ad 0d` |
| Forget this Device | `0x0dad11f4` | `f4 11 ad 0d` |

## Findings from the runtime export

The supplied file has the expected length and the active A2DP timestamp patch
at `0x082b2bd6`. Its SHA-256 is
`057b2a240574115559d1fce1e93bf6fc083e2602278dfb28cfc2851ccd00e9a0`.
Addresses below use the runtime base `0x08000000`.

| Firmware address | Evidence in the disassembly |
| --- | --- |
| `0x081ef9fe` | Controller registration references `TSilverSettingsBluetoothCntlr` and loads its factory pointer `0x082b0e1b`. The factory enters the constructor at `0x082b16ec`. |
| `0x08416ed0` | Fetches the Settings device model from the singleton slot at `0x08812d7c`, allocating it if absent. The app reads the slot directly to avoid allocating the model. |
| `0x082901dc` and `0x08290202` | Stock device count and row name readers. Both acquire the mutex at model offset `0x70`. The row reader gets the row count at `+0x64`, starts iteration at `+0x50`, and reads the name pointer through the slot at row offset `+0x10`. |
| `0x082bf72c` | Logs inquiry periods in 1.28-second units and calls `0x080f2f3c` with the stock inquiry parameters and an asynchronous callback at `0x082c2137` (Thumb). |
| `0x0843559c` | A device-found event calls `0x082b77ec`, which references the `inquiryResult - device` log. |
| `0x083d1db2` and `0x083d1ed4` | Settings entry and exit wrappers. They use the type-1 Bluetooth client from `0x08416e94`. The entry wrapper passes service mask 24 to client slot `+0x44`. |
| `0x08455cd0` | Client slot `+0x3c` leads to `0x082b6f88`, which logs the local discoverability change. |
| `0x08455c2c` | Client slot `+0x44` returns immediately if the agent at client offset `+0x88` already exists. Otherwise it creates an agent with stock callbacks and submits discovery mode 0 through `0x080cd280`. |
| `0x08455fdc` | The stock agent callback handles completion by releasing the agent with `0x080cd1ce` and clearing client offset `+0x88`. |
| `0x08417068` and `0x080cd648` | The first queries module power for the same client type that Settings uses. The second queries DUT mode, which blocks the scan routine. |
| `0x084336ec` | The discovery agent tests the service mask at `+0x50`. A mask of `0xffffffff` takes the early success path at `0x0843370c` and skips the profile checks. The tables export resolves the classifier call through `0x084b6f1c` and `0x08438974`. |
| `0x080cd1e4` | Reads device handles from a discovery agent. The stock caller at `0x08196d60` passes the agent handle, output array, count pointer, and array capacity. Bluetooth Test holds the client mutex used by the completion callback while taking the snapshot. |
| `0x080ccf5c` and `0x080ccdb4` | Copy a name or a formatted address into a caller-supplied buffer. Stock call sites at `0x08452f5e` and `0x0814e446` supply the device handle, output buffer, and buffer size. |

The earlier Settings-based scan was verified on hardware. The direct Classic
result reader and BLE test still need an iPod run.

## BLE firmware evidence

| Firmware address | Evidence in the disassembly |
| --- | --- |
| `0x0846e0d0` | The Fitness heart-rate data source fetches the stock BLE session. |
| `0x083d57c0` | Gets the Fitness BLE session owner at `0x08909180`, constructing it if needed. The native session is at owner offset `+0x9c` under the mutex at `+0xa0`. |
| `0x084596d8` | The stock session requests a scan through LE Central at `0x0843aa00`, passing the central object, native session, UUID vector, duplicate flag, and foreground flag. |
| `0x0843aef8` | Removes that native session's scan request and reconciles the radio state. The stop callback at `0x08439288` clears cached advertising reports. |
| `0x084063ae` | Yields the current task for a duration supplied in microseconds. The Classic inquiry code calls it at `0x082b7d40` with 250,000 µs. |
| `0x083df960` | LE Central singleton slot at `0x089702fc`. Its constructor installs the vtable at `0x087ec870`. |
| `0x08438f18` | The advertising report handler takes the mutex at `+0x18` and inserts entries into the device-keyed tree at `+0x70`. The tree sentinel is at `+0x80` and count at `+0x84`. |
| `0x08439030` | Stores advertising and scan-response objects in three slots reached through the cache node at `+0x14`. The stock formatter at `0x084ad2a4` reads the name at report offset `+0x0c` and the 18-byte UUID vector at `+0x38`. |
| `0x08438efa` | Reads the scanning flag at `+0x3e` under the same mutex. |
| `0x083d5c18` | Looks up a device by its seven-byte typed address. The Connect test uses the existing device registry at `0x089702a4`. |
| `0x0846f800` | Fitness activates the stock BLE session through virtual slot `+0x10` with mode 3 and restores mode 2 when finished. The connection wrapper checks session readiness through slot `+0x38` before submitting a request. |
| `0x084595e6` and `0x084152d8` | Request a connection through the stock BLE session and query the controller's connected-device map. |
| `0x082b3d40` | Releases the BLE session's request for a device. The stock disconnect wrapper calls this routine at `0x08459822`. |
| `0x083f069a` | The report handler uses this routine to copy the device address type and six address bytes from `+0x1a`. |

## Trace the stock discovery flow

1. Start with the runtime export above or an already decrypted `39A10023`
   OSOS image. For a complete stock image, confirm the address checks with
   `python tools/analyze_nano7_firmware.py /path/to/osos.img1` from the repo
   root, in a Python environment with Capstone. Use `--raw` for a raw OS body.
   A runtime export can omit or contain patched bytes at some of those addresses.
2. Import the runtime export or raw OS body into Ghidra as little-endian ARMv7
   at `0x08000000`. For a decrypted IMG1, the body starts at file offset
   `0x400`. Decode the stock routines in Thumb mode where their entry points
   indicate it.
3. Search for `0x0dad11e7` and `0x0dad11e8` as scalars and as little-endian
   bytes. Look for code that constructs an ID with `MOVW`/`MOVT` and for
   references to tables containing the IDs. Follow cross-references into the
   Bluetooth settings controller. Use the device-row strings to find where
   scan results enter its list model.
4. Trace what starts and stops discovery, where the results arrive, and which
   task owns the device list. Confirm the object and argument layout from stock
   callers before adding an SDK wrapper. Compare the first read-only list on
   hardware with Settings using a known accessory in pairing mode. Exercise
   Bluetooth off, repeated scans, and leaving the app during a scan.

Classic inquiry requires devices to be discoverable. BLE advertising uses a
separate discovery path.
[Bluetooth Core Specification](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-60/out/en/host/generic-access-profile.html)
