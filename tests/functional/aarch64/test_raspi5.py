#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

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


if __name__ == '__main__':
    LinuxKernelTest.main()
