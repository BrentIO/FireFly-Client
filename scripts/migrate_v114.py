#!/usr/bin/env python3
"""
migrate_v114.py — Migrate all v1.14 FireFly Client devices to new firmware over MQTT.

Three-phase operation:
  Phase 1 — Discovery: subscribe to aveo/client/#, collect retained status payloads,
             build a device registry, write devices.json.
  Phase 2 — Upgrade: publish firmware URL to each pending device.
  Phase 3 — Monitor: watch for status changes and update devices.json until all
             devices reach a terminal state.

Requires: paho-mqtt
"""

import argparse
import json
import os
import sys
import time
import threading
from datetime import datetime, timezone

import paho.mqtt.client as mqtt


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
TOPIC_ROOT = "aveo/client"
SUBSCRIBE_WILDCARD = "aveo/client/#"

TERMINAL_STATUSES = {"COMPLETE", "FAILED", "NO_RESPONSE"}
NO_RESPONSE_TIMEOUT_SECS = 300  # 5 minutes after SENT

DEVICES_FILE = "devices.json"

RETAINED_SUBTOPICS = {"status", "ip", "firmware", "name", "uptime", "errorMessage"}


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def utcnow_iso() -> str:
    """Return current UTC time as an ISO 8601 string."""
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def mac_from_device_id(device_id: str) -> str:
    """Convert a 12-char hex device ID to a colon-separated lowercase MAC address."""
    d = device_id.lower()
    return ":".join(d[i:i+2] for i in range(0, 12, 2))


def load_devices_file(firmware_url: str) -> dict:
    """Load an existing devices.json or return a fresh structure."""
    if os.path.exists(DEVICES_FILE):
        with open(DEVICES_FILE, "r", encoding="utf-8") as fh:
            data = json.load(fh)
        print(f"Loaded existing {DEVICES_FILE} with {len(data.get('devices', []))} device(s).")
        return data
    return {
        "generated": utcnow_iso(),
        "firmware_url": firmware_url,
        "devices": [],
    }


def save_devices_file(data: dict) -> None:
    """Write the full devices.json to disk."""
    with open(DEVICES_FILE, "w", encoding="utf-8") as fh:
        json.dump(data, fh, indent=2)


def find_device(data: dict, device_id: str) -> dict | None:
    """Return the device entry dict for device_id, or None."""
    for dev in data["devices"]:
        if dev["device_id"] == device_id:
            return dev
    return None


def print_table(devices: list) -> None:
    """Print a formatted table of discovered devices."""
    if not devices:
        print("  (no devices discovered)")
        return
    header = f"{'Device ID':<16}  {'Name':<30}  {'IP':<16}  {'Firmware':<10}  {'Status'}"
    print(header)
    print("-" * len(header))
    for dev in devices:
        print(
            f"{dev['device_id']:<16}  "
            f"{dev.get('name', ''):<30}  "
            f"{dev.get('ip', ''):<16}  "
            f"{dev.get('firmware_version', ''):<10}  "
            f"{dev['status']}"
        )


# ---------------------------------------------------------------------------
# Phase 1 — Discovery
# ---------------------------------------------------------------------------

def run_discovery(args) -> dict:
    """
    Connect to the broker, collect retained payloads from aveo/client/#,
    build the initial device registry, and return the data dict.

    If devices.json already exists on disk, devices that are already at a
    terminal status are preserved and skipped.
    """
    print(f"\n=== Phase 1: Discovery (timeout {args.discovery_timeout}s) ===")

    # Staging area: device_id -> {field: value}
    raw: dict[str, dict] = {}
    lock = threading.Lock()
    discovery_done = threading.Event()

    def on_connect(client, userdata, flags, rc, properties=None):
        if rc != 0:
            print(f"Connection failed with code {rc}", file=sys.stderr)
            sys.exit(1)
        print("Connected to broker. Subscribing to aveo/client/#...")
        client.subscribe(SUBSCRIBE_WILDCARD)

    def on_message(client, userdata, msg):
        topic = msg.topic  # e.g. aveo/client/F4CFA2E416A6/status
        parts = topic.split("/")
        # Expected: aveo / client / <device_id> / <subtopic>
        if len(parts) < 4:
            return
        device_id = parts[2]
        subtopic = parts[3]
        if subtopic not in RETAINED_SUBTOPICS:
            return
        payload = msg.payload.decode("utf-8", errors="replace").strip()
        with lock:
            if device_id not in raw:
                raw[device_id] = {}
            if subtopic == "firmware":
                raw[device_id]["firmware_version"] = payload
            elif subtopic == "name":
                raw[device_id]["name"] = payload
            elif subtopic == "ip":
                raw[device_id]["ip"] = payload

    client = _build_client(args)
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(args.broker, args.port, keepalive=60)
    client.loop_start()

    print(f"Listening for {args.discovery_timeout} second(s)...")
    time.sleep(args.discovery_timeout)

    client.loop_stop()
    client.disconnect()

    # Build / merge with any existing devices.json
    data = load_devices_file(args.firmware_url)
    existing_ids = {dev["device_id"] for dev in data["devices"]}

    with lock:
        for device_id, fields in raw.items():
            if device_id in existing_ids:
                # Update fields on existing entry but do not reset a terminal status
                dev = find_device(data, device_id)
                if "mac_address" not in dev:
                    dev["mac_address"] = mac_from_device_id(device_id)
                for k, v in fields.items():
                    dev[k] = v
            else:
                data["devices"].append({
                    "device_id": device_id,
                    "mac_address": mac_from_device_id(device_id),
                    "name": fields.get("name", ""),
                    "ip": fields.get("ip", ""),
                    "firmware_version": fields.get("firmware_version", ""),
                    "status": "PENDING",
                    "status_updated": utcnow_iso(),
                })

    save_devices_file(data)

    print(f"\nDiscovered {len(data['devices'])} device(s):")
    print_table(data["devices"])
    return data


# ---------------------------------------------------------------------------
# Phase 2 — Upgrade
# ---------------------------------------------------------------------------

def run_upgrade(args, data: dict, client: mqtt.Client | None) -> None:
    """
    Publish firmware URL to each PENDING device and mark it SENT.
    In dry-run mode, prints what would be sent without publishing anything.
    """
    dry_run = args.dry_run
    label = " [DRY RUN]" if dry_run else ""
    print(f"\n=== Phase 2: Upgrade{label} ===")

    pending = [d for d in data["devices"] if d["status"] == "PENDING"]
    if not pending:
        print("No PENDING devices to upgrade.")
        return

    if dry_run:
        print(f"Would send firmware URL to {len(pending)} device(s):")
        for dev in pending:
            topic = f"{TOPIC_ROOT}/{dev['device_id']}/firmware/set"
            print(f"  [{dev['device_id']}] {dev.get('name', '')} → {topic}")
            print(f"    payload: {args.firmware_url}")
        print("\n(Dry run — no messages published. Use --no-dry-run to execute.)")
        return

    print(f"Sending firmware URL to {len(pending)} device(s)...")
    for dev in pending:
        topic = f"{TOPIC_ROOT}/{dev['device_id']}/firmware/set"
        client.publish(topic, args.firmware_url, retain=False)
        dev["status"] = "SENT"
        dev["status_updated"] = utcnow_iso()
        dev["_sent_at"] = dev["status_updated"]  # internal tracking
        save_devices_file(data)
        print(f"  [{dev['device_id']}] {dev.get('name', '')} → firmware URL sent")
        time.sleep(2)

    print("All firmware URLs dispatched.")


# ---------------------------------------------------------------------------
# Phase 3 — Monitor
# ---------------------------------------------------------------------------

def run_monitor(args, data: dict, client: mqtt.Client) -> None:
    """
    Listen for status changes and update devices.json until all devices
    reach a terminal state or the per-device timeout fires.
    """
    print("\n=== Phase 3: Monitor ===")

    lock = threading.Lock()
    all_done = threading.Event()

    def _check_all_done():
        return all(d["status"] in TERMINAL_STATUSES for d in data["devices"])

    def on_message(client, userdata, msg):
        topic = msg.topic
        parts = topic.split("/")
        if len(parts) < 4:
            return
        device_id = parts[2]
        subtopic = parts[3]
        if subtopic != "status":
            return

        payload = msg.payload.decode("utf-8", errors="replace").strip()

        with lock:
            dev = find_device(data, device_id)
            if dev is None:
                return
            if dev["status"] in TERMINAL_STATUSES:
                return  # already done

            prev_status = dev["status"]
            new_status = None

            if payload == "UPGRADING":
                new_status = "UPGRADING"
            elif payload == "OFFLINE" and prev_status == "UPGRADING":
                new_status = "COMPLETE"
            elif payload == "UPDATE_FAILED":
                new_status = "FAILED"

            if new_status and new_status != prev_status:
                dev["status"] = new_status
                dev["status_updated"] = utcnow_iso()
                # Remove internal tracking key if present
                dev.pop("_sent_at", None)
                save_devices_file(data)
                print(f"  [{device_id}] {dev.get('name', '')} status → {new_status}")
                if _check_all_done():
                    all_done.set()

    client.on_message = on_message

    print("Monitoring for status changes (Ctrl-C to abort)...")

    while not all_done.is_set():
        time.sleep(5)
        now_iso = utcnow_iso()
        now_dt = datetime.now(timezone.utc)

        with lock:
            for dev in data["devices"]:
                if dev["status"] in TERMINAL_STATUSES:
                    continue
                if dev["status"] != "SENT":
                    continue
                # Check no-response timeout
                sent_at_str = dev.get("_sent_at") or dev.get("status_updated")
                if sent_at_str:
                    try:
                        sent_dt = datetime.strptime(sent_at_str, "%Y-%m-%dT%H:%M:%SZ").replace(
                            tzinfo=timezone.utc
                        )
                        elapsed = (now_dt - sent_dt).total_seconds()
                        if elapsed >= NO_RESPONSE_TIMEOUT_SECS:
                            dev["status"] = "NO_RESPONSE"
                            dev["status_updated"] = now_iso
                            dev.pop("_sent_at", None)
                            save_devices_file(data)
                            print(
                                f"  [{dev['device_id']}] {dev.get('name', '')} "
                                f"status → NO_RESPONSE (no reply in {NO_RESPONSE_TIMEOUT_SECS}s)"
                            )
                    except ValueError:
                        pass

            if _check_all_done():
                all_done.set()

    print("All devices have reached a terminal state.")


# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

def print_summary(data: dict) -> None:
    """Print a final summary table."""
    devices = data["devices"]
    total = len(devices)
    sent = sum(1 for d in devices if d["status"] in {"SENT", "UPGRADING"})
    complete = sum(1 for d in devices if d["status"] == "COMPLETE")
    failed = sum(1 for d in devices if d["status"] == "FAILED")
    no_response = sum(1 for d in devices if d["status"] == "NO_RESPONSE")

    print("\n=== Final Summary ===")
    print(f"  Total       : {total}")
    print(f"  Sent        : {sent}")
    print(f"  Complete    : {complete}")
    print(f"  Failed      : {failed}")
    print(f"  No Response : {no_response}")
    print()
    print_table(devices)


# ---------------------------------------------------------------------------
# MQTT client factory
# ---------------------------------------------------------------------------

def _build_client(args) -> mqtt.Client:
    """Create and configure a paho MQTT client."""
    # paho v2 requires a callback_api_version argument; fall back gracefully for v1
    try:
        client = mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
            client_id="firefly-migrate-v114",
        )
    except AttributeError:
        client = mqtt.Client(client_id="firefly-migrate-v114")

    if args.username:
        client.username_pw_set(args.username, args.password)

    return client


# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------

def parse_args():
    parser = argparse.ArgumentParser(
        description="Migrate v1.14 FireFly Client devices to new firmware over MQTT.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--broker", required=True, help="MQTT broker hostname or IP")
    parser.add_argument("--port", type=int, default=1883, help="MQTT broker port")
    parser.add_argument("--username", default=None, help="MQTT username")
    parser.add_argument("--password", default=None, help="MQTT password")
    parser.add_argument("--firmware-url", required=True, help="URL of the firmware binary to push")
    parser.add_argument(
        "--discovery-timeout",
        type=int,
        default=30,
        help="Seconds to wait for retained device messages during discovery",
    )
    parser.add_argument(
        "--dry-run",
        default=True,
        action=argparse.BooleanOptionalAction,
        help="Preview what would be sent without publishing firmware URLs (default: on; use --no-dry-run to execute)",
    )
    return parser.parse_args()


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    args = parse_args()

    if args.dry_run:
        print("*** DRY RUN — no firmware URLs will be published. Pass --no-dry-run to execute. ***")

    # Phase 1 — Discovery (uses its own short-lived MQTT connection)
    data = run_discovery(args)

    pending_count = sum(1 for d in data["devices"] if d["status"] == "PENDING")
    if pending_count == 0:
        already_terminal = sum(1 for d in data["devices"] if d["status"] in TERMINAL_STATUSES)
        print(
            f"\nNo PENDING devices found "
            f"({already_terminal} already at terminal status). Nothing to do."
        )
        print_summary(data)
        return

    if args.dry_run:
        # Show what would be sent, then stop — no MQTT connection needed
        run_upgrade(args, data, client=None)
        print_summary(data)
        return

    # Build a long-lived client for phases 2 and 3
    client = _build_client(args)

    connected = threading.Event()

    def on_connect(c, userdata, flags, rc, properties=None):
        if rc != 0:
            print(f"Connection failed with code {rc}", file=sys.stderr)
            sys.exit(1)
        c.subscribe(SUBSCRIBE_WILDCARD)
        connected.set()

    client.on_connect = on_connect
    client.connect(args.broker, args.port, keepalive=60)
    client.loop_start()

    # Wait for connection before publishing
    connected.wait(timeout=10)
    if not connected.is_set():
        print("Could not connect to broker for upgrade/monitor phases.", file=sys.stderr)
        sys.exit(1)

    try:
        # Phase 2 — Upgrade
        run_upgrade(args, data, client)

        # Phase 3 — Monitor
        run_monitor(args, data, client)

    except KeyboardInterrupt:
        print("\nInterrupted by user.")
    finally:
        client.loop_stop()
        client.disconnect()

    # Strip internal tracking fields before final save
    for dev in data["devices"]:
        dev.pop("_sent_at", None)
    save_devices_file(data)

    print_summary(data)


if __name__ == "__main__":
    main()
