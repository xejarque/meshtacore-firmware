#!/usr/bin/env python3
"""Hybrid encryption helpers for the Meshtacore custom firmware builder.

Why: build requests carry PSKs/passwords and the resulting firmware embeds them,
but issues, Actions logs, artifacts and releases are public. The web builder
encrypts the request with the repo public key and the workflow encrypts the
resulting ZIP with a throw-away public key generated in the requester's browser.

Wire format (identical in both directions, see custom_builder_web/crypto.js):
    RSA-OAEP(SHA-256) wrapped AES key (256 bytes) | IV (12 bytes) | AES-256-GCM ciphertext+tag
"""

import argparse
import base64
import json
import os
import sys
import zlib

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

WRAPPED_KEY_LEN = 256  # RSA-2048
IV_LEN = 12
SECRET_KEY_HINTS = ("PSK", "PASSWORD", "KEY", "SECRET")


def _oaep() -> padding.OAEP:
    return padding.OAEP(
        mgf=padding.MGF1(algorithm=hashes.SHA256()),
        algorithm=hashes.SHA256(),
        label=None,
    )


def _b64decode_any(text: str) -> bytes:
    """Decode standard or URL-safe base64, with or without padding."""
    cleaned = "".join(text.split()).replace("-", "+").replace("_", "/")
    return base64.b64decode(cleaned + "=" * (-len(cleaned) % 4))


def encrypt_bytes(data: bytes, public_key_spki_b64: str) -> bytes:
    public_key = serialization.load_der_public_key(_b64decode_any(public_key_spki_b64))
    aes_key = AESGCM.generate_key(bit_length=256)
    iv = os.urandom(IV_LEN)
    ciphertext = AESGCM(aes_key).encrypt(iv, data, None)
    return public_key.encrypt(aes_key, _oaep()) + iv + ciphertext


def decrypt_bytes(blob: bytes, private_key_pem: str) -> bytes:
    private_key = serialization.load_pem_private_key(private_key_pem.encode(), None)
    wrapped, iv, ciphertext = (
        blob[:WRAPPED_KEY_LEN],
        blob[WRAPPED_KEY_LEN : WRAPPED_KEY_LEN + IV_LEN],
        blob[WRAPPED_KEY_LEN + IV_LEN :],
    )
    return AESGCM(private_key.decrypt(wrapped, _oaep())).decrypt(iv, ciphertext, None)


def decrypt_request(request_b64: str, private_key_pem: str) -> dict:
    """Decrypt a build request produced by the web builder (deflate-raw JSON)."""
    plain = decrypt_bytes(_b64decode_any(request_b64), private_key_pem)
    return json.loads(zlib.decompress(plain, -15).decode("utf-8"))


def mask_secrets(prefs: dict) -> None:
    """Ask the Actions runner to redact secret values from every later log line."""
    for key, value in prefs.items():
        if (
            isinstance(value, str)
            and value
            and any(h in key.upper() for h in SECRET_KEY_HINTS)
        ):
            if "\n" not in value and "\r" not in value:
                print(f"::add-mask::{value}")


def _cmd_decrypt_request(args: argparse.Namespace) -> None:
    request = decrypt_request(
        os.environ["BUILD_REQUEST_B64"], os.environ["BUILD_PRIVATE_KEY"]
    )
    prefs = request.get("config", {}).get("custom_userprefs") or {}
    mask_secrets(prefs)
    with open(args.prefs_out, "w", encoding="utf-8") as f:
        json.dump(prefs, f, indent=2)
    with open(args.pubkey_out, "w", encoding="utf-8") as f:
        f.write(request["user_public_key"])


def _cmd_encrypt_file(args: argparse.Namespace) -> None:
    with open(args.public_key_file, encoding="utf-8") as f:
        public_key = f.read().strip()
    with open(args.infile, "rb") as f:
        data = f.read()
    with open(args.outfile, "wb") as f:
        f.write(encrypt_bytes(data, public_key))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)

    dec = sub.add_parser(
        "decrypt-request", help="env: BUILD_REQUEST_B64, BUILD_PRIVATE_KEY"
    )
    dec.add_argument("--prefs-out", default="requested_userprefs.json")
    dec.add_argument("--pubkey-out", default="user_public_key.b64")
    dec.set_defaults(func=_cmd_decrypt_request)

    enc = sub.add_parser("encrypt-file")
    enc.add_argument("--public-key-file", default="user_public_key.b64")
    enc.add_argument("--in", dest="infile", required=True)
    enc.add_argument("--out", dest="outfile", required=True)
    enc.set_defaults(func=_cmd_encrypt_file)

    args = parser.parse_args()
    try:
        args.func(args)
    except Exception as exc:  # never echo payloads; only the error class
        print(
            f"::error::build_crypto {args.cmd} failed: {type(exc).__name__}",
            file=sys.stderr,
        )
        sys.exit(1)


if __name__ == "__main__":
    main()
