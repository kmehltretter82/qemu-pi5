#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

import hashlib

from qemu_test import LinuxKernelTest, Asset
from qemu_test import exec_command_and_wait_for_pattern


class Aarch64Raspi5Machine(LinuxKernelTest):
    """Exercise BCM2712 SMP, timers, debug UART and emulated PSCI."""

    ASSET_KERNEL = Asset(
        'https://storage.tuxboot.com/buildroot/20241119/arm64/Image',
        'b74743c5e89e1cea0f73368d24ae0ae85c5204ff84be3b5e9610417417d2f235')
    ASSET_INITRD = Asset(
        ('https://github.com/groeck/linux-build-test/raw/'
         '86b2be1384d41c8c388e63078a847f1e1c4cb1de/'
         'rootfs/arm64/rootfs.cpio.gz'),
        '7c0b16d1853772f6f4c3ca63e789b3b9ff4936efac9c8a01fb0c98c05c7a7648')

    def boot_board(self, machine, model):
        self.set_machine(machine)
        self.require_accelerator('tcg')
        self.vm.set_console()
        self.vm.add_args(
            '-accel', 'tcg',
            '-kernel', self.ASSET_KERNEL.fetch(),
            '-initrd', self.ASSET_INITRD.fetch(),
            '-append', self.KERNEL_COMMON_COMMAND_LINE +
            'console=ttyAMA0,115200 earlycon=pl011,mmio32,0x107d001000 '
            'panic=-1 rdinit=/bin/sh')
        self.vm.launch()
        self.wait_for_console_pattern('running at 54.00MHz')
        self.wait_for_console_pattern('smp: Brought up 1 node, 4 CPUs')
        self.wait_for_console_pattern('Run /bin/sh as init process')
        self.wait_for_console_pattern('~ #')

        exec_command_and_wait_for_pattern(
            self, 'mount -t sysfs sysfs /sys; '
            'cat /sys/devices/system/cpu/online', '0-3')
        exec_command_and_wait_for_pattern(
            self, 'cat /sys/firmware/devicetree/base/model; echo', model)
        # Linux must discover each private L2 and the shared external L3,
        # which is not reported in the Cortex-A76's architectural CLIDR.
        cache_properties = ('level', 'type', 'size', 'coherency_line_size',
                            'number_of_sets', 'ways_of_associativity',
                            'shared_cpu_list')
        cache_files = ' '.join(f'"$p/{prop}"' for prop in cache_properties)
        for cpu in range(4):
            command = (
                'check_cache() { '
                f'p=/sys/devices/system/cpu/cpu{cpu}/cache/index$1; shift; '
                f'test "$(cat {cache_files})" = '
                '"$(printf \'%s\\n\' "$@")"; }; '
                f'check_cache 0 1 Data 64K 64 256 4 {cpu} && '
                f'check_cache 1 1 Instruction 64K 64 256 4 {cpu} && '
                f'check_cache 2 2 Unified 512K 64 1024 8 {cpu} && '
                'check_cache 3 3 Unified 2048K 64 2048 16 0-3 && '
                "printf 'CACHE_%s\\n' OK || printf 'CACHE_%s\\n' FAIL")
            exec_command_and_wait_for_pattern(
                self, command, 'CACHE_OK', 'CACHE_FAIL')
        # The expected text is absent from the echoed command itself.
        exec_command_and_wait_for_pattern(
            self, "sleep 1; printf 'TIMER_%s\\n' OK", 'TIMER_OK')
        exec_command_and_wait_for_pattern(
            self, 'poweroff -f', 'reboot: Power down')
        self.vm.wait()

    def test_raspi5b(self):
        self.boot_board('raspi5b', 'Raspberry Pi 5 Model B')

    def test_raspi500(self):
        self.boot_board('raspi500', 'Raspberry Pi 500')


class Aarch64Raspi5SDMachine(LinuxKernelTest):
    """Exercise Linux's BCM2712 SDHCI driver and persistent card I/O."""

    # The 4 KiB-page kernel supports the existing BusyBox initramfs and has
    # MMC_BLOCK and SDHCI_BRCMSTB built in. The core boot test's small kernel
    # lacks MMC drivers. The Pi-specific 16 KiB kernel cannot run this rootfs.
    ASSET_KERNEL = Asset(
        ('https://raw.githubusercontent.com/raspberrypi/firmware/'
         'd91dd52c88a3918028a0ca6bbe6fe40e91bee951/boot/kernel8.img'),
        'bb2b97293c9257755fb9f15657ca1d514d7164b0f034aab63b0b806fbf6711ab')
    ASSET_INITRD = Aarch64Raspi5Machine.ASSET_INITRD

    def launch_sd_board(self, vm, kernel, card):
        vm.set_console()
        vm.add_args(
            '-accel', 'tcg', '-kernel', kernel,
            '-initrd', self.ASSET_INITRD.fetch(),
            '-drive', f'if=sd,file={card},format=raw',
            '-append', self.KERNEL_COMMON_COMMAND_LINE +
            'console=ttyAMA0,115200 earlycon=pl011,mmio32,0x107d001000 '
            'panic=0 panic_on_oops=1 rdinit=/bin/sh')
        vm.launch()
        self.wait_for_console_pattern('smp: Brought up 1 node, 4 CPUs', vm)
        self.wait_for_console_pattern('~ #', vm)
        exec_command_and_wait_for_pattern(
            self, 'mount -t devtmpfs devtmpfs /dev; '
            'for i in 1 2 3 4 5; do '
            'test -b /dev/mmcblk0 && break; sleep 1; done; '
            "test -b /dev/mmcblk0 && printf 'SD_%s\\n' READY || "
            "printf 'SD_%s\\n' FAIL", 'SD_READY', 'SD_FAIL', vm)
        exec_command_and_wait_for_pattern(
            self, 'mount -t sysfs sysfs /sys; '
            'readlink /sys/class/mmc_host/mmc0/device/driver',
            'sdhci-brcmstb', vm=vm)

    def check_sector(self, vm, sector, digest):
        exec_command_and_wait_for_pattern(
            self, f'dd if=/dev/mmcblk0 bs=512 skip={sector} count=1 '
            '2>/dev/null | sha256sum', digest, vm=vm)

    def poweroff(self, vm):
        exec_command_and_wait_for_pattern(
            self, 'poweroff -f', 'reboot: Power down', vm=vm)
        vm.wait()

    def check_sd_persistence(self, machine):
        self.set_machine(machine)
        self.require_accelerator('tcg')
        kernel = self.uncompress(self.ASSET_KERNEL, 'Image-sd', format='gz')
        card = self.scratch_file('sd.raw')
        source = bytes((i * 17 + 13) & 0xff for i in range(512))
        digest = hashlib.sha256(source).hexdigest()
        with open(card, 'wb') as image:
            image.truncate(4 * 1024 * 1024 * 1024)
            image.seek(3 * 512)
            image.write(source)

        self.launch_sd_board(self.vm, kernel, card)
        self.check_sector(self.vm, 3, digest)
        exec_command_and_wait_for_pattern(
            self, 'dd if=/dev/mmcblk0 of=/tmp/sector bs=512 skip=3 count=1 '
            '&& dd if=/tmp/sector of=/dev/mmcblk0 bs=512 seek=17 count=1 '
            "conv=notrunc && sync && printf 'SD_%s\\n' WRITTEN || "
            "printf 'SD_%s\\n' FAIL", 'SD_WRITTEN', 'SD_FAIL')
        self.check_sector(self.vm, 17, digest)
        self.poweroff(self.vm)

        # A guest readback alone could pass with a broken persistent backend.
        with open(card, 'rb') as image:
            image.seek(17 * 512)
            self.assertEqual(image.read(512), source)
            image.seek(3 * 512)
            self.assertEqual(image.read(512), source)
            image.seek(16 * 512)
            self.assertEqual(image.read(512), bytes(512))

        # A fresh VM reads the same bytes from the persistent card backend.
        rebooted = self.get_vm(name='sd-reboot')
        self.launch_sd_board(rebooted, kernel, card)
        self.check_sector(rebooted, 17, digest)
        self.poweroff(rebooted)

    def test_raspi5b_sd(self):
        self.check_sd_persistence('raspi5b')

    def test_raspi500_sd(self):
        self.check_sd_persistence('raspi500')


if __name__ == '__main__':
    LinuxKernelTest.main()
