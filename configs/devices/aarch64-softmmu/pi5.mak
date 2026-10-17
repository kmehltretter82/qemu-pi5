# Focused downstream Pi 5/500 build, including the storage regression device.
CONFIG_RASPI5=y
CONFIG_NVME_PCI=y
# NVMe's security command handlers reference this backend even when unused.
CONFIG_SPDM_SOCKET=y
