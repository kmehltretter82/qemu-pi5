Raspberry Pi 5, Pi 500 and Pi 500+ (experimental)
===============================================

This downstream fork provides ``raspi5b`` (alias ``raspi5``) and
``raspi500`` and ``raspi500p`` machines. They implement an initial BCM2712
core platform for direct Linux kernel boot. They do not yet boot a
Raspberry Pi OS SD image or the Raspberry Pi EEPROM/VideoCore firmware.

Implemented
-----------

* Four Cortex-A76 CPUs, with MPIDR affinities 0x000, 0x100, 0x200, 0x300.
* RAM at physical address zero. Pi 5 supports 1, 2, 4, 8 or 16 GiB
  (default 2 GiB); Pi 500 uses 8 GiB and Pi 500+ uses 16 GiB.
* GICv2 distributor, CPU and virtualization interfaces at the BCM2712
  physical addresses, including per-core GICH aliases and five-bit priorities.
* Device-tree cache topology: private 64 KiB instruction/data L1s, private
  512 KiB L2s, and a shared 2 MiB L3. Cache timing is not modeled.
* CPU architectural timers at 54 MHz.
* Per-core PMU interrupts on GIC SPI 16 through 19, with device-tree CPU
  affinity. The PMU node is omitted with ``-cpu cortex-a76,pmu=off``.
* Both BCM2712 MIP MSI-X receivers, including vector offsets, host/VPU
  configuration and masking, status, software raise/clear and GIC routing.
* The external PCIe1 host, including root/downstream configuration, INTx,
  programmable outbound and inbound translations, and MSI-X delivery through
  MIP1.  QEMU PCIe endpoints can be attached to the ``pcie1`` bus.
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

For the Pi 500, use ``-M raspi500 -m 8G``; for the Pi 500+, use
``-M raspi500p -m 16G``. All three machines require four CPUs;
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

To attach QEMU's NVMe endpoint to PCIe1, add for example::

  -drive if=none,id=nvme0,file=nvme.raw,format=raw \
  -device nvme,serial=pi500,drive=nvme0,bus=pcie1,msix_qsize=8

Linux's ``brcm-pcie`` driver programs the BCM2712 translation windows.  The
endpoint's DMA and MSI-X writes then pass through the modeled inbound address
space; accesses outside enabled windows do not fall through to system RAM.

Validation
----------

From the build directory, run the board wiring tests and the Linux boot
tests (the latter download checksum-pinned kernel and initramfs assets)::

  meson test --print-errorlogs \
    qtest-aarch64/bcm2712-test qtest-aarch64/bcm2712-gic-test \
    qtest-aarch64/bcm2712-pmu-test
  meson test --setup thorough --print-errorlogs func-aarch64-raspi5

The board tests exercise UART10, both MIP blocks and SDIO1 interrupt routing,
MIP boundary vectors, masking, status and reset, SD register
access/reset, binary sector reads/writes and persistent backend contents,
independent RAM locations below and above 4 GiB, and PCIe1 root/downstream
configuration, link/reset state, BAR mapping and an attached NVMe endpoint.
The Pi 500+ ADMA2-64 test performs
24 multiblock transfers through linked descriptor tables above 8 GiB.
It checks scattered buffers across page and 8 GiB boundaries, guard bytes,
poisoned 32-bit address mirrors, transfer-complete IRQs and persistent
card contents. These are functional DMA checks; no CPU data cache is modeled.
Separate GIC tests check per-core virtualization state, virtual interrupts,
maintenance PPI25,
priority precision and immediate output deassertion during reset. The
functional tests boot all three boards with four CPUs, check timer frequency,
cache topology and PMU discovery, execute serial-console commands and
power off through PSCI. SD tests bind Linux's ``sdhci-brcmstb``
driver, verify sector hashes and writes, then read the persisted bytes in
a fresh VM using the same card backend.

GIC and timer-free register tests keep the guest CPUs paused while driving
MMIO and IRQs through qtest. Card and DMA tests keep the virtual clock running
and verify deferred card power-up. Linux functional tests exercise running
CPUs.

PMU tests use TCG to execute an embedded AArch64 guest. They trigger actual
counter overflows and check banked GIC pending/acknowledgement state and
per-core IRQ output levels, including reset with an asserted interrupt.
The guest polls with exceptions masked; interrupt-handler execution is
outside this test. No cross compiler or additional guest assets are needed
at test runtime.

The SD tests use a checksum-pinned Raspberry Pi 4 KiB-page kernel with
built-in MMC drivers. The small kernel used by the core tests lacks these
drivers; the Pi-specific 16 KiB-page kernel requires a compatible userland.

Limitations
-----------

This is a functional bring-up model, not a complete or timing-accurate Pi.
PCIe2/RP1, SDIO2/Wi-Fi, USB, Ethernet, GPIO, GPU/display,
mailbox firmware services, board identification registers and the power/reset
controllers are not implemented. The generated device tree omits these devices.
Unmodified hardware DTBs are not supported by this milestone; ``-dtb``
is available for experiments with descriptions matching the modeled subset.

RAM is currently contiguous, without firmware reservations or aliases.
The GIC is configured with 320 interrupt IDs; its capacity and identification
registers still need comparison against hardware. The UART reuses QEMU's
generic PL011, including its 16-byte FIFO and identification registers,
rather than the Pi's exact PL011 revision. UART and APB clocks share a fixed
9.216 MHz source in the generated DT. EL3 firmware is not modeled; Linux
starts at EL2.
Migration compatibility is not promised while this experimental machine
is under development.

The PMU uses QEMU's functional counter model. Under TCG, cycle counts
follow virtual time at a fixed 1 GHz rate; retired-instruction counting
requires precise ``-icount``. Cache events and physical Pi performance
timing are not modeled.

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

PCIe1 implements the register and translation subset used by Linux direct
boot and QEMU endpoints.  PHY/MDIO timing, advanced error reporting, hotplug,
power management and the controller's internal MSI output are not modeled.
PCIe2 and an RP1 endpoint remain future work.

The next PCIe/RP1 milestones and unresolved hardware details are recorded
in :doc:`raspi5-rp1-plan`.
For native SD DMA tests and an external vb2 bundle smoke runner, see
:doc:`raspi5-dma-testing`.

References
----------

The initial address map, interrupt numbers and CPU affinities follow
Linux's ``arch/arm64/boot/dts/broadcom/bcm2712.dtsi``. The 54 MHz counter
frequency was also checked against boot logs captured from a physical Pi 500.

* https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/broadcom/bcm2712.dtsi
* https://github.com/raspberrypi/linux/blob/4103a989a46196239b830bd07693277e42b8f992/arch/arm64/boot/dts/broadcom/bcm2712-ds.dtsi
* https://www.raspberrypi.com/documentation/computers/processors.html#bcm2712
* https://www.raspberrypi.com/products/raspberry-pi-500-plus/
* https://github.com/torvalds/linux/blob/master/drivers/mmc/host/sdhci-brcmstb.c
* https://documentation-service.arm.com/static/5e8f15e27100066a414f7424

Development provenance
----------------------

The initial BCM2712 machine, board wiring and associated tests/documentation
are AI-assisted downstream experimental work. They are not intended for
submission to upstream QEMU under its current code-provenance policy.
