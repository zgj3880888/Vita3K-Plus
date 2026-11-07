#!/usr/bin/env python3
"""Print the signing certificate of an APK and compare it with an expected digest.

apksigner's --print-certs output is unreliable inside `set -o pipefail` pipelines
(it is a shell wrapper around java, and a downstream `head` closes the pipe
before java flushes), so the digest is read straight from the APK Signing Block.
"""
import hashlib
import struct
import sys

MAGIC = b"APK Sig Block 42"
V2 = 0x7109871A
V3 = 0xF05368C0
V31 = 0x1B93AD61


def length_prefixed(buf, off):
    n = struct.unpack("<I", buf[off:off + 4])[0]
    return buf[off + 4:off + 4 + n], off + 4 + n


def first_cert(value):
    signers, _ = length_prefixed(value, 0)
    signer, _ = length_prefixed(signers, 0)
    signed_data, _ = length_prefixed(signer, 0)
    _digests, pos = length_prefixed(signed_data, 0)
    certs, _ = length_prefixed(signed_data, pos)
    cert, _ = length_prefixed(certs, 0)
    return cert


def main():
    apk = sys.argv[1]
    expected = sys.argv[2].replace(":", "").lower() if len(sys.argv) > 2 else None
    data = open(apk, "rb").read()
    magic = data.rfind(MAGIC)
    if magic < 0:
        print("no APK Signing Block (v1-only signature)")
        return 1
    size = struct.unpack("<Q", data[magic - 8:magic])[0]
    start = magic + 8 - size
    pos, end = start + 8, magic - 8
    pairs = {}
    while pos < end:
        n = struct.unpack("<Q", data[pos:pos + 8])[0]
        scheme = struct.unpack("<I", data[pos + 8:pos + 12])[0]
        pairs[scheme] = data[pos + 12:pos + 8 + n]
        pos += 8 + n

    digest = None
    for scheme, name in ((V3, "v3"), (V31, "v3.1"), (V2, "v2")):
        if scheme in pairs:
            cert = first_cert(pairs[scheme])
            d = hashlib.sha256(cert).hexdigest()
            print("%-4s certificate sha256 = %s" % (name, d))
            if digest is None:
                digest = d

    if digest is None:
        print("no v2/v3 signer found")
        return 1
    if expected is None:
        return 0
    if digest != expected:
        print("::error::signing certificate changed (%s != %s), the APK would not "
              "install over the previous UC build" % (digest, expected))
        return 1
    print("OK: signature matches the pinned UC certificate")
    return 0


if __name__ == "__main__":
    sys.exit(main())
