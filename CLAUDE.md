# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

FireFly-Client is ESP8266 Arduino firmware for FireFly Input devices. It provisions from a FireFly-Controller over HTTPS, then communicates via MQTT — subscribing to input state events and driving LED outputs accordingly. It is a rewrite of the legacy FireFly_Client.

## Repository

- License: MIT (Copyright 2026 Brent I/O)
- Remote: https://github.com/BrentIO/FireFly-Client.git
- GitHub issues are filed here and in the companion repo `BrentIO/FireFly-Controller`

## Branching and PRs

- Always work on a feature branch — never commit directly to `main`
- Always open a PR (`gh pr create`) when ready for review
- Never approve or merge your own PRs

## Structure

```
Client/
  Client.ino       # Main firmware (Arduino sketch)
  asyncapi.yaml    # AsyncAPI 3.1.0 spec for all MQTT channels
common/
  hardware.h       # Hardware abstraction — selects device header via PRODUCT_HEX
  devices/
    FFI0600-2011.h # GPIO defines for FireFly Input 6-ch ESP8266 (Nov 2020)
```

## Build System

This project uses **Arduino CLI** with a GitHub Actions CI/CD pipeline. Board and partition details are defined in `devices.yaml`; library/core versions are in `libraries.yaml`.

The key build flag is:

```
-DPRODUCT_HEX=0x06002011
```

`PRODUCT_HEX` is derived from the product ID by stripping the prefix (`FFI`/`FFC`), removing the dash, and treating the remaining digits as a hex literal. Example: `FFI0600-2011` → `0x06002011`.

`hardware.h` uses `#if PRODUCT_HEX == 0x...` to include the correct device header from `common/devices/`. Adding a new hardware variant requires a new header there, a matching `#if` block in `hardware.h`, and a new entry in `devices.yaml`.

## Key Architecture

**Provisioning flow:** On first boot (no config), the device scans for the `FireFly-Provisioning` SoftAP. The AP password is derived from the Controller's BSSID via nibble-interleave (12 hex chars). Once connected to `192.168.4.1`, provisioning runs three HTTP steps over plain HTTP (no TLS on the SoftAP):

1. `POST /api/provisioning/token` — body `{uuid, mac_address}` (MAC lowercase) → `{token}` (uint32_t, passed as decimal string)
2. `GET /api/clients/{uuid}` — `provisioning-token: {token}` header → client config JSON, saved to `/config.json`
3. `GET /api/provisioning/certs` — `provisioning-token: {token}` header → `{fingerprint, pem}`, saved to `/ca.pem` + `/ca_fp.txt`; 404 means no cert is designated and the device continues without TLS

After step 3 the device disconnects and reboots into normal operation.

**HID/LED mapping:** The provisioning response includes a top-level `id` field (the `portId` shared by all channels on this client) and a `hids` object keyed `"1"`–`"6"` (1-indexed strings). The key equals the `channelNumber` used in MQTT topics. Key − 1 is the 0-based index into `LED_PINS[]`. Each `hids` entry may include `defaultBrightness` (1–100, defaults to 100) and a `tags` array. Slots with no entry are left unconfigured (empty `portId`) and skipped in all loops.

**MQTT topics (subscribe):**
- `FireFly/inputs/{portId}/channels/{channelNumber}/state` — input state per HID
- `FireFly/{uuid}/leds/brightness/set` — per-device brightness command (0–100)
- `FireFly/clients/leds/brightness/set` — broadcast brightness command (0–100)
- `FireFly/clients/cert/state` — retained cert broadcast (JSON: `fingerprint` + `pem`)
- `FireFly/tag/{tagName}/set` — tag LED animation command (one topic per unique tag)

**MQTT topics (publish):**
- `FireFly/{uuid}/availability` — `online`/`offline` (retained, LWT)
- `FireFly/{uuid}/cert/state` — current cert fingerprint (retained)
- Various diagnostic and HA autodiscovery topics (see `asyncapi.yaml`)

**Input state → LED behavior:**
| State | LED action |
|-------|-----------|
| `SHORT` | Save brightness, turn off |
| `LONG` | Flash full brightness 100 ms, then hold at half of saved brightness (5% floor) until NORMAL |
| `NORMAL` | Restore saved brightness |
| `EXCESSIVE` | Blink 3×100ms on/off, then restore saved brightness |

**Certificate handling:** `certPem` and `certFingerprint` are global `String`s. Each HTTPS call creates a fresh `WiFiClientSecure` and calls `setCACert(certPem.c_str())`. There is no global `WiFiClientSecure` instance. Cert rotation arrives via the retained `FireFly/clients/cert/state` broadcast so offline devices receive it on reconnect.

**MQTT buffer:** 4096 bytes to accommodate the cert JSON payload.
