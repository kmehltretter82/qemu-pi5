#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run an external vb2 board bundle in a disposable BCM2712 guest.

Python >= 3.12, cpio and a static AArch64 BusyBox are required. This checks
native SD DMA integrity and vb2/vgem plumbing, not physical cache coherency.
"""

import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tarfile
import tempfile


FILES = ('base/Image.gz', 'base/modules-full.tar.gz',
         'patched/modules-patched.tar.gz', 'vb2-ramcheck.arm64',
         'pi-ramcheck.sh')
SD_BYTES = 128 * 1024
SD_SOURCE = 16
SD_DEST = 1024
SD_ROUNDS = 4


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def verified_bundle(bundle):
    checksums = {}
    for line in (bundle / 'SHA256SUMS').read_text().splitlines():
        value, name = line.split(maxsplit=1)
        name = name.removeprefix('*')
        if name in checksums:
            raise ValueError(f'duplicate checksum: {name}')
        checksums[name] = value
    for name in FILES:
        if checksums.get(name) != digest(bundle / name):
            raise ValueError(f'missing or incorrect input checksum: {name}')
    return {name: checksums[name] for name in FILES}


def guest_init(frames, sd_hash):
    return f'''#!/bin/busybox sh
/bin/busybox --install -s /bin
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
fail() {{
    echo "QEMU-DMA-FAIL $*"
    echo 'QEMU-DMA-DONE rc=2'
    poweroff -f
    exit 2
}}
mount -t proc proc /proc || fail proc
mount -t sysfs sysfs /sys || fail sysfs
mount -t devtmpfs devtmpfs /dev || fail devtmpfs
mount -t debugfs none /sys/kernel/debug || fail debugfs
echo 'QEMU-DMA-SCOPE native-SD-DMA and CMA/vgem plumbing; caches not modeled'
uname -a
for i in 1 2 3 4 5 6 7 8 9 10; do
    test -b /dev/mmcblk0 && break
    sleep 1
done
test -b /dev/mmcblk0 || fail missing-SD
readlink /sys/class/mmc_host/mmc0/device/driver
dmesg | grep 'using ADMA' || fail missing-ADMA-driver
sd=/dev/mmcblk0
count={SD_BYTES // 512}
expected_hash='{sd_hash}'
for i in 0 1 2 3; do
    dest=$(({SD_DEST} + i * {SD_BYTES // 512}))
    dd if=$sd of=/tmp/source bs=512 skip={SD_SOURCE} count=$count \\
        iflag=direct || fail SD-read
    test "$(sha256sum /tmp/source | cut -d ' ' -f 1)" = "$expected_hash" ||
        fail SD-source-hash
    dd if=/tmp/source of=$sd bs=512 seek=$dest count=$count \\
        oflag=direct conv=notrunc || fail SD-write
    dd if=$sd of=/tmp/readback bs=512 skip=$dest count=$count \\
        iflag=direct || fail SD-readback
    cmp /tmp/source /tmp/readback || fail SD-compare
    echo "QEMU-SD-ROUND i=$i bytes={SD_BYTES} hash={sd_hash}"
done
modprobe vgem || fail vgem
card=
for d in /sys/class/drm/card*; do
    case "$(basename "$(readlink -f "$d/device")")" in
        vgem*) card=/dev/dri/$(basename "$d");;
    esac
done
test -c "$card" || fail missing-vgem-card
echo "QEMU-IMPORTER vgem $card (no vc4 hardware model)"
cd /test || fail test-directory
VB2RAM_DRM=$card sh ./pi-ramcheck.sh {frames}
rc=$?
test "$rc" = 0 || fail vb2-matrix-rc-$rc
echo 'QEMU-DMA-DONE rc=0'
poweroff -f
'''


def pack_root(root, output):
    paths = [Path('.')] + sorted(p.relative_to(root) for p in root.rglob('*'))
    names = b''.join(str(p).encode() + b'\0' for p in paths)
    packed = subprocess.run(
        ['cpio', '--null', '-o', '--format=newc', '--quiet'],
        cwd=root, input=names, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        check=True)
    with output.open('wb') as stream:
        with gzip.GzipFile(filename='', mode='wb', fileobj=stream,
                           compresslevel=1, mtime=0) as compressed:
            compressed.write(packed.stdout)


def validate_log(log, frames, churn, sd_hash):
    text = log.replace('\r', '')
    if len(re.findall(r'^QEMU-DMA-DONE rc=0$', text, re.M)) != 1:
        raise ValueError('missing unique guest success marker')
    if 'QEMU-DMA-FAIL' in text:
        raise ValueError('guest failure marker')
    rounds = re.findall(r'^QEMU-SD-ROUND i=(\d+) bytes=(\d+) hash=(\w+)$',
                        text, re.M)
    if rounds != [(str(i), str(SD_BYTES), sd_hash) for i in range(SD_ROUNDS)]:
        raise ValueError('incomplete SD DMA rounds')
    importer = re.findall(r'^QEMU-IMPORTER vgem (/dev/dri/card\d+) ',
                          text, re.M)
    if len(importer) != 1:
        raise ValueError('missing vgem importer identity')
    cells = re.findall(r'^TREE=(\w+) CELL=([\w-]+) rc=(\d+)$', text, re.M)
    expected = [(tree, cell, '0') for tree in ('base', 'patched')
                for cell in ('vivid-vc4', 'vivid-nodev')]
    if cells != expected:
        raise ValueError(f'incomplete or unexpected vb2 matrix: {cells}')
    helpers = re.findall(r'^vb2_vmalloc_begin_cpu_access in kallsyms: (\d+)$',
                         text, re.M)
    if helpers != ['0', '1']:
        raise ValueError('base/patched module identity check failed')
    results = []
    for line in re.findall(r'^RESULT (.+)$', text, re.M):
        row = {key: int(value) for key, value in
               re.findall(r'(\w+)=(\d+)', line)}
        if row.get('frames') != frames or row.get('bytes') != frames * 614400:
            raise ValueError(f'unexpected capture length: {row}')
        for key in ('stale_frames', 'cpu_vs_ram', 'late', 'residual',
                    'max_per_frame'):
            if row.get(key) != 0:
                raise ValueError(f'nonzero or missing TCG result: {row}')
        results.append(row)
    if len(results) != len(expected):
        raise ValueError('missing capture results')
    aliases = re.findall(r'^alias check buf (\d+): phys 0x[0-9a-f]+, '
                         r'(\d+) of (\d+) bytes not in RAM at once, '
                         r'(\d+) after churn$', text, re.M)
    if (aliases != [(str(i), '0', '614400', '0') for _ in expected
                    for i in range(4)]):
        raise ValueError('missing or failed strict alias checks')
    calls = re.findall(r'^QEMU-RAMCHECK-CHURN (\d+)$', text, re.M)
    if calls != [str(churn)] * len(expected):
        raise ValueError('inconsistent churn invocation')
    if re.search(r'Kernel panic|Oops:|BUG:|DMA-API:|unbalanced', text):
        raise ValueError('kernel fault or accounting report')
    warnings = re.findall(r'^.*WARNING:.*$', text, re.M)
    for warning in warnings:
        if not ('kernel/trace/trace_events.c:420' in warning and
                'test_double_dereference' in warning):
            raise ValueError(f'unexpected kernel warning: {warning}')
    return {'cells': cells, 'results': results, 'alias_checks': len(aliases),
            'frames': frames * len(expected), 'sd_rounds': len(rounds),
            'importer': importer[0], 'physical_cache_evidence': False,
            'known_boot_warnings': warnings,
            'module_loader_symbol_messages': text.count('Unknown symbol')}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, required=True)
    parser.add_argument('--bundle', type=Path, required=True)
    parser.add_argument('--busybox', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True,
                        help='new results directory; existing ones rejected')
    parser.add_argument('--machine',
                        choices=('raspi5b', 'raspi500', 'raspi500p'),
                        default='raspi500p')
    parser.add_argument('--frames', type=int, default=6)
    parser.add_argument('--churn-mib', type=int, default=64)
    parser.add_argument('--timeout', type=int, default=600)
    args = parser.parse_args()
    if min(args.frames, args.churn_mib, args.timeout) < 1:
        parser.error('frames, churn and timeout must be positive')
    qemu, bundle, busybox = (p.resolve(strict=True) for p in
                            (args.qemu, args.bundle, args.busybox))
    inputs = verified_bundle(bundle)
    with busybox.open('rb') as stream:
        header = stream.read(20)
    if (header[:6] != b'\x7fELF\x02\x01' or
            struct.unpack('<H', header[18:20])[0] != 183):
        parser.error('BusyBox must be a static AArch64 ELF binary')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    (output / 'inputs.json').write_text(json.dumps({
        'bundle': str(bundle), 'sha256': inputs,
        'qemu': str(qemu), 'qemu_sha256': digest(qemu),
        'busybox_sha256': digest(busybox), 'machine': args.machine,
        'frames_per_cell': args.frames, 'churn_mib': args.churn_mib,
    }, indent=2) + '\n')
    source = bytes((i * 43 + (i >> 8) * 17 + 19) & 255 for i in range(SD_BYTES))
    sd_hash = hashlib.sha256(source).hexdigest()
    card = output / 'sd.raw'
    with card.open('wb') as stream:
        stream.truncate(4 * 1024 ** 3)
        stream.seek(SD_SOURCE * 512)
        stream.write(source)
    initrd = output / 'initramfs.cpio.gz'
    with tempfile.TemporaryDirectory(prefix='root-', dir=output) as scratch:
        root = Path(scratch)
        for directory in ('bin', 'proc', 'sys', 'dev', 'tmp', 'test', 'lib'):
            (root / directory).mkdir()
        with tarfile.open(bundle / 'base/modules-full.tar.gz') as archive:
            archive.extractall(root, filter='data')
        shutil.copy2(busybox, root / 'bin/busybox')
        shutil.copy2(bundle / 'vb2-ramcheck.arm64',
                     root / 'test/vb2-ramcheck-real.arm64')
        shutil.copy2(bundle / 'pi-ramcheck.sh', root / 'test/pi-ramcheck.sh')
        shutil.copy2(bundle / 'patched/modules-patched.tar.gz',
                     root / 'test/modules-patched.tar.gz')
        wrapper = root / 'test/vb2-ramcheck.arm64'
        wrapper.write_text('#!/bin/sh\n'
                           f'echo "QEMU-RAMCHECK-CHURN {args.churn_mib}"\n'
                           'exec /test/vb2-ramcheck-real.arm64 '
                           f'-c {args.churn_mib} "$@"\n')
        wrapper.chmod(0o755)
        init = root / 'init'
        init.write_text(guest_init(args.frames, sd_hash))
        init.chmod(0o755)
        shutil.copy2(init, output / 'guest-init.sh')
        shutil.copy2(wrapper, output / 'ramcheck-wrapper.sh')
        pack_root(root, initrd)
    command = [str(qemu), '-M', args.machine, '-accel', 'tcg',
               '-display', 'none', '-serial', f'file:{output / "console.log"}',
               '-monitor', 'none', '-no-reboot',
               '-kernel', str(bundle / 'base/Image.gz'), '-initrd', str(initrd),
               '-drive', f'if=sd,file={card},format=raw',
               '-trace', f'enable=sdhci_adma*,file={output / "sd-adma.trace"}',
               '-append', 'console=ttyAMA0,115200 '
               'earlycon=pl011,mmio32,0x107d001000 rdinit=/init '
               'cma=64M panic=-1 loglevel=4']
    (output / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
    with (output / 'qemu.log').open('wb') as stream:
        completed = subprocess.run(command, stdin=subprocess.DEVNULL,
                                   stdout=stream, stderr=stream,
                                   timeout=args.timeout)
    if completed.returncode != 0:
        raise ValueError(f'QEMU exited {completed.returncode}; see {output}')
    summary = validate_log((output / 'console.log').read_text(errors='replace'),
                           args.frames, args.churn_mib, sd_hash)
    trace = (output / 'sd-adma.trace').read_text()
    transfers = re.findall(r'sdhci_adma_loop addr=0x([0-9a-f]+), '
                           r'len=(\d+), attr=0x([0-9a-f]+)', trace)
    addresses = [int(addr, 16) for addr, _, attr in transfers
                 if int(attr, 16) & 0x30 == 0x20]
    if not addresses:
        raise ValueError('no native SD ADMA transfer trace')
    with card.open('rb') as stream:
        for sector in [SD_SOURCE] + [SD_DEST + i * (SD_BYTES // 512)
                                   for i in range(SD_ROUNDS)]:
            stream.seek(sector * 512)
            if stream.read(SD_BYTES) != source:
                raise ValueError(f'SD contents differ at sector {sector}')
        for sector in (SD_DEST - 1, SD_DEST + SD_ROUNDS * (SD_BYTES // 512)):
            stream.seek(sector * 512)
            if stream.read(512) != bytes(512):
                raise ValueError(f'neighbor SD sector changed: {sector}')
    summary.update({'sd_transfer_descriptors': len(addresses),
                    'sd_dma_above_4g': sum(a > 0xffffffff for a in addresses),
                    'sd_max_dma_address': hex(max(addresses)),
                    'persistent_sd_verified': True,
                    'sd_source_sha256': sd_hash})
    (output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
