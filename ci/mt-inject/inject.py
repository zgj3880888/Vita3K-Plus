#!/usr/bin/env python3
"""Inject MT Manager's document provider into a built APK.

MT Manager's "inject file provider" (注入文件提供器) does exactly two things to an
APK: it adds a ContentProvider and a trampoline activity to AndroidManifest.xml,
and it drops a small classes2.dex next to classes.dex that implements them. That
lets the app's /data/data/<pkg>/ show up in MT's file browser without root.

Doing it by hand means opening the APK in MT on the phone and letting it re-sign
with MT's own certificate, which breaks every later over-the-air update because
the signature no longer matches ours. This script reproduces the same two edits
on the build machine and re-signs with the project keystore, so the APK stays
installable over the previous one.

The manifest skeleton comes from ci/mt-inject/manifest.template, which is a
manifest MT itself produced. Two things in it are build specific and get fixed
up here:

  * versionName -- replaced with the one this APK actually declares. The string
    pool is rebuilt, so the new name does not have to be the same length.
  * com.google.android.gms.version -- an @integer reference that shifts whenever
    the resource table gains an entry. The real id is read back out of the APK
    being patched rather than hard coded.

Usage:
    inject.py --apk <app.apk> --ks <keystore.jks> [--build-tools DIR]
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
TEMPLATE_MANIFEST = os.path.join(HERE, 'manifest.template')
EXTRA_DEX = os.path.join(HERE, 'classes2.dex')
DEX_NAME = 'classes2.dex'

RES_STRING_POOL = 0x0001
RES_XML_START_ELEMENT = 0x0102
RES_XML_RESOURCE_MAP = 0x0180
RES_TABLE_PACKAGE = 0x0200
RES_TABLE_TYPE = 0x0201

VERSION_RE = re.compile(r'^\d+\.\d+[\w.\-]*-uc-[\w.\-]+$')


# --------------------------------------------------------------- AXML strings
class Pool:
    """Minimal ResStringPool reader."""

    def __init__(self, data, off):
        typ, hsz, size = struct.unpack_from('<HHI', data, off)
        assert typ == RES_STRING_POOL, 'not a string pool at %d' % off
        self.off = off
        self.hsz = hsz
        self.size = size
        count, style_count, flags, s_start, st_start = struct.unpack_from('<IIIII', data, off + 8)
        self.count = count
        self.style_count = style_count
        self.flags = flags
        self.s_start = s_start
        self.st_start = st_start
        self.utf8 = bool(flags & 0x100)
        self.offs = list(struct.unpack_from('<%dI' % count, data, off + hsz))
        self.strings = [self._read(data, off + s_start + o) for o in self.offs]

    def _read(self, data, p):
        if self.utf8:
            q = p
            b = data[q]
            q += 1
            if b & 0x80:
                q += 1
            b = data[q]
            q += 1
            if b & 0x80:
                n = ((b & 0x7f) << 8) | data[q]
                q += 1
            else:
                n = b
            return data[q:q + n].decode('utf-8', 'replace')
        n, = struct.unpack_from('<H', data, p)
        q = p + 2
        if n & 0x8000:
            n = ((n & 0x7fff) << 16) | struct.unpack_from('<H', data, q)[0]
            q += 2
        return data[q:q + n * 2].decode('utf-16-le', 'replace')

    def get(self, i):
        if i == 0xFFFFFFFF or i >= self.count:
            return None
        return self.strings[i]


def _encode(s, utf8):
    """Length prefix, content, then the terminator the pool insists on."""
    if utf8:
        raw = s.encode('utf-8')
        u16 = len(s.encode('utf-16-le')) // 2
        head = bytearray()
        for v in (u16, len(raw)):
            if v > 0x7f:
                head += bytes([0x80 | (v >> 8), v & 0xff])
            else:
                head.append(v)
        return bytes(head) + raw + b'\x00'
    u16 = len(s.encode('utf-16-le')) // 2
    if u16 > 0x7fff:
        return struct.pack('<HH', 0x8000 | (u16 >> 16), u16 & 0xffff) + s.encode('utf-16-le') + b'\x00\x00'
    return struct.pack('<H', u16) + s.encode('utf-16-le') + b'\x00\x00'


def find_pool(data):
    off = struct.unpack_from('<H', data, 2)[0]
    total = struct.unpack_from('<I', data, 4)[0]
    while off < total:
        typ, hsz, size = struct.unpack_from('<HHI', data, off)
        if typ == RES_STRING_POOL:
            return Pool(data, off)
        off += size
    raise SystemExit('no string pool in manifest')


def replace_pool_string(data, old, new):
    """Swap a pool string for another of any length, rebuilding the pool."""
    data = bytes(data)
    total = struct.unpack_from('<I', data, 4)[0]
    pool = find_pool(data)
    poff, cnt = pool.off, pool.count
    strings = list(pool.strings)

    hits = [i for i, s in enumerate(strings) if s == old]
    if not hits:
        raise SystemExit('pool has no %r' % old)
    for i in hits:
        strings[i] = new

    body = bytearray()
    new_offs = [0] * cnt
    for i in sorted(range(cnt), key=lambda k: pool.offs[k]):
        new_offs[i] = len(body)
        body += _encode(strings[i], pool.utf8)
    while len(body) % 4:
        body.append(0)

    styles_len = pool.size - pool.st_start if pool.st_start > pool.s_start else 0
    styles = data[poff + pool.st_start: poff + pool.size] if styles_len else b''
    head_len = pool.hsz + cnt * 4 + pool.style_count * 4
    s_start = head_len + (-head_len % 4)
    new_size = s_start + len(body) + styles_len

    out_pool = bytearray()
    out_pool += struct.pack('<HHI', RES_STRING_POOL, pool.hsz, new_size)
    out_pool += struct.pack('<IIIII', cnt, pool.style_count, pool.flags,
                            s_start, s_start + len(body) if styles_len else 0)
    out_pool += struct.pack('<%dI' % cnt, *new_offs)
    if pool.style_count:
        out_pool += data[poff + pool.hsz + cnt * 4: poff + head_len]
    out_pool += b'\x00' * (s_start - len(out_pool))
    out_pool += body
    out_pool += styles

    delta = new_size - pool.size
    out = bytearray(data[:poff]) + out_pool + data[poff + pool.size:]
    struct.pack_into('<I', out, 4, total + delta)
    return bytes(out)


def version_name(manifest):
    for s in find_pool(manifest).strings:
        if s and VERSION_RE.match(s):
            return s
    raise SystemExit('manifest declares no versionName')


def patch_gms_version(data, resid):
    """Point the gms.version meta-data at this APK's own integer resource."""
    out = bytearray(data)
    total = struct.unpack_from('<I', out, 4)[0]
    off = struct.unpack_from('<H', out, 2)[0]
    pool = None
    done = 0
    while off < total:
        ctyp, chsz, csize = struct.unpack_from('<HHI', out, off)
        if csize == 0:
            break
        if ctyp == RES_STRING_POOL:
            pool = Pool(bytes(out), off)
        elif ctyp == RES_XML_START_ELEMENT:
            if pool.get(struct.unpack_from('<I', out, off + 20)[0]) == 'meta-data':
                astart, asize, acount = struct.unpack_from('<HHH', out, off + 24)
                name = None
                value_at = None
                for k in range(acount):
                    a = off + 16 + astart + k * asize
                    attr = pool.get(struct.unpack_from('<I', out, a + 4)[0])
                    if attr == 'name':
                        name = pool.get(struct.unpack_from('<I', out, a + 16)[0])
                    elif attr == 'value':
                        value_at = a + 16
                if name == 'com.google.android.gms.version' and value_at is not None:
                    struct.pack_into('<I', out, value_at, resid)
                    done += 1
        off += csize
    if not done:
        raise SystemExit('manifest has no com.google.android.gms.version meta-data')
    return bytes(out)


# ---------------------------------------------------------------- resources
def read_arsc(apk):
    with zipfile.ZipFile(apk) as z:
        return z.read('resources.arsc')


def arsc_lookup_key(arsc, key_name):
    """Find the resource id whose entry key is key_name."""
    off = struct.unpack_from('<H', arsc, 2)[0]
    pkgs = []
    while off < len(arsc):
        ctyp, chsz, csize = struct.unpack_from('<HHI', arsc, off)
        if csize <= 0:
            break
        if ctyp == RES_TABLE_PACKAGE:
            pkgs.append(off)
        off += csize

    for poff in pkgs:
        pid, = struct.unpack_from('<I', arsc, poff + 8)
        ch = poff + struct.unpack_from('<H', arsc, poff + 2)[0]
        pend = poff + struct.unpack_from('<I', arsc, poff + 4)[0]
        type_pool = key_pool = None
        while ch < pend:
            ctyp, chsz, csize = struct.unpack_from('<HHI', arsc, ch)
            if csize <= 0:
                break
            if ctyp == RES_STRING_POOL:
                if type_pool is None:
                    type_pool = Pool(arsc, ch)
                elif key_pool is None:
                    key_pool = Pool(arsc, ch)
            elif ctyp == RES_TABLE_TYPE and key_pool is not None:
                tid = arsc[ch + 8]
                flags = arsc[ch + 9]
                entry_count, = struct.unpack_from('<I', arsc, ch + 12)
                entries_start, = struct.unpack_from('<I', arsc, ch + 16)
                cfg_size, = struct.unpack_from('<I', arsc, ch + 20)
                default_cfg = not any(arsc[ch + 24: ch + 20 + cfg_size])
                if default_cfg:
                    for eidx, eoff in _entry_offsets(arsc, ch, chsz, flags, entry_count):
                        p = ch + entries_start + eoff
                        esize, eflags = struct.unpack_from('<HH', arsc, p)
                        if eflags & 0x0001:
                            continue
                        key = key_pool.get(struct.unpack_from('<I', arsc, p + 4)[0])
                        if key == key_name:
                            return (pid << 24) | (tid << 16) | eidx
            ch += csize
    raise SystemExit('resources.arsc has no entry named %s' % key_name)


def _entry_offsets(arsc, ch, chsz, flags, count):
    if flags & 0x01:  # sparse
        for i in range(count):
            eidx, eoff = struct.unpack_from('<HH', arsc, ch + chsz + i * 4)
            yield eidx, eoff * 4
    elif flags & 0x02:  # offset16
        for i in range(count):
            eoff, = struct.unpack_from('<H', arsc, ch + chsz + i * 2)
            if eoff != 0xFFFF:
                yield i, eoff * 4
    else:
        for i in range(count):
            eoff, = struct.unpack_from('<I', arsc, ch + chsz + i * 4)
            if eoff != 0xFFFFFFFF:
                yield i, eoff


# ------------------------------------------------------------------- signing
def build_tools(explicit):
    if explicit:
        return explicit
    sdk = os.environ.get('ANDROID_SDK_ROOT') or os.environ.get('ANDROID_HOME')
    if not sdk:
        raise SystemExit('set ANDROID_SDK_ROOT or pass --build-tools')
    root = os.path.join(sdk, 'build-tools')

    def key(name):
        return [int(x) if x.isdigit() else 0 for x in re.split(r'[.\-]', name)]

    versions = sorted(os.listdir(root), key=key)
    return os.path.join(root, versions[-1])


def exe(path):
    return path + '.exe' if os.name == 'nt' and os.path.exists(path + '.exe') else path


def apksigner(bt):
    local = exe(os.path.join(bt, 'apksigner'))
    if os.path.exists(local):
        return [local]
    for suffix in ('.bat', '.sh'):
        if os.path.exists(local + suffix):
            return [local + suffix]
    return ['java', '-jar', os.path.join(bt, 'lib', 'apksigner.jar')]


def sign(path, bt, ks, alias, ks_pass, key_pass):
    aligned = path + '.aligned'
    if os.path.exists(aligned):
        os.remove(aligned)
    subprocess.run([exe(os.path.join(bt, 'zipalign')), '-p', '-f', '4', path, aligned],
                   check=True, stdout=subprocess.DEVNULL)

    cmd = apksigner(bt) + ['sign', '--ks', ks, '--ks-key-alias', alias,
                           '--ks-pass', 'pass:' + ks_pass, '--key-pass', 'pass:' + key_pass,
                           '--v1-signing-enabled', 'true', '--v2-signing-enabled', 'true',
                           '--v3-signing-enabled', 'true', '--out', path + '.signed', aligned]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    os.replace(path + '.signed', path)
    os.remove(aligned)


def verify(path, bt):
    r = subprocess.run(apksigner(bt) + ['verify', '--print-certs', path],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit('signature does not verify:\n' + r.stdout + r.stderr)
    for line in r.stdout.splitlines():
        if 'SHA-256 digest' in line:
            print('   ' + line.strip())


# ---------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--apk', required=True)
    ap.add_argument('--ks', required=True)
    ap.add_argument('--alias', default='vita3kuc')
    ap.add_argument('--ks-pass', default=os.environ.get('SIGNING_STORE_PASSWORD', 'vita3kuc123'))
    ap.add_argument('--key-pass', default=os.environ.get('SIGNING_KEY_PASSWORD', 'vita3kuc123'))
    ap.add_argument('--build-tools')
    ap.add_argument('--skip-sign', action='store_true')
    args = ap.parse_args()

    src = os.path.abspath(args.apk)
    with zipfile.ZipFile(src) as z:
        if DEX_NAME in z.namelist():
            print('%s already carries %s, nothing to do' % (src, DEX_NAME))
            return
        apk_manifest = z.read('AndroidManifest.xml')

    tpl_manifest = open(TEMPLATE_MANIFEST, 'rb').read()
    want_ver = version_name(apk_manifest)
    have_ver = version_name(tpl_manifest)
    print('versionName: %s -> %s' % (have_ver, want_ver))

    gms_id = arsc_lookup_key(read_arsc(src), 'google_play_services_version')
    print('gms.version resource: 0x%08x' % gms_id)

    manifest = replace_pool_string(tpl_manifest, have_ver, want_ver)
    manifest = patch_gms_version(manifest, gms_id)
    if version_name(manifest) != want_ver:
        raise SystemExit('manifest rewrite produced %r' % version_name(manifest))

    dex2 = open(EXTRA_DEX, 'rb').read()
    tmp = src + '.tmp'
    kept = 0
    with zipfile.ZipFile(src) as zi, zipfile.ZipFile(tmp, 'w') as zo:
        for info in zi.infolist():
            name = info.filename
            if name.startswith('META-INF/') and name.upper().endswith(
                    ('.RSA', '.DSA', '.EC', '.SF', 'MANIFEST.MF')):
                continue
            data = manifest if name == 'AndroidManifest.xml' else zi.read(name)
            ni = zipfile.ZipInfo(name, date_time=info.date_time)
            ni.compress_type = info.compress_type
            ni.external_attr = info.external_attr
            ni.create_system = info.create_system
            ni.internal_attr = info.internal_attr
            zo.writestr(ni, data)
            kept += 1
        ni = zipfile.ZipInfo(DEX_NAME, date_time=(1981, 1, 1, 1, 1, 0))
        ni.compress_type = zipfile.ZIP_DEFLATED
        ni.external_attr = 0o600 << 16
        ni.create_system = 0
        zo.writestr(ni, dex2, compresslevel=9)
    os.replace(tmp, src)
    print('rebuilt: %d entries + %s' % (kept, DEX_NAME))

    if args.skip_sign:
        print('skipping signing as asked')
        return

    bt = build_tools(args.build_tools)
    sign(src, bt, os.path.abspath(args.ks), args.alias, args.ks_pass, args.key_pass)
    verify(src, bt)
    print('done: %s (%d bytes)' % (src, os.path.getsize(src)))


if __name__ == '__main__':
    main()
