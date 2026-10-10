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
  physical addresses.
* CPU architectural timers at 54 MHz.
* PL011 debug UART10 at 0x107d001000, connected to GIC SPI 121.
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

Validation
----------

From the build directory, run the board wiring tests and the Linux boot
tests (the latter download checksum-pinned kernel and initramfs assets)::

  meson test --print-errorlogs qtest-aarch64/bcm2712-test
  meson test --setup thorough --print-errorlogs func-aarch64-raspi5

The board tests exercise UART10-to-GIC interrupt routing, controller reset
and independent RAM locations below and above 4 GiB. The functional tests
boot both boards with four CPUs, check the timer frequency, execute commands
over the serial console and power off through PSCI.

Limitations
-----------

This is a functional bring-up model, not a complete or timing-accurate Pi.
RP1, PCIe, SD/SDIO, USB, Ethernet, Wi-Fi, GPIO, GPU/display, mailbox firmware
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

References
----------

The initial address map, interrupt numbers and CPU affinities follow
Linux's ``arch/arm64/boot/dts/broadcom/bcm2712.dtsi``. The 54 MHz counter
frequency was also checked against boot logs captured from a physical Pi 500.

* https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/broadcom/bcm2712.dtsi
* https://www.raspberrypi.com/documentation/computers/processors.html#bcm2712

Development provenance
----------------------

The initial BCM2712 machine, board wiring and associated tests/documentation
are AI-assisted downstream experimental work. They are not intended for
submission to upstream QEMU under its current code-provenance policy.
