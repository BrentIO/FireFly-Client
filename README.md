# FireFly Client

Firmware for the ESP8266-based FireFly Client device. This is a rewrite of [FireFly_Client](https://github.com/BrentIO/FireFly_Client) and works in conjunction with [FireFly-Controller](https://github.com/BrentIO/FireFly-Controller).

## Hardware

The client device is built on an **ESP8266** microcontroller. Each unit supports up to 6 buttons/switches via two RJ-45 jacks (4 channels on the primary, 2 on the secondary/extended). Button LEDs provide feedback. Devices are powered over the Cat6 cable from the Controller.

### RJ-45 Pinout

| Pin | Wire Color | Usage |
|-----|------------|-------|
| 1 | White/Orange | Channel 1 |
| 2 | Orange | Channel 2 |
| 3 | White/Green | Channel 3 |
| 4 | Blue | +12VDC |
| 5 | White/Blue | +12VDC |
| 6 | Green | Channel 4 |
| 7 | White/Brown | Ground |
| 8 | Brown | Ground |

The secondary jack uses only pins 1, 2, 7, and 8 (channels 5–6, power, ground).

### Communication

Clients communicate with the Controller over the Cat6 cable using a **non-IP protocol** — the RJ-45 connector carries power and direct channel signals, not Ethernet. WiFi is used only during initial provisioning and for OTA firmware updates.

## Flash Layout

The device uses a **4MB flash** chip configured with the standard Arduino ESP8266 `4M2M` layout (4MB flash, 2MB filesystem). This was confirmed from the original firmware binary and the original repo's flashing instructions.

| Address | Size | Contents |
|---------|------|----------|
| `0x000000` | 4KB | eboot (second-stage bootloader) |
| `0x001000` | 1019KB | App slot 0 (active sketch) |
| `0x101000` | 1019KB | App slot 1 (OTA staging) |
| `0x200000` | ~2MB | SPIFFS / LittleFS filesystem |
| `0x3FB000` | 4KB | RF calibration data |
| `0x3FC000` | 4KB | PHY init data |
| `0x3FE000` | 8KB | WiFi credentials / SDK config |

### Original firmware binary analysis

Running `esptool image_info` on the binary from the original repo confirms:

```
Image size:  398,992 bytes
Image type:  ESP8266
Flash size:  4MB
Flash freq:  40MHz
Flash mode:  DIO
Entry point: 0x4010f45c
```

The binary is the combined eboot + application image. `image_info` reports only the eboot portion (a single 3.5KB IRAM segment); the remaining ~390KB is the `irom0.text` application code that runs directly from the flash cache.

### Original flashing command

From the original repo, bootloader and SPIFFS were flashed together:

```bash
python3 esptool.py --port {PORT} --baud 115200 write_flash \
    0x00000 {BOOTLOADER_FILE} \
    0x00200000 {SPIFF_FILE}
```

### Arduino board settings

| Setting | Value |
|---------|-------|
| Board | Generic ESP8266 Module |
| Flash Size | 4MB (FS:2MB OTA:~1019KB) |
| Flash Mode | DIO |
| Flash Frequency | 40MHz |
| CPU Frequency | 80MHz |

## Development Environment

- **ESP8266 Arduino core:** 3.1.2
- **Board manager URL:** `https://arduino.esp8266.com/stable/package_esp8266com_index.json`

## Provisioning

When unprovisioned, the client scans for a WPA2 SoftAP named `FireFly-Provisioning` broadcast by the Controller. The WPA2 password is derived deterministically from the Controller's BSSID using a nibble-interleave algorithm: for each index `i` (0–5), take the upper nibble of `BSSID[i]` and the lower nibble of `BSSID[5-i]`, concatenated as uppercase hex.

Example: BSSID `A1:B2:C3:D4:E5:F6` → Password `A6B5C4D3E2F1`

Once connected, the client:
1. Calls `GET /api/provisioning/nonce` (no auth)
2. Calls `GET /api/provisioning/client` with its MAC address and the nonce
3. Stores the returned configuration (WiFi credentials, MQTT broker, OTA URL) to persistent storage
4. Reboots into normal operating mode

## OTA Updates

The client checks for firmware updates by calling:

```
GET /ota/{class}/{product_hex}?current_version={version}
```

Updates are sequential — the device always installs the next available version, never skips. Version format is `YYYY.MM.bb` (e.g., `2026.03.01`).

| Response | Meaning |
|----------|---------|
| `200` | Next version available (or already current — compare versions) |
| `400` | Missing `current_version` parameter |
| `404` | No released firmware exists for this device |
| `409` | Running revoked firmware with no newer release available |

The OTA manifest response:

```json
{
  "type": "FireFly Client",
  "version": "2026.03.01",
  "url": "https://firmware.example.com/Client/2026.03.01/Client.ino.bin"
}
```

## Migration from v1.14 Firmware

Production units running firmware v1.14 can be migrated to this firmware without physical access to the devices. The eboot bootloader at `0x000000` and the partition layout are never touched by OTA — they are preserved across all updates.

### How v1.14 stores configuration

All device configuration in v1.14 (WiFi credentials, MQTT settings, LED config) is stored in **EEPROM**, not SPIFFS. The SPIFFS partition contains only the provisioning web UI assets. This means pushing new firmware leaves all existing config intact — EEPROM survives OTA completely.

The v1.14 SPIFFS and the new firmware's SPIFFS/LittleFS are not compatible formats, but since no user config lives there, the filesystem can be left as-is or overwritten without consequence.

### Two OTA paths available in v1.14

**For provisioned devices** (connected to your WiFi network): publish the new firmware binary URL to the device's MQTT topic. The device fetches and applies it immediately:

```
Topic:   aveo/client/{Device ID}/firmware/set
Payload: https://your-server.com/path/to/Client.ino.bin
```

**For unprovisioned devices** (showing the `FireFly-{MAC}` hotspot): connect to the hotspot, navigate to `http://192.168.1.1/update`, and upload the compiled `.bin` via the Firmware tab.

### Discovering devices via MQTT

v1.14 devices publish a health payload automatically on every boot and every 28 minutes thereafter. The `{clientTopic}/status` message is registered as a **retained LWT**, so subscribing to `#` on your MQTT broker will immediately reveal the topic path of every device that has ever connected — including ones currently offline.

To discover all devices:
```
Subscribe: aveo/client/#
```

To request an immediate health update from a specific device:
```
Publish to: aveo/client/{Device ID}/health/get
Payload:    (any value)
```

The device responds by publishing:

| Topic | Payload |
|-------|---------|
| `aveo/client/{Device ID}/status` | `ONLINE` |
| `aveo/client/{Device ID}/ip` | Device IP address |
| `aveo/client/{Device ID}/firmware` | Firmware version (e.g. `1.14`) |
| `aveo/client/{Device ID}/deviceName` | MAC-derived device name |
| `aveo/client/{Device ID}/name` | Friendly name |
| `aveo/client/{Device ID}/uptime` | Milliseconds since last boot |
| `aveo/client/{Device ID}/errorMessage` | Empty if healthy |

### Re-provisioning after migration

The v1.14 EEPROM config format is incompatible with the new firmware's provisioning model. On first boot after the firmware update, the new firmware will not find valid configuration and must enter provisioning mode automatically, creating a SoftAP and waiting for the Controller to provision it via the standard `FireFly-Provisioning` flow.

This requires a one-time walk-around: for each wall-mounted device, bring the Controller within 3–5 feet (the Controller's provisioning SoftAP runs at 2 dBm) and trigger provisioning from the Controller's web UI.

### Migration checklist

1. Compile new firmware for `Generic ESP8266 Module`, 4MB, DIO, 40MHz, `4M2M`
2. Push firmware to provisioned devices via MQTT, or to unprovisioned devices via `http://192.168.1.1/update`
3. Device reboots, detects no valid config, enters provisioning mode
4. Bring Controller within range of each device and provision via Controller web UI
5. Device stores new config and reboots into normal operating mode
6. Confirm device connects to WiFi, MQTT, and begins checking Cloud OTA endpoint

