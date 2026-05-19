#!/usr/bin/env python3
"""
FireFly ESP8266 CA Certificate Rotation Publisher

Fetches the TLS certificate chain from FIREFLY_CLOUD_API_ROOT, selects the
most-stable CA cert (self-signed root if present, otherwise last in chain),
computes its SHA-256 fingerprint, and publishes a retained JSON message to
the MQTT broker on FireFly/clients/cert/state so all provisioned ESP8266
clients receive the updated cert on their next connection.
"""

import json
import os
import re
import subprocess
import sys

from cryptography import x509
from cryptography.hazmat.primitives import hashes
import paho.mqtt.publish as mqtt_publish


def require_env(name):
    val = os.environ.get(name, '').strip()
    if not val:
        print(f'❌ {name} is required', file=sys.stderr)
        sys.exit(1)
    return val


def optional_env(name, default=''):
    return os.environ.get(name, default).strip()


def fetch_chain(domain):
    try:
        result = subprocess.run(
            ['openssl', 's_client', '-connect', f'{domain}:443',
             '-servername', domain, '-showcerts'],
            input=b'',
            capture_output=True,
            timeout=15,
        )
        return result.stdout
    except subprocess.TimeoutExpired:
        print(f'❌ Timed out connecting to {domain}:443', file=sys.stderr)
        sys.exit(1)


def select_cert(chain_data):
    pem_blocks = re.findall(
        b'-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----',
        chain_data,
        re.DOTALL,
    )
    if not pem_blocks:
        print('❌ No certificate blocks found in chain output', file=sys.stderr)
        sys.exit(1)

    # Prefer self-signed root CA; fall back to last cert in chain.
    # Note: cross-signed roots (e.g. Amazon Root CA 1 signed by Starfield)
    # are not self-signed, so the fallback handles them correctly.
    for pem in pem_blocks:
        cert = x509.load_pem_x509_certificate(pem)
        if cert.issuer == cert.subject:
            print(f'Using self-signed root CA: {cert.subject.rfc4514_string()}')
            return pem

    pem = pem_blocks[-1]
    cert = x509.load_pem_x509_certificate(pem)
    print(f'No self-signed root in chain; using last cert: {cert.subject.rfc4514_string()}')
    return pem


def compute_fingerprint(pem):
    cert = x509.load_pem_x509_certificate(pem)
    digest = cert.fingerprint(hashes.SHA256())
    return ':'.join(f'{b:02X}' for b in digest)


def main():
    cloud_url  = require_env('FIREFLY_CLOUD_API_ROOT')
    mqtt_host  = require_env('MQTT_HOST')
    mqtt_port  = int(optional_env('MQTT_PORT', '1883'))
    mqtt_user  = optional_env('MQTT_USERNAME')
    mqtt_pass  = optional_env('MQTT_PASSWORD')
    mqtt_topic = optional_env('MQTT_TOPIC', 'FireFly/clients/cert/state')

    domain = re.sub(r'https?://', '', cloud_url).split('/')[0]
    print(f'Fetching cert chain from {domain}...')

    chain_data = fetch_chain(domain)
    pem = select_cert(chain_data)

    fingerprint = compute_fingerprint(pem)
    pem_str = pem.decode()
    if not pem_str.endswith('\n'):
        pem_str += '\n'

    payload = json.dumps({'fingerprint': fingerprint, 'pem': pem_str})

    print(f'Fingerprint: {fingerprint}')
    print(f'Publishing retained message to {mqtt_host}:{mqtt_port} → {mqtt_topic}')

    auth = {'username': mqtt_user, 'password': mqtt_pass} if mqtt_user else None

    try:
        mqtt_publish.single(
            mqtt_topic,
            payload=payload,
            qos=1,
            retain=True,
            hostname=mqtt_host,
            port=mqtt_port,
            auth=auth,
        )
    except Exception as e:
        print(f'❌ Failed to publish to MQTT broker: {e}', file=sys.stderr)
        sys.exit(1)

    print('✅ Done')


if __name__ == '__main__':
    main()
