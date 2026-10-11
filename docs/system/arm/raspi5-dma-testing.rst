.. SPDX-License-Identifier: GPL-2.0-or-later

BCM2712 DMA verification
=======================

The downstream BCM2712 machines provide native SD DMA and a direct Linux
boot environment for DMA API and dma-buf test programs. They do not model
CPU data caches. Keep transfer-integrity results separate from physical
cache-coherency evidence.

Native SD DMA
-------------

Run the board tests from the build directory::

  meson test --print-errorlogs qtest-aarch64/bcm2712-test

``/aarch64/bcm2712/raspi500/sd-adma64`` checks a 64-bit descriptor table
and scattered buffers above 4 GiB. The Pi 500+ case,
``/aarch64/bcm2712/raspi500p/sd-adma64-sg``, adds eight read/write/read
rounds of eight sectors each. It uses linked tables above 8 GiB, with
descriptors and buffers straddling pages and one buffer spanning 8 GiB.
It checks payloads, buffer guards, poisoned low-address mirrors,
descriptor preservation, repeated completion IRQs and persistent card
contents. A NOP descriptor also exercises deferred ADMA continuation.

To capture full descriptor and linked-table addresses from the source root::

  QTEST_QEMU_BINARY="$PWD/build/qemu-system-aarch64" \
  QTEST_QEMU_ARGS="-trace enable=sdhci_adma*,file=$PWD/build/sd-adma.trace" \
    build/tests/qtest/bcm2712-test \
      -p /aarch64/bcm2712/raspi500p/sd-adma64-sg

The GIC and PMU suites and the Linux boot/card-persistence tests are useful
companion checks; see :doc:`raspi5` for their commands. All card images are
disposable files, not physical disks. SD DMA is a host-controller transfer
path; it does not expose a DMAengine memcpy channel for Linux ``dmatest``.

An external vb2 test bundle
--------------------------

``scripts/raspi5-vb2-smoke.py`` accepts a prepared external board-test
bundle and a static AArch64 BusyBox. It verifies the five required bundle
files against ``SHA256SUMS`` and creates a new results directory. It does
not download or modify a kernel, module tree or physical board. Python
3.12 or newer and ``cpio`` are required.

The current bundle contract is:

* ``base/Image.gz`` and ``base/modules-full.tar.gz``: a matching kernel and
  unpatched modules, with GICv2, PL011, SDHCI/BRCMSTB and initramfs support;
* ``patched/modules-patched.tar.gz``: the alternative media module tree
  for the same kernel release;
* ``vb2-ramcheck.arm64``: the static capture/alias test;
* ``pi-ramcheck.sh``: the two-tree capture runner;
* ``SHA256SUMS``: checksums for those files.

The supplied runner must implement the existing ``VB2RAM_DRM`` override
and ``TREE=... CELL=... rc=...`` output contract. The currently validated
bundle uses release ``7.3.0-rc5-brcmf-vb2ram+``. Its kernel has CMA and
dma-buf heaps, vivid/vgem modules, and ``CONFIG_STRICT_DEVMEM=n``.
It does not enable ``CONFIG_DMA_API_DEBUG`` or function tracing, so this
run makes no DMA-debug or begin/end-call-count claim.

Example from the source root, with paths to locally supplied inputs::

  python3 scripts/raspi5-vb2-smoke.py \
    --qemu build/qemu-system-aarch64 \
    --bundle /path/to/hwtest/artifacts \
    --busybox /path/to/static-arm64-busybox \
    --output /path/to/new-results \
    --machine raspi500p --frames 6 --churn-mib 64

The guest first performs four 128 KiB direct-I/O read/write/read rounds
through Linux's ``sdhci-brcmstb`` driver. The host checks that ADMA transfer
traces exist, reports observed high DMA addresses, and verifies the source,
every written destination and adjacent sectors in the persistent backend.

It then runs vivid capture with the baseline and patched module trees,
each with a CMA/vgem importer and a no-import control. The original tool
and board runner are copied unchanged. A recorded wrapper supplies the
same churn size to every tool invocation. The raw runner's legacy cell
name ``vivid-vc4`` is retained, but ``QEMU-IMPORTER`` and ``summary.json``
identify vgem: the machine has no vc4 model.

The verifier requires all four cells, the expected frame and byte counts,
the baseline/patched helper-symbol identity, 16 zero-residual alias checks,
uniform churn and zero stale/late/residual bytes. It rejects guest failure
markers, missing evidence and kernel faults. The supplied image's known
boot-time ``trace_events.c:420 test_double_dereference`` warning is recorded;
other WARNING lines are rejected. BusyBox's small module loader can emit
transient ``Unknown symbol`` messages while resolving dependencies. Their
count is reported, rather than claiming a clean kernel log.

The results directory contains the command, input hashes, generated init
and wrapper, initramfs, serial log, QEMU stderr, ADMA trace, sparse SD image
and machine-readable summary. A failed host verification leaves these
files available. Existing results directories are rejected.

What this can establish
-----------------------

This tests the modeled SD DMA path, high-address handling, Linux boot,
module switching and capture/alias-test execution. It can expose bad
address translation, transfer lengths, scatter-gather handling and runner
errors within the modeled subset.

It cannot reproduce dirty CPU lines surviving a memory-pressure sweep.
``target/arm/helper.c`` explicitly registers ``DC_IVAC``, ``DC_CVAC`` and
``DC_CIVAC`` as ``ARM_CP_NOP`` because QEMU does not emulate data caches.
The CPU and non-cacheable aliases therefore share the same backing RAM.
Reporting the correct L1/L2/L3 sizes in the device tree does not add cache
replacement, writeback or a separate DMA visibility domain. A zero result
with both module trees is expected and is not evidence against the
physical Pi 400 comparison or an explanation of the Pi 500+ failure.

RP1, USB/webcam, vc4 and physical NVMe HMB behavior are absent.  PCIe1 can
now host QEMU's NVMe endpoint and exercises programmed inbound/outbound DMA
translations plus MIP1 MSI-X delivery.  PCIe2/RP1 work remains in
:doc:`raspi5-rp1-plan`; an unrelated PCI endpoint or forced
synthetic cache fault would not reproduce the physical board's behavior.

Next physical Pi 500+ diagnostic
-------------------------------

The prior physical alias check left 64 bytes after 16 MiB churn and
320 bytes after 64 MiB churn. Neither attempt reached capture. The most
useful next board run is an alias-only calibration using the baseline
tree, with the existing NVMe 32 MiB HMB limit and temporary WirePlumber
isolation.

Record per-page physical addresses, mismatch offsets and byte samples,
allowed/requested/actual CPUs, and affinity-call results. Preserve the
pressure-only failure. Separately attach/map the buffer in vc4, write a
fresh pattern between matching ``DMA_BUF_IOCTL_SYNC`` START/END WRITE
markers, and compare the O_SYNC view immediately after END. Keep the
importer alive long enough to record its attachment. A pre-import SYNC
alone is insufficient for the CMA exporter, which synchronizes mapped
attachments.

Agreement only after explicit synchronization would support an eviction
or setup-protocol explanation. Continued differences would require
checking page identity, mapping attributes, the sync path and the memory
observer before another capture comparison. Setup synchronization must
not be inserted between DQBUF and the first frame snapshot: it could hide
the omission under test. Any temporary setup importer must be detached
before a no-import control streams. Normalize the actual negotiated
camera format before a future stock/patched webcam comparison.

This is a proposed hardware experiment, not a prepared calibration
binary or a hardware result. The Pi 400 comparison already provides
physical evidence; Pi 500+ broadens coverage after its measurement method
has been calibrated.

References
----------

* https://docs.kernel.org/driver-api/dma-buf.html#dma-buffer-ioctls
* https://docs.kernel.org/driver-api/dmaengine/dmatest.html

The emulator additions and test harness are AI-assisted downstream work,
subject to the development-provenance limitation in :doc:`raspi5`.
