BCM2712 PCIe and RP1 implementation plan
========================================

This is a research and implementation note for the experimental downstream
``raspi5b``, ``raspi500`` and ``raspi500p`` machines.  PCIe1 and both MIP
receivers are implemented; PCIe2 and RP1 remain planned.  Sources and local
hardware observations were reviewed on 2026-10-10; pin the Linux source
revision used by each future boot test.
The native host and MIP source audit uses Linux revision
``3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0``.

Recommended next checkpoint
---------------------------

The PCIe1 checkpoint is complete.  Linux enumerates QEMU's NVMe endpoint,
performs direct I/O through the programmed 64 GiB inbound RAM window, and
delivers MSI-X writes through the MIP1 window.  Traces have also confirmed
DMA into guest physical RAM above 4 GiB.  The next implementation checkpoint
is PCIe2 followed by a verified RP1 endpoint.

RP1 follows when its PCI configuration and interrupt controls can be
implemented from verified evidence. Its first useful checkpoint is Linux
binding ``rp1_pci`` and servicing one real peripheral through BAR1 and
MSI-X. UART is a candidate because it exercises MMIO and level interrupts
without requiring peripheral DMA. An endpoint that merely supplies the
RP1 PCI ID and empty BARs would not meet that checkpoint.

Established address and interrupt map
-------------------------------------

Linux's `BCM2712 device tree`_ describes these native host interfaces.
Numbers in the interrupt columns are GIC SPI numbers; GIC interrupt IDs
are 32 greater.

.. list-table:: BCM2712 host controllers
   :header-rows: 1
   :widths: 14 24 18 22 22

   * - Port
     - Registers (size 0x9310)
     - Link
     - INTx A/B/C/D
     - Controller / MSI
   * - PCIe1
     - 0x1000110000
     - Gen2 x1
     - 219 / 220 / 221 / 222
     - 223 / 224
   * - PCIe2, RP1
     - 0x1000120000
     - Gen2 x4
     - 229 / 230 / 231 / 232
     - 233 / 234

PCIe2's CPU outbound windows start at 0x1f00000000 (PCI address zero,
almost 4 GiB) and 0x1c00000000 (PCI address 0x400000000, 12 GiB).
Its inbound DT ranges translate PCI 0x1000000000 to CPU RAM at zero,
PCI 0xfffffff000 to MIP0 at CPU 0x1000130000, and PCI zero to the
CPU's 4 MiB RP1 aperture at 0x1f00000000. CPU and PCI addresses must
remain distinct throughout the implementation.

PCIe1 uses CPU outbound bases 0x1b00000000 and 0x1800000000 for the
same PCI target ranges. Its inbound view maps PCI 0x1000000000 to RAM
at zero and PCI 0xfffffff000 to MIP1 at CPU 0x1000131000. It has no
RP1 aperture in its DMA ranges.

The local Pi 500+ capture
``runs/capture-sdhci-tryboot-20261008T185744Z/pci.stdout`` shows a
``14e4:2712`` root port, revision 0x30, and an RP1 ``1de4:0001``
endpoint with class 0x020000. Linux assigned these non-prefetchable,
32-bit BARs; the CPU addresses are an allocation result, not fixed RP1
register addresses:

.. list-table:: Captured RP1 BAR allocation
   :header-rows: 1
   :widths: 10 20 28 42

   * - BAR
     - Size
     - CPU address
     - Role
   * - 0
     - 16 KiB
     - 0x1f00410000
     - Endpoint register details need verification
   * - 1
     - 4 MiB
     - 0x1f00000000
     - RP1 peripherals
   * - 2
     - 64 KiB
     - 0x1f00400000
     - Shared SRAM

Native host model
-----------------

The `Linux PCIe driver`_ supplies the controller programming interface.
Root bus function 0 accesses its configuration at controller offset zero.
Downstream configuration uses index 0x9000, containing bus and devfn in
ECAM format, followed by a 4 KiB data window at 0x8000. This is sufficient
to reuse QEMU PCI configuration dispatch without exposing a generic ECAM
controller to the guest.

Four outbound windows use PCI target registers at 0x400c/0x4010,
CPU base/limit at 0x4070, and high address fields at 0x4080/0x4084.
BCM2712 uses the BCM7712-style layout: up to ten inbound windows,
inbound address/size registers starting at 0x402c and 0x40d4, and
CPU remap registers at 0x40ac and 0x410c. Size encoding is non-linear.
Size code zero disables an inbound window; codes 1 through 21 encode
``2^(code + 15)`` bytes, and codes 28 through 31 encode 4 through 32 KiB.
CPU remap bit zero enables translation. Outbound base/limit fields use
MiB units with inclusive limits.
MDIO is at 0x1100/0x1104/0x1108; reset is at 0x9210, PERSTB at
0x4064, status at 0x4068, and hard-debug at 0x4304. Status bit 7
selects root-complex mode; bits 4 and 5 report link readiness.
Mainline's BCM2712 post-setup path programs the 54 MHz reference clock
through MDIO and polls completion. Raspberry Pi vendor driver revisions
also touch additional UBUS/QoS controls; select and pin the first test
kernel before defining the initial register subset.

Use QEMU's PCI host, bridge and PCIe capability infrastructure for
enumeration, BAR sizing, bus numbering and ordinary configuration state.
Implement the Broadcom register interface and translation controls as a
dedicated host. ``gpex`` and ``designware`` offer structural examples,
but their guest register interfaces describe different hardware.

Drive MemoryRegion aliases or an equivalent translated DMA address space
from the programmed window registers. Keep decoding disabled when a
window is disabled, preserve transaction failures outside valid mappings,
and route endpoint DMA and MSI writes through the same inbound view.
QEMU's ``msi_send_message()`` already uses the endpoint's bus-master
address space; retain that path so disabling the MIP inbound window also
prevents MSI delivery. A separate MSI shortcut would bypass translation.
Reset, PERST, MDIO completion and link state need observable transitions;
a constant link-up value is insufficient for reset tests. The initial
model may omit analog timing while implementing those transitions.

MIP and RP1 interrupt requirements
----------------------------------

The `MIP driver`_ composes an MSI message with vector number as data.
MIP0's CPU register block is 0x1000130000, size 0xc0; its PCI message
address is 0xfffffff000, reached through an inbound host window. The
device tree allocates 64 vectors at SPI 128 through 191. The driver
initializes host edge configuration at offsets 0x20/0x30, host masks
at 0x40/0x50, and VPU masks at 0x60/0x70. Raise, clear and status
registers require explicit modeling and verification. MIP1 is a separate
instance for PCIe1 with eight allocatable vectors. Its DT specifies
``msi-ranges`` base SPI 247 and ``brcm,msi-offset = <8>``. The driver
adds that offset before composing message data and selecting the parent
SPI, so messages 8 through 15 route to SPI 255 through 262 (GIC IDs
287 through 294). These MIP interrupts are separate from the controller's
SPI 224 MSI output. Test the offset explicitly rather than treating the
DT range base as the first delivered SPI.

The `RP1 PCI driver`_ requires an OF node, a usable BAR1, and exactly
61 MSI-X vectors before it creates the RP1 interrupt domain and populates
child devices. It uses BAR1 + 0x108000 + 0x8 + 4*vector for interrupt
configuration: enable is bit 0, acknowledge bit 2, and acknowledge-enable
bit 3. Set/clear aliases are +0x800/+0xc00. Standard MSI-X table masks
and RP1's additional interrupt controls are separate state.

The `RP1 datasheet`_ explains why the acknowledge controls matter: a
level interrupt can stay asserted across handler activity. With
acknowledge enabled, delivery temporarily masks that source; acknowledge
rearms it and resends when it remains asserted. Implement this around
QEMU ``msix_notify()``, retaining its standard table and pending-bit
handling. Never wire RP1 peripheral IRQs directly to the GIC.

Local ``PHASE0-RESULTS.md`` records an earlier hardware boot failure when
``CONFIG_MISC_RP1=y`` preceded a modular ``CONFIG_BCM2712_MIP``.
The corrected boot used built-in MIP. Future functional kernels must
provide MIP before the RP1 probe; controller MSI or INTx alone will not
satisfy the RP1 driver's allocation request.

Reuse candidates and remaining evidence
---------------------------------------

The `RP1 peripheral device tree`_ places GEM Ethernet at BAR1 + 0x100000
with source 6, and DWC3 USB hosts at +0x200000/+0x300000 with edge
sources 31/36. Its RP1 clock controller and peripheral DMA view also
need modeling before those nodes can be enabled.

QEMU ``cadence_gem`` has 64-bit DMA and configurable identification,
``usb-dwc3`` composes an ``xhci-sysbus`` child, and ``pl011`` and
``designware_i2c`` provide candidates for low-speed peripherals. Compare
each candidate's registers, IRQ behavior and reset state with RP1 before
reuse. In particular, the current DWC3 model's global controls are partial
and contain fixed capability values. Pass RP1 children a translated DMA
MemoryRegion instead of allowing their default system-memory address
space to bypass PCIe windows.

Resolve these questions before the corresponding implementation:

* Capture the full RP1 PCI capability chain: MSI-X table size, table/PBA
  BAR indicators and offsets, PCIe capability placement, and reset state.
  Existing ``lspci`` output reports capabilities as access denied, so
  table placement is not established by the saved evidence.
* Confirm BAR0's accessible regions, BAR2 reset contents, and the minimal
  endpoint initialization state expected after firmware handoff.
* Verify MIP raise/clear encoding, edge/level status persistence and mask
  transitions using additional documentation or controlled hardware
  observations. Linux initialization alone does not specify all semantics.
* Verify controller revision/reset values and PHY MDIO readback. Distinguish
  the PCI revision byte from the controller's hardware revision register.
* Check RP1 clock/reset dependencies, peripheral identification values,
  and the direct-boot DT hierarchy with the selected guest kernel. Keep
  unimplemented child devices absent or disabled.

Validation sequence
-------------------

1. Test native root/downstream configuration, absent functions, BAR
   relocation, bridge bus numbers, reset/link transitions, both outbound
   apertures, and disabled-window behavior with qtest.
2. Test inbound RAM translation below and above 4 GiB, MIP vector routing,
   masking and repeated delivery. Exercise a real QEMU NVMe endpoint on
   PCIe1 under Linux, then verify data read/write and MSI-X delivery.
   Use ``nvme,msix_qsize=8`` for MIP1's eight-vector subset. Check table/PBA
   placement from PCI capabilities, pending replay after endpoint masking,
   and advancing MIP interrupt counters during non-polling Linux I/O.
   Disable or relocate inbound windows and verify that DMA/MSI no longer
   reaches the original targets; use poisoned low-address mirrors for
   queues and buffers above 4 GiB.
3. Attach the verified RP1 endpoint on PCIe2. Check BAR sizing/relocation,
   all 61 MSI-X allocations, table/PBA masks and pending replay, atomic
   set/clear operations, and level-source acknowledge/retrigger behavior.
4. Enable one verified RP1 peripheral and test it through its Linux driver,
   including repeated interrupts and device reset. Expand to USB keyboard
   enumeration and Ethernet only after clock, IRQ and DMA paths pass.

Primary sources
---------------

.. _BCM2712 device tree: https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/arch/arm64/boot/dts/broadcom/bcm2712.dtsi
.. _Linux PCIe driver: https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/pci/controller/pcie-brcmstb.c
.. _MIP driver: https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/irqchip/irq-bcm2712-mip.c
.. _RP1 PCI driver: https://github.com/torvalds/linux/blob/master/drivers/misc/rp1/rp1_pci.c
.. _RP1 datasheet: https://datasheets.raspberrypi.com/rp1/rp1-peripherals.pdf
.. _RP1 peripheral device tree: https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/broadcom/rp1-common.dtsi

This note and any resulting implementation are AI-assisted downstream
experimental work, with the provenance described in ``raspi5.rst``.
