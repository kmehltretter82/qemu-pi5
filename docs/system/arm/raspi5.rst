Raspberry Pi 5 and Pi 500 (experimental)
=======================================

This downstream fork provides ``raspi5b`` (alias ``raspi5``) and
``raspi500`` machines. They implement an initial BCM2712 core platform for
direct Linux kernel boot. They do not yet boot a Raspberry Pi OS SD image
or the Raspberry Pi EEPROM/VideoCore firmware.

Implemented
-----------

* Four Cortex-A76 CPUs, with MPIDR affinities 0x000, 0x100, 0x200, 0x300.
* RAM at physical address zero. Pi 5 supports 1, 2, 4, 8 or 16 GiB
  (default 2 GiB); Pi 500 uses 8 GiB.
* GICv2 distributor, CPU and virtualization interfaces at the BCM2712
  physical addresses, including per-core GICH aliases and five-bit priorities.
* Device-tree cache topology: private 64 KiB instruction/data L1s, private
  512 KiB L2s, and a shared 2 MiB L3. Cache timing is not modeled.
* CPU architectural timers at 54 MHz.
* PL011 debug UART10 at 0x107d001000, connected to GIC SPI 121.
* SDIO1 microSD host at 0x1000fff000, configuration block at 0x1000fff400,
  and GIC SPI 273. The SDHCI v3 subset uses a 200 MHz base clock and limits
  card transfers to 50 MHz, with PIO, SDMA and ADMA2 support.
* Direct kernel and initramfs loading, with QEMU's emulated PSCI over SMC.
* A generated device tree describing the implemented subset of hardware.

Example
-------

Build a minimal TCG emulator from this checkout::

  mkdir build
  cd build
  ../configure --target-list=aarch64-softmmu --without-default-features \
    --enable-tcg --enable-fdt
  ninja qemu-system-aarch64

Use an AArch64 Linux kernel with the GICv2, architectural timer, PL011
console and initramfs drivers built in::

  qemu-system-aarch64 -M raspi5b -accel tcg -m 2G \
    -display none -serial stdio -monitor none \
    -kernel Image -initrd rootfs.cpio.gz \
    -append 'console=ttyAMA0,115200 earlycon=pl011,mmio32,0x107d001000 rdinit=/bin/sh'

For the Pi 500, use ``-M raspi500 -m 8G``. Both machines require four CPUs;
this is the default, so ``-smp`` is optional. The first serial backend is
the SoC debug UART, not an RP1 UART on the GPIO header.

To attach an SD image, add ``-drive if=sd,file=sd.raw,format=raw``. The
slot is empty by default. A kernel with ``CONFIG_MMC``, ``CONFIG_MMC_BLOCK``,
``CONFIG_MMC_SDHCI``, ``CONFIG_MMC_SDHCI_PLTFM`` and
``CONFIG_MMC_SDHCI_BRCMSTB`` built in can access it as ``/dev/mmcblk0``.
For a root filesystem on the card, load the kernel directly and pass the
appropriate ``root=/dev/mmcblk0pN rootwait`` command line. EEPROM/VideoCore
firmware loading from the card remains unimplemented. QEMU's generic SD-card
device requires a writable backend and rejects ``readonly=on``. Use
``snapshot=on`` for disposable writes that leave the original image intact.

Validation
----------

From the build directory, run the board wiring tests and the Linux boot
tests (the latter download checksum-pinned kernel and initramfs assets)::

  meson test --print-errorlogs qtest-aarch64/bcm2712-test qtest-aarch64/bcm2712-gic-test
  meson test --setup thorough --print-errorlogs func-aarch64-raspi5

The board tests exercise UART10 and SDIO1 interrupt routing, SD register
access/reset, binary sector reads/writes and persistent backend contents,
and independent RAM locations below and above 4 GiB. Separate GIC tests
check per-core virtualization state, virtual interrupts, maintenance PPI25,
priority precision and reset. The functional tests boot both boards with
four CPUs, check timer frequency and cache topology, execute serial-console
commands and power off through PSCI. SD tests bind Linux's ``sdhci-brcmstb``
driver, verify sector hashes and writes, then read the persisted bytes in
a fresh VM using the same card backend.

GIC and timer-free register tests keep the guest CPUs paused while driving
MMIO and IRQs through qtest. Card and DMA tests keep the virtual clock running
and verify deferred card power-up. Linux functional tests exercise running
CPUs.

The SD tests use a checksum-pinned Raspberry Pi 4 KiB-page kernel with
built-in MMC drivers. The small kernel used by the core tests lacks these
drivers; the Pi-specific 16 KiB-page kernel requires a compatible userland.

Limitations
-----------

This is a functional bring-up model, not a complete or timing-accurate Pi.
RP1, PCIe, SDIO2/Wi-Fi, USB, Ethernet, GPIO, GPU/display, mailbox firmware
services, board identification registers and the power/reset controllers
are not implemented. The generated device tree omits these devices.
Unmodified hardware DTBs are not supported by this milestone; ``-dtb``
is available for experiments with descriptions matching the modeled subset.

RAM is currently contiguous, without firmware reservations or aliases.
The GIC is configured with 320 interrupt IDs; its capacity and identification
registers still need comparison against hardware. The UART reuses QEMU's
generic PL011, including its 16-byte FIFO and identification registers,
rather than the Pi's exact PL011 revision. UART and APB clocks share a fixed
9.216 MHz source in the generated DT. CPU PMU interrupts are not wired and
no PMU node is advertised. EL3 firmware is not modeled; Linux starts at EL2.
Migration compatibility is not promised while this experimental machine
is under development.

The SD model supports static media. Configuration-driven card detection
changes Present State but does not synthesize insertion/removal interrupts.
Pin selection, PHY mode and vendor control registers preserve the driver's
writes without modeling their electrical or clock-gating effects. UHS speed
modes, CQE, tuning fidelity and error recovery are not validated by this
milestone. The advertised SD capabilities describe the implemented subset,
not an exact hardware register capture. Physical write-protect switches and
read-only SD backends are outside this milestone.
The reused SDHCI migration state does not preserve Host Control 2, so SD
migration and VM snapshot restoration are not supported by this milestone.

The next PCIe/RP1 milestones and unresolved hardware details are recorded
in :doc:`raspi5-rp1-plan`.

References
----------

The initial address map, interrupt numbers and CPU affinities follow
Linux's ``arch/arm64/boot/dts/broadcom/bcm2712.dtsi``. The 54 MHz counter
frequency was also checked against boot logs captured from a physical Pi 500.

* https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/broadcom/bcm2712.dtsi
* https://www.raspberrypi.com/documentation/computers/processors.html#bcm2712
* https://github.com/torvalds/linux/blob/master/drivers/mmc/host/sdhci-brcmstb.c
* https://documentation-service.arm.com/static/5e8f15e27100066a414f7424

Development provenance
----------------------

The initial BCM2712 machine, board wiring and associated tests/documentation
are AI-assisted downstream experimental work. They are not intended for
submission to upstream QEMU under its current code-provenance policy.
