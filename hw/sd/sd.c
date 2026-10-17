/*
 * SD Memory Card emulation as defined in the "SD Memory Card Physical
 * layer specification, Version 2.00."
 *
 * eMMC emulation defined in "JEDEC Standard No. 84-A43"
 *
 * Copyright (c) 2006 Andrzej Zaborowski  <balrog@zabor.org>
 * Copyright (c) 2007 CodeSourcery
 * Copyright (c) 2018 Philippe Mathieu-Daudé
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/cutils.h"
#include "qemu/bswap.h"
#include "hw/core/irq.h"
#include "hw/core/registerfields.h"
#include "system/block-backend.h"
#include "hw/sd/sd.h"
#include "migration/vmstate.h"
#include "migration/blocker.h"
#include "qapi/error.h"
#include "qemu/bitmap.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qemu/log.h"
#include "qemu/guest-random.h"
#include "qemu/module.h"
#include "net/checksum.h"
#include "net/eth.h"
#include "net/net.h"
#include "sdmmc-internal.h"
#include "trace.h"
#include "crypto/hmac.h"

//#define DEBUG_SD 1

#define SDSC_MAX_CAPACITY   (2 * GiB)

#define INVALID_ADDRESS     UINT32_MAX

typedef enum {
    sd_r0 = 0,    /* no response */
    sd_r1,        /* normal response command */
    spi_r2,       /* STATUS */
    sd_r2_i,      /* CID register */
    sd_r2_s,      /* CSD register */
    sd_r3,        /* OCR register */
    sd_r6 = 6,    /* Published RCA response */
    sd_r7,        /* Operating voltage */
    sd_r1b = -1,
    sd_illegal = -2,
} sd_rsp_type_t;

typedef enum {
    sd_spi,
    sd_bc,     /* broadcast -- no response */
    sd_bcr,    /* broadcast with response */
    sd_ac,     /* addressed -- no data transfer */
    sd_adtc,   /* addressed with data transfer */
} sd_cmd_type_t;

enum SDCardModes {
    sd_inactive,
    sd_card_identification_mode,
    sd_data_transfer_mode,
};

enum SDCardStates {
    sd_waitirq_state        = -2, /* emmc */
    sd_inactive_state       = -1,

    sd_idle_state           = 0,
    sd_ready_state          = 1,
    sd_identification_state = 2,
    sd_standby_state        = 3,
    sd_transfer_state       = 4,
    sd_sendingdata_state    = 5,
    sd_receivingdata_state  = 6,
    sd_programming_state    = 7,
    sd_disconnect_state     = 8,
    sd_bus_test_state       = 9,  /* emmc */
    sd_sleep_state          = 10, /* emmc */
    sd_io_state             = 15  /* sd */
};

#define SDMMC_CMD_MAX 64

typedef sd_rsp_type_t (*sd_cmd_handler)(SDState *sd, SDRequest req);

typedef struct SDProto {
    const char *name;
    struct {
        const unsigned class;
        const sd_cmd_type_t type;
        const char *name;
        sd_cmd_handler handler;
    } cmd[SDMMC_CMD_MAX], acmd[SDMMC_CMD_MAX];
} SDProto;

#define RPMB_STUFF_LEN      196
#define RPMB_KEY_MAC_LEN    32
#define RPMB_DATA_LEN       256     /* one RPMB block is half a sector */
#define RPMB_NONCE_LEN      16
#define RPMB_HASH_LEN       284

typedef struct QEMU_PACKED {
    uint8_t stuff_bytes[RPMB_STUFF_LEN];
    uint8_t key_mac[RPMB_KEY_MAC_LEN];
    uint8_t data[RPMB_DATA_LEN];
    uint8_t nonce[RPMB_NONCE_LEN];
    uint32_t write_counter;
    uint16_t address;
    uint16_t block_count;
    uint16_t result;
    uint16_t req_resp;
} RPMBDataFrame;

QEMU_BUILD_BUG_MSG(sizeof(RPMBDataFrame) != 512,
                   "invalid RPMBDataFrame size");

struct SDState {
    DeviceState parent_obj;

    /* SD Memory Card Registers */
    uint32_t ocr;
    uint8_t scr[8];
    uint8_t cid[16];
    uint8_t csd[16];
    uint16_t rca;
    uint32_t card_status;
    uint8_t sd_status[64];
    union {
        uint8_t ext_csd[512];
        struct {
            uint8_t ext_csd_rw[192]; /* Modes segment */
            uint8_t ext_csd_ro[320]; /* Properties segment */
        };
    };

    /* Static properties */

    uint8_t spec_version;
    uint64_t boot_part_size;
    uint64_t rpmb_part_size;
    BlockBackend *blk;
    uint8_t boot_config;

    const SDProto *proto;

    /* Runtime changeables */

    int32_t state;    /* current card state, one of SDCardStates */
    uint32_t vhs;
    bool wp_switch;
    unsigned long *wp_group_bmap;
    int32_t wp_group_bits;
    uint64_t size;
    uint32_t blk_len;
    uint32_t multi_blk_cnt;
    uint32_t erase_start;
    uint32_t erase_end;
    uint8_t pwd[16];
    uint32_t pwd_len;
    uint8_t function_group[6];
    uint8_t current_cmd;
    const char *last_cmd_name;
    /* True if we will handle the next command as an ACMD. Note that this does
     * *not* track the APP_CMD status bit!
     */
    bool expecting_acmd;
    uint32_t blk_written;

    uint64_t data_start;
    uint32_t data_offset;
    size_t data_size;
    uint8_t data[512];
    struct {
        uint32_t write_counter;
        uint8_t key[RPMB_KEY_MAC_LEN];
        uint8_t key_set;
        RPMBDataFrame result;
    } rpmb;
    QEMUTimer *ocr_power_timer;
    uint8_t dat_lines;
    bool cmd_line;
    char *preset_auth_key;
};

/*
 * Experimental CYW43455 SDIO endpoint.
 *
 * This is intentionally a local development model, not an assertion that a
 * complete or hardware-faithful CYW43455 is implemented.  In addition to the
 * three-function identity, it provides a synthetic BCM4345/9 chip-discovery
 * surface on functions one and two so that brcmfmac can reach firmware
 * startup.  Function three is enumeration-only, matching the unbound WLAN
 * interface observed on a Pi 400.  A bounded synthetic
 * SDPCM control responder is provided for host-driver experiments.  Ethernet
 * frames can move in both directions through a QEMU network backend.  An
 * optional one-shot association event raises guest carrier after netdev open,
 * and bounded escan support reports one synthetic access point.  Firmware
 * execution and radio behavior remain unimplemented.
 */
#define CYW43455_SDIO_NUM_FUNCS         3
#define CYW43455_SDIO_LEGACY_REG_BANKS  3
#define CYW43455_SDIO_ADDR_SPACE_SIZE   (1U << 17)
#define CYW43455_SDIO_OCR               0x00ff8000U
#define CYW43455_SDIO_RCA               1
#define CYW43455_SDIO_VENDOR_ID         0x02d0
#define CYW43455_SDIO_DEVICE_ID         0xa9a6
#define CYW43455_SDIO_FUNC_MASK         \
    (((1U << (CYW43455_SDIO_NUM_FUNCS + 1)) - 1) & ~1U)
#define CYW43455_SDIO_INT_MASK          \
    ((1U << (CYW43455_SDIO_NUM_FUNCS + 1)) - 1)

#define SDIO_CCCR_REV                   0x00
#define SDIO_CCCR_SD_REV                0x01
#define SDIO_CCCR_IO_ENABLE             0x02
#define SDIO_CCCR_IO_READY              0x03
#define SDIO_CCCR_INT_ENABLE            0x04
#define SDIO_CCCR_INT_PENDING           0x05
#define SDIO_CCCR_ABORT                 0x06
#define SDIO_CCCR_BUS_IF                0x07
#define SDIO_CCCR_CAPS                  0x08
#define SDIO_CCCR_CIS_PTR               0x09
#define SDIO_CCCR_POWER                 0x12
#define SDIO_CCCR_SPEED                 0x13

#define SDIO_FBR_SIZE                   0x100
#define SDIO_FBR_STD_IF                 0x00
#define SDIO_FBR_CIS_PTR                0x09
#define SDIO_FBR_BLOCK_SIZE             0x10

#define SDIO_CISTPL_MANFID              0x20
#define SDIO_CISTPL_FUNCID              0x21
#define SDIO_CISTPL_FUNCE               0x22
#define SDIO_CISTPL_END                 0xff

#define SDIO_R4_READY                   BIT(31)
#define SDIO_R4_NUM_FUNCS_SHIFT         28
#define SDIO_R5_FUNCTION_NUMBER         BIT(9)
#define SDIO_R5_OUT_OF_RANGE            BIT(8)

#define SDIO_CMD52_WRITE                BIT(31)
#define SDIO_CMD52_FUNC_SHIFT           28
#define SDIO_CMD52_RAW                  BIT(27)
#define SDIO_CMD52_ADDR_SHIFT           9

#define SDIO_CMD53_WRITE                BIT(31)
#define SDIO_CMD53_FUNC_SHIFT           28
#define SDIO_CMD53_BLOCK_MODE           BIT(27)
#define SDIO_CMD53_INCREMENT            BIT(26)
#define SDIO_CMD53_ADDR_SHIFT           9

#define CYW43455_CIS_COMMON             0x1000
#define CYW43455_CIS_FUNC1              0x1100
#define CYW43455_CIS_FUNC2              0x1200
#define CYW43455_CIS_FUNC3              0x1300

#define CYW43455_CHIPCLKCSR              0x1000e
#define CYW43455_FORCE_ALP               0x01
#define CYW43455_FORCE_HT                0x02
#define CYW43455_ALP_AVAIL_REQ           0x08
#define CYW43455_HT_AVAIL_REQ            0x10
#define CYW43455_CLKCSR_CONTROL_MASK      0x3f
#define CYW43455_ALP_AVAIL               0x40
#define CYW43455_HT_AVAIL                0x80

#define CYW43455_SBADDRLOW               0x1000a
#define CYW43455_SBADDRMID               0x1000b
#define CYW43455_SBADDRHIGH              0x1000c
#define CYW43455_SB_ACCESS_START         0x08000
#define CYW43455_SB_ACCESS_END           0x10000
#define CYW43455_SB_OFFSET_MASK          0x07fff

/* Observed on a physical Pi 400; the remaining discovery data is synthetic. */
#define CYW43455_CHIP_ID                 0x15294345
#define CYW43455_ENUM_BASE               0x18000000
#define CYW43455_EROM_POINTER            0x180000fc
#define CYW43455_EROM_BASE               0x18110000

#define CYW43455_DMP_COMPONENT           0x00000001
#define CYW43455_DMP_MASTER_PORT         0x00000003
#define CYW43455_DMP_ADDRESS             0x00000005
#define CYW43455_DMP_EOT                 0x0000000f
#define CYW43455_DMP_NUM_MWRAP           (1U << 14)
#define CYW43455_DMP_MWRAP               (3U << 6)

#define CYW43455_BCMA_IOCTL              0x0408
#define CYW43455_BCMA_RESET_CTL          0x0800
#define CYW43455_ARMCR4_CAP              0x0004
#define CYW43455_ARMCR4_BANKIDX          0x0040
#define CYW43455_ARMCR4_BANKINFO         0x0044
#define CYW43455_ARMCR4_BANK_COUNT       3

#define CYW43455_SDIO_CORE_INDEX         4
#define CYW43455_SDIO_INTSTATUS          0x0020
#define CYW43455_SDIO_HOSTINTMASK        0x0024
#define CYW43455_SDIO_TOSBMAILBOX        0x0040
#define CYW43455_SDIO_TOHOSTMAILBOX      0x0044
#define CYW43455_SDIO_TOSBMAILBOXDATA    0x0048
#define CYW43455_SDIO_TOHOSTMAILBOXDATA  0x004c
#define CYW43455_I_HMB_FRAME_IND         BIT(6)

#define CYW43455_SDPCM_HW_HEADER_LEN     4
#define CYW43455_SDPCM_HWEXT_LEN         8
#define CYW43455_SDPCM_HWEXT_LAST        BIT(24)
#define CYW43455_SDPCM_HWEXT_LENGTH_MASK 0x00ffffff
#define CYW43455_SDPCM_HEADER_LEN        12
#define CYW43455_SDPCM_CONTROL_CHANNEL   0
#define CYW43455_SDPCM_EVENT_CHANNEL     1
#define CYW43455_SDPCM_GLOM_CHANNEL      3
#define CYW43455_RX_GLOM_MAX             8
#define CYW43455_SDPCM_DATA_CHANNEL      2
#define CYW43455_BCDC_HEADER_LEN         16
#define CYW43455_BCDC_DATA_HEADER_LEN    4
#define CYW43455_BCDC_PROTO_VER          2
#define CYW43455_BCDC_DCMD_ERROR         BIT(0)
#define CYW43455_BCDC_DCMD_SET           BIT(1)
#define CYW43455_BCME_BADARG             (-2)
#define CYW43455_BCME_BUSY               (-16)
#define CYW43455_BCME_UNSUPPORTED        (-23)
#define CYW43455_BCME_BADLEN             (-24)
#define CYW43455_BCME_NOTASSOCIATED      (-28)
#define CYW43455_C_GET_VERSION           1
#define CYW43455_C_UP                    2
#define CYW43455_C_DOWN                  3
#define CYW43455_C_SET_PROMISC           10
#define CYW43455_C_GET_INFRA             19
#define CYW43455_C_SET_INFRA             20
#define CYW43455_C_GET_SSID              25
#define CYW43455_C_SET_SSID              26
#define CYW43455_C_GET_REGULATORY        46
#define CYW43455_C_SET_REGULATORY        47
#define CYW43455_C_DISASSOC              52
#define CYW43455_C_GET_ROAM_TRIGGER      54
#define CYW43455_C_SET_ROAM_TRIGGER      55
#define CYW43455_C_GET_ROAM_DELTA        56
#define CYW43455_C_SET_ROAM_DELTA        57
#define CYW43455_C_GET_BCNPRD            75
#define CYW43455_C_SET_BCNPRD            76
#define CYW43455_C_GET_DTIMPRD           77
#define CYW43455_C_SET_DTIMPRD           78
#define CYW43455_C_GET_PM                 85
#define CYW43455_C_SET_PM                 86
#define CYW43455_C_GET_REVINFO           98
#define CYW43455_C_GET_AP                117
#define CYW43455_C_SET_AP                118
#define CYW43455_C_GET_PHY_NOISE         135
#define CYW43455_C_GET_BSS_INFO          136
#define CYW43455_C_GET_BANDLIST          140
#define CYW43455_C_GET_ASSOCLIST         159
#define CYW43455_C_GET_VAR               262
#define CYW43455_C_SET_VAR               263
#define CYW43455_D11AC_IOTYPE            2
#define CYW43455_WLC_BAND_AUTO           0
#define CYW43455_WLC_BAND_5G             1
#define CYW43455_WLC_BAND_2G             2
#define CYW43455_WLC_BAND_ALL            3
#define CYW43455_PM_OFF                   0
#define CYW43455_PM_MAX                   1
#define CYW43455_PM_FAST                  2
#define CYW43455_OBSS_COEX_AUTO          (-1)
#define CYW43455_OBSS_COEX_OFF             0
#define CYW43455_OBSS_COEX_ON              1
#define CYW43455_QTXPOWER_OVERRIDE        BIT(31)
#define CYW43455_QTXPOWER_MAX_QDBM        127
#define CYW43455_QTXPOWER_DEFAULT_QDBM    124
#define CYW43455_MFP_NONE                   0
#define CYW43455_MFP_CAPABLE                1
#define CYW43455_MFP_REQUIRED               2
#define CYW43455_PNO_VERSION                 2
#define CYW43455_PNO_MIN_PERIOD             10
#define CYW43455_PNO_MAX_PERIOD            508
#define CYW43455_PNO_PARAM_LEN              24
#define CYW43455_PNO_CONFIG_LEN            140
#define CYW43455_PNO_NET_PARAM_LEN          56
#define CYW43455_PNO_MACADDR_LEN             8
#define CYW43455_PNO_MACADDR_VERSION         1
#define CYW43455_PNO_MAC_FLAGS_MASK          3
#define CYW43455_PNO_GSCAN_VERSION           2
#define CYW43455_PNO_GSCAN_HEADER_LEN        10
#define CYW43455_PNO_GSCAN_BUCKET_LEN         8
#define CYW43455_PNO_MAX_BUCKETS              1
#define CYW43455_PNO_MAX_CHANNELS            64
#define CYW43455_PNO_MAX_NETWORKS            16
#define CYW43455_PNO_SSID_MAX_LEN            32
#define CYW43455_PNO_HIDDEN                 BIT(2)
#define CYW43455_CHANSPEC_2G_CH1_20MHZ   0x1001
#define CYW43455_CHANSPEC_2G_CH13_40MHZ  0x190b
#define CYW43455_CHANSPEC_5G_CH36_20MHZ  0xd024
#define CYW43455_CHANSPEC_CHANNEL_MASK   0x00ff
#define CYW43455_CHANSPEC_SIDEBAND_MASK  0x0700
#define CYW43455_CHANSPEC_SIDEBAND_LOWER 0x0000
#define CYW43455_CHANSPEC_SIDEBAND_UPPER 0x0100
#define CYW43455_CHANSPEC_BANDWIDTH_MASK 0x3800
#define CYW43455_CHANSPEC_BANDWIDTH_20   0x1000
#define CYW43455_CHANSPEC_BANDWIDTH_40   0x1800
#define CYW43455_CHANSPEC_BAND_MASK      0xc000
#define CYW43455_CHANSPEC_BAND_5G        0xc000
#define CYW43455_CHANNEL_2G_MIN           1
#define CYW43455_CHANNEL_2G_MAX           13
#define CYW43455_CHANNEL_40_CENTER_MIN    3
#define CYW43455_CHANNEL_40_CENTER_MAX    11
#define CYW43455_BW_CAP_20MHZ             BIT(0)
#define CYW43455_BW_CAP_40MHZ             BIT(1)
#define CYW43455_BW_CAP_2G                \
    (CYW43455_BW_CAP_20MHZ | CYW43455_BW_CAP_40MHZ)
#define CYW43455_CHANSPEC_20_COUNT        13
#define CYW43455_CHANSPEC_40_COUNT        18
#define CYW43455_CHANSPEC_ALL_COUNT       \
    (CYW43455_CHANSPEC_20_COUNT + CYW43455_CHANSPEC_40_COUNT + 1)
#define CYW43455_CHANSPEC_LIST_LEN(_count) \
    (sizeof(uint32_t) * (1 + (_count)))
#define CYW43455_F2_FRAME_MAX            (16 * KiB)
/* Pi 400 7.84.17.1: 294 observed control replies advanced by 41. */
#define CYW43455_TX_CREDIT_WINDOW_DEFAULT 41
#define CYW43455_TX_CREDIT_WINDOW_MAX    64
#define CYW43455_MULTICAST_MAX           32
#define CYW43455_ARP_HOSTIP_MAX          8
#define CYW43455_ARP_OL_AGENT            BIT(0)
#define CYW43455_ND_HOSTIP_MAX           8
#define CYW43455_ICMP6_NEXT_HEADER       58
#define CYW43455_ICMP6_NS                135
#define CYW43455_ICMP6_NA                136
#define CYW43455_ND_PAYLOAD_LEN          32
#define CYW43455_STA_INFO_VERSION        4
#define CYW43455_STA_INFO_LEN            172
#define CYW43455_STA_INFO_FLAGS_OFFSET   8
#define CYW43455_STA_INFO_IDLE_OFFSET    12
#define CYW43455_STA_INFO_EA_OFFSET      16
#define CYW43455_STA_INFO_COUNT_OFFSET   24
#define CYW43455_STA_INFO_RATES_OFFSET   28
#define CYW43455_STA_INFO_IN_OFFSET      44
#define CYW43455_STA_INFO_TX_PKTS_OFFSET 52
#define CYW43455_STA_INFO_TX_FAIL_OFFSET 56
#define CYW43455_STA_INFO_RX_UCAST_OFFSET 60
#define CYW43455_STA_INFO_RX_MCAST_OFFSET 64
#define CYW43455_STA_INFO_TX_RATE_OFFSET 68
#define CYW43455_STA_INFO_RX_RATE_OFFSET 72
#define CYW43455_STA_INFO_TX_TOTAL_OFFSET 84
#define CYW43455_STA_INFO_RX_TOTAL_OFFSET 88
#define CYW43455_STA_INFO_TX_MCAST_OFFSET 92
#define CYW43455_STA_INFO_TX_BYTES_OFFSET 96
#define CYW43455_STA_INFO_RX_BYTES_OFFSET 104
#define CYW43455_STA_INFO_TX_UCAST_BYTES_OFFSET 112
#define CYW43455_STA_INFO_TX_MCAST_BYTES_OFFSET 120
#define CYW43455_STA_INFO_RX_UCAST_BYTES_OFFSET 128
#define CYW43455_STA_INFO_RX_MCAST_BYTES_OFFSET 136
#define CYW43455_STA_INFO_RSSI_OFFSET    144
#define CYW43455_STA_INFO_NF_OFFSET      148
#define CYW43455_STA_INFO_AID_OFFSET     152
#define CYW43455_STA_INFO_RX_LAST_RSSI_OFFSET 168
#define CYW43455_STA_WME                 BIT(1)
#define CYW43455_STA_AUTHE               BIT(3)
#define CYW43455_STA_ASSOC               BIT(4)
#define CYW43455_STA_AUTHO               BIT(5)
#define CYW43455_STA_SCBSTATS            BIT(14)
#define CYW43455_STA_FLAGS               \
    (CYW43455_STA_WME | CYW43455_STA_AUTHE | CYW43455_STA_ASSOC | \
     CYW43455_STA_AUTHO | CYW43455_STA_SCBSTATS)
#define CYW43455_BSS_INFO_VERSION        109
#define CYW43455_BSS_INFO_LEN            128
#define CYW43455_BSS_INFO_PREFIX_LEN     4
#define CYW43455_BSS_INFO_BSSID_OFFSET   8
#define CYW43455_BSS_INFO_BEACON_OFFSET  14
#define CYW43455_BSS_INFO_CAP_OFFSET     16
#define CYW43455_BSS_INFO_SSID_LEN_OFFSET 18
#define CYW43455_BSS_INFO_SSID_OFFSET    19
#define CYW43455_BSS_INFO_RATE_COUNT_OFFSET 52
#define CYW43455_BSS_INFO_RATES_OFFSET  56
#define CYW43455_BSS_INFO_CHANSPEC_OFFSET 72
#define CYW43455_BSS_INFO_DTIM_OFFSET    76
#define CYW43455_BSS_INFO_RSSI_OFFSET    78
#define CYW43455_BSS_INFO_NOISE_OFFSET   80
#define CYW43455_BSS_INFO_N_CAP_OFFSET   81
#define CYW43455_BSS_INFO_CTL_CH_OFFSET  88
#define CYW43455_BSS_INFO_IE_OFFSET      116
#define CYW43455_BSS_INFO_IE_LEN_OFFSET  120
#define CYW43455_BSS_INFO_SNR_OFFSET     124
#define CYW43455_BSS_BEACON_PERIOD       100
#define CYW43455_BSS_DTIM_PERIOD         1
#define CYW43455_BSS_SSID                "qemu-cyw43455"
#define CYW43455_WLAN_CAPABILITY_ESS     BIT(0)
#define CYW43455_WLAN_CAPABILITY_SHORT_PREAMBLE BIT(5)
#define CYW43455_WLAN_CAPABILITY_SHORT_SLOT_TIME BIT(10)
#define CYW43455_BSS_CAPABILITIES        \
    (CYW43455_WLAN_CAPABILITY_ESS | \
     CYW43455_WLAN_CAPABILITY_SHORT_PREAMBLE | \
     CYW43455_WLAN_CAPABILITY_SHORT_SLOT_TIME)
#define CYW43455_LINK_RATE_KBPS          72200
#define CYW43455_SIGNAL_DBM              (-28)
#define CYW43455_NOISE_DBM               (-95)
#define CYW43455_WLC_VERSION_STRUCT_VERSION 1
#define CYW43455_WLC_VERSION_LEN         16
#define CYW43455_EPI_VERSION_MAJOR       7
#define CYW43455_EPI_VERSION_MINOR       84
#define CYW43455_EPI_VERSION_RC          17
#define CYW43455_EPI_VERSION_INCREMENT   1
#define CYW43455_WLC_VERSION_MAJOR       7
#define CYW43455_WLC_VERSION_MINOR       84
#define CYW43455_BRCM_EVENT_HEADER_LEN   10
#define CYW43455_EVENT_MSG_LEN           48
#define CYW43455_EVENT_ETHERTYPE         0x886c
#define CYW43455_EVENT_VENDOR_LONG       0x8001
#define CYW43455_EVENT_USER_SUBTYPE      1
#define CYW43455_EVENT_MSG_VERSION       2
#define CYW43455_EVENT_MSG_LINK          BIT(0)
#define CYW43455_EVENT_SET_SSID          0
#define CYW43455_EVENT_ASSOC_IND         8
#define CYW43455_EVENT_DISASSOC_IND      12
#define CYW43455_EVENT_LINK              16
#define CYW43455_AP_EVENT_UP             BIT(0)
#define CYW43455_AP_EVENT_ASSOC          BIT(1)
#define CYW43455_AP_EVENT_DISASSOC       BIT(2)
#define CYW43455_AP_EVENT_DOWN           BIT(3)
#define CYW43455_AP_EVENT_MASK           0x0f
#define CYW43455_AP_JOIN_LEN             52
#define CYW43455_AP_SSID_MAX             32
#define CYW43455_EVENT_ESCAN_RESULT      69
#define CYW43455_EVENT_FIFO_CREDIT_MAP   74
#define CYW43455_EVENT_BCMC_CREDIT_SUPPORT 127
#define CYW43455_FWS_EVENT_MASK_MIN      16
#define CYW43455_FWS_EVENT_MASK_MAX      32
#define CYW43455_FWS_TLV_FLAGS           0x4f
#define CYW43455_FWS_TXSTATUS            4
#define CYW43455_FWS_PKTTAG              5
#define CYW43455_FWS_INTERFACE_OPEN      9
#define CYW43455_FWS_INTERFACE_CLOSE     10
#define CYW43455_FWS_FIFO_CREDITBACK     11
#define CYW43455_FWS_PENDING_TRAFFIC     12
#define CYW43455_FWS_FILLER              255
#define CYW43455_FWS_TAG_FLAGS           0x78000000U
#define CYW43455_FWS_TAG_FROM_HOST       BIT(27)
#define CYW43455_FWS_STATUS_TOSSED       (3U << 27)
#define CYW43455_EVENT_STATUS_SUCCESS    0
#define CYW43455_EVENT_STATUS_PARTIAL    8
#define CYW43455_LINK_EVENT_LEN          \
    (CYW43455_SDPCM_HEADER_LEN + CYW43455_BCDC_DATA_HEADER_LEN + \
     ETH_HLEN + CYW43455_BRCM_EVENT_HEADER_LEN + CYW43455_EVENT_MSG_LEN)
#define CYW43455_ESCAN_REQ_VERSION_V1    1
#define CYW43455_ESCAN_REQ_VERSION_V2    2
#define CYW43455_ESCAN_RESULT_VERSION    1
#define CYW43455_ESCAN_ACTION_START      1
#define CYW43455_ESCAN_HEADER_LEN        8
#define CYW43455_SCAN_PARAMS_V1_FIXED_LEN 64
#define CYW43455_SCAN_PARAMS_V2_FIXED_LEN 72
#define CYW43455_SCAN_PARAMS_V2_VERSION_OFFSET 0
#define CYW43455_SCAN_PARAMS_V2_LENGTH_OFFSET 2
#define CYW43455_SCAN_SSID_LEN           36
#define CYW43455_SCAN_SSID_MAX_LEN       32
#define CYW43455_SCAN_SSID_LEN_OFFSET    0
#define CYW43455_SCAN_SSID_OFFSET        4
#define CYW43455_SURVEY_RESULTS_MAX      512
#define CYW43455_ESCAN_RESULT_FIXED_LEN  12
#define CYW43455_BSS_INFO_IE_BYTES       24
#define CYW43455_ESCAN_BSS_INFO_LEN      \
    (CYW43455_BSS_INFO_LEN + CYW43455_BSS_INFO_IE_BYTES)
#define CYW43455_ESCAN_RESULT_LEN        \
    (CYW43455_ESCAN_RESULT_FIXED_LEN + CYW43455_ESCAN_BSS_INFO_LEN)

enum {
    CYW43455_ESCAN_EVENT_NONE,
    CYW43455_ESCAN_EVENT_PARTIAL,
    CYW43455_ESCAN_EVENT_COMPLETE,
};

typedef struct CYW43455CoreInfo {
    uint16_t id;
    uint8_t revision;
    uint32_t base;
    uint32_t wrapper;
} CYW43455CoreInfo;

typedef struct CYW43455ScanLayout {
    uint32_t params_fixed_len;
    uint32_t ssid_offset;
    uint32_t bssid_offset;
    uint32_t bss_type_offset;
    uint32_t channel_num_offset;
} CYW43455ScanLayout;

typedef struct CYW43455FWSStatus {
    uint32_t tag;
    int64_t due_ms;
} CYW43455FWSStatus;

/* Common BCM4345 topology; verify revisions and descriptors on a Pi 400. */
static const CYW43455CoreInfo cyw43455_sdio_cores[] = {
    { 0x800, 51, 0x18000000, 0x18100000 }, /* ChipCommon */
    { 0x812, 54, 0x18001000, 0x18101000 }, /* D11 */
    { 0x83e, 9,  0x18002000, 0x18102000 }, /* ARM CR4 */
    { 0x83c, 14, 0x18003000, 0x18103000 }, /* PCIe Gen2 */
    { 0x829, 21, 0x18004000, 0x18104000 }, /* SDIO device */
};

static const uint8_t cyw43455_sdio_default_mac[ETH_ALEN] = {
    0x02, 0, 0, 0, 0, 1,
};

static const uint8_t cyw43455_sdio_peer_mac[ETH_ALEN] = {
    0x52, 0x55, 0x0a, 0x00, 0x02, 0x02,
};

OBJECT_DECLARE_SIMPLE_TYPE(CYW43455SDIOState, CYW43455_SDIO)

struct CYW43455SDIOState {
    SDState parent_obj;

    uint32_t hardware_profile;
    NICConf conf;
    NICState *nic;
    uint8_t current_mac[ETH_ALEN];
    /* Keep the version 1 migration layout for functions zero through two. */
    uint8_t regs[CYW43455_SDIO_LEGACY_REG_BANKS]
                [CYW43455_SDIO_ADDR_SPACE_SIZE];
    uint8_t f3_regs[CYW43455_SDIO_ADDR_SPACE_SIZE];
    uint32_t transfer_addr;
    uint32_t transfer_remaining;
    uint8_t transfer_func;
    bool transfer_write;
    bool transfer_increment;
    bool selected;
    uint32_t wrapper_ioctl[ARRAY_SIZE(cyw43455_sdio_cores)];
    uint32_t wrapper_reset[ARRAY_SIZE(cyw43455_sdio_cores)];
    uint32_t armcr4_bankidx;
    uint32_t sdio_intstatus;
    uint32_t sdio_hostintmask;
    uint32_t sdio_tosbmailbox;
    uint32_t sdio_tohostmailbox;
    uint32_t sdio_tosbmailboxdata;
    uint32_t sdio_tohostmailboxdata;
    uint8_t f2_tx_frame[CYW43455_F2_FRAME_MAX];
    uint8_t f2_rx_frame[CYW43455_F2_FRAME_MAX];
    uint8_t f2_rx_pending_frame[CYW43455_F2_FRAME_MAX];
    uint32_t f2_tx_len;
    uint32_t f2_rx_len;
    uint32_t f2_rx_pos;
    uint32_t f2_rx_pending_len;
    uint8_t f2_rx_seq;
    QEMUTimer *rx_glom_timer;
    uint32_t rx_glom_delay_ms;
    uint8_t rx_glom_max_frames;
    uint16_t rx_glom_align;
    uint8_t rx_glom_count;
    uint16_t rx_glom_lengths[CYW43455_RX_GLOM_MAX];
    uint8_t rx_glom_frame[CYW43455_F2_FRAME_MAX];
    uint32_t rx_glom_len;
    bool rx_glom_ready;
    bool rx_glom_super_pending;
    QEMUTimer *tx_credit_timer;
    uint32_t tx_credit_delay_ms;
    uint8_t tx_credit_window;
    uint8_t tx_credit_seq;
    uint8_t tx_credit_limit;
    bool tx_credit_pending;
    bool tx_credit_update_pending;
    /* Unsaved private FWS state blocks migration until a cold reset. */
    bool fws_enabled;
    uint8_t fws_credit_budget;
    uint8_t fws_status_batch_max;
    uint32_t fws_startup_delay_ms;
    uint32_t fws_status_delay_ms;
    uint32_t fws_credit_delay_ms;
    QEMUTimer *fws_timer;
    Error *fws_migration_blocker;
    bool fws_negotiated;
    bool fws_map_sent;
    bool fws_bcmc_sent;
    bool fws_if_open;
    uint8_t fws_if_pending;
    bool fws_if_reopen_pending;
    uint8_t fws_event_mask[CYW43455_FWS_EVENT_MASK_MAX];
    uint8_t fws_ac_available;
    uint8_t fws_bcmc_available;
    uint8_t fws_credit_pending[CYW43455_FWS_FIFO_COUNT];
    bool fws_hanger_active[CYW43455_FWS_HANGER_SLOTS];
    CYW43455FWSStatus fws_status[CYW43455_FWS_STATUS_MAX];
    uint8_t fws_status_head;
    uint8_t fws_status_count;
    uint8_t fws_retiring_count;
    uint32_t fws_retiring_tags[CYW43455_FWS_STATUS_BATCH_MAX];
    int64_t fws_map_due_ms;
    int64_t fws_credit_due_ms;
    bool rx_data_enabled;
    bool auto_associate;
    bool explicit_association;
    bool association_requested;
    bool link_ssid_pending;
    bool link_event_pending;
    bool link_event_sent;
    bool link_event_up;
    bool firmware_connected;
    bool backend_link_events_armed;
    /* Private open, primary-BSS HLE; this does not execute radio firmware. */
    bool explicit_role;
    bool ap_mode;
    bool ap_started;
    bool ap_peer_associated;
    bool ap_channel_configured;
    bool ap_hidden;
    bool infra;
    bool regulatory;
    uint16_t ap_beacon_period;
    uint8_t ap_dtim_period;
    uint8_t ap_ssid_len;
    uint8_t ap_ssid[CYW43455_AP_SSID_MAX];
    uint8_t ap_events;
    uint8_t escan_event_phase;
    uint16_t escan_sync_id;
    uint32_t ap_chanspec;
    uint32_t chanspec;
    uint32_t survey_duration_ms;
    bool survey_active;
    uint8_t multicast[CYW43455_MULTICAST_MAX][ETH_ALEN];
    uint32_t multicast_count;
    uint8_t arp_hostip[CYW43455_ARP_HOSTIP_MAX][sizeof(uint32_t)];
    uint32_t arp_hostip_count;
    uint32_t arp_mode;
    uint8_t nd_hostip[CYW43455_ND_HOSTIP_MAX][16];
    uint32_t nd_hostip_count;
    uint32_t pm2_sleep_ret;
    uint32_t power_mode;
    int32_t obss_coex;
    uint32_t qtxpower;
    uint32_t mfp_mode;
    uint16_t pfn_channels[CYW43455_PNO_MAX_CHANNELS];
    uint8_t pfn_ssid_len[CYW43455_PNO_MAX_NETWORKS];
    uint8_t pfn_ssids[CYW43455_PNO_MAX_NETWORKS]
                     [CYW43455_PNO_SSID_MAX_LEN];
    uint32_t pfn_network_flags[CYW43455_PNO_MAX_NETWORKS];
    uint8_t pfn_scan_mac[ETH_ALEN];
    uint32_t pfn_scan_freq;
    uint32_t pfn_channel_count;
    uint32_t pfn_network_count;
    uint8_t pfn_scan_mac_flags;
    uint8_t pfn_bucket_count;
    bool pfn_enabled;
    bool tdls_enabled;
    bool roam_off;
    uint32_t bcn_timeout;
    int32_t roam_trigger;
    int32_t roam_delta;
    uint64_t tx_bytes;
    uint64_t rx_bytes;
    uint64_t tx_multicast_bytes;
    uint64_t rx_multicast_bytes;
    uint32_t tx_packets;
    uint32_t rx_packets;
    uint32_t tx_multicast_packets;
    uint32_t rx_multicast_packets;
    int64_t association_time_ms;
    bool arp_offload;
    bool nd_offload;
    bool allmulti;
    bool promiscuous;
};

static void cyw43455_sdio_emit_tx_credit_frame(CYW43455SDIOState *s);
static void cyw43455_sdio_emit_rx_glom(CYW43455SDIOState *s);
static void cyw43455_sdio_advance_rx(CYW43455SDIOState *s);

/* Only identification is calibrated here, not proprietary firmware execution. */
static uint16_t cyw43455_sdio_device_id(CYW43455SDIOState *s)
{
    return s->hardware_profile == CYW43455_SDIO_PROFILE_PI5 ?
           0x4345 : CYW43455_SDIO_DEVICE_ID;
}

static uint8_t cyw43455_sdio_chip_revision(CYW43455SDIOState *s)
{
    return s->hardware_profile == CYW43455_SDIO_PROFILE_PI5 ? 6 : 9;
}

static bool cyw43455_sdio_link_available(CYW43455SDIOState *s)
{
    if (s->ap_mode) {
        return s->ap_started && s->rx_data_enabled &&
               s->ap_peer_associated &&
               !qemu_get_queue(s->nic)->link_down;
    }
    return !(s->explicit_association || s->explicit_role) ||
           (s->firmware_connected &&
            !(s->link_event_pending && !s->link_event_up) &&
            !qemu_get_queue(s->nic)->link_down);
}

static SDBus *cyw43455_sdio_bus(CYW43455SDIOState *s)
{
    BusState *bus = qdev_get_parent_bus(DEVICE(s));

    return bus ? SD_BUS(bus) : NULL;
}

static uint8_t *cyw43455_sdio_func_regs(CYW43455SDIOState *s,
                                        unsigned int function)
{
    return function == 3 ? s->f3_regs : s->regs[function];
}

static bool cyw43455_sdio_card_irq_level(CYW43455SDIOState *s)
{
    uint8_t enabled = s->regs[0][SDIO_CCCR_INT_ENABLE];
    uint8_t pending = s->regs[0][SDIO_CCCR_INT_PENDING];

    return (enabled & BIT(0)) && (enabled & pending & ~BIT(0));
}

static void cyw43455_sdio_update_irq(CYW43455SDIOState *s)
{
    SDBus *bus;
    bool pending;

    pending = s->sdio_intstatus & s->sdio_hostintmask;
    s->regs[0][SDIO_CCCR_INT_PENDING] = pending ? BIT(1) : 0;

    bus = cyw43455_sdio_bus(s);
    if (bus) {
        sdbus_set_card_irq(bus, cyw43455_sdio_card_irq_level(s));
    }
}

static bool cyw43455_sdio_rx_ready(CYW43455SDIOState *s)
{
    return s->nic &&
           cyw43455_sdio_link_available(s) &&
           s->rx_data_enabled &&
           (s->regs[0][SDIO_CCCR_IO_ENABLE] & BIT(2)) &&
           !s->f2_rx_len &&
           !s->f2_rx_pending_len &&
           !s->link_event_pending &&
           !s->ap_events &&
           !s->escan_event_phase &&
           !s->rx_glom_ready && !s->rx_glom_super_pending &&
           !(s->sdio_intstatus & CYW43455_I_HMB_FRAME_IND);
}

static void cyw43455_sdio_flush_queued_packets(CYW43455SDIOState *s)
{
    if (cyw43455_sdio_rx_ready(s)) {
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
    }
}

static bool cyw43455_sdio_chanspec_info(uint32_t chanspec,
                                        uint8_t *control_channel,
                                        uint8_t *bandwidth_mhz)
{
    uint32_t center_channel = chanspec & CYW43455_CHANSPEC_CHANNEL_MASK;
    uint32_t sideband = chanspec & CYW43455_CHANSPEC_SIDEBAND_MASK;
    uint32_t bandwidth = chanspec & CYW43455_CHANSPEC_BANDWIDTH_MASK;
    int control;

    if (chanspec & ~(CYW43455_CHANSPEC_CHANNEL_MASK |
                     CYW43455_CHANSPEC_SIDEBAND_MASK |
                     CYW43455_CHANSPEC_BANDWIDTH_MASK |
                     CYW43455_CHANSPEC_BAND_MASK)) {
        return false;
    }
    if ((chanspec & CYW43455_CHANSPEC_BAND_MASK) ==
        CYW43455_CHANSPEC_BAND_5G) {
        if (chanspec != CYW43455_CHANSPEC_5G_CH36_20MHZ) {
            return false;
        }
        *control_channel = 36;
        *bandwidth_mhz = 20;
        return true;
    }
    if ((chanspec & CYW43455_CHANSPEC_BAND_MASK) != 0) {
        return false;
    }

    switch (bandwidth) {
    case CYW43455_CHANSPEC_BANDWIDTH_20:
        if (sideband != CYW43455_CHANSPEC_SIDEBAND_LOWER ||
            center_channel < CYW43455_CHANNEL_2G_MIN ||
            center_channel > CYW43455_CHANNEL_2G_MAX) {
            return false;
        }
        control = center_channel;
        *bandwidth_mhz = 20;
        break;
    case CYW43455_CHANSPEC_BANDWIDTH_40:
        if (center_channel < CYW43455_CHANNEL_40_CENTER_MIN ||
            center_channel > CYW43455_CHANNEL_40_CENTER_MAX ||
            (sideband != CYW43455_CHANSPEC_SIDEBAND_LOWER &&
             sideband != CYW43455_CHANSPEC_SIDEBAND_UPPER)) {
            return false;
        }
        control = center_channel +
                  (sideband == CYW43455_CHANSPEC_SIDEBAND_UPPER ? 2 : -2);
        *bandwidth_mhz = 40;
        break;
    default:
        return false;
    }

    if (control < CYW43455_CHANNEL_2G_MIN ||
        control > CYW43455_CHANNEL_2G_MAX) {
        return false;
    }
    *control_channel = control;
    return true;
}

static uint32_t cyw43455_sdio_ap_ies(CYW43455SDIOState *s, uint8_t *output,
                                    bool ds_parameter)
{
    uint8_t *ie = output;

    *ie++ = 0; /* SSID, including embedded zero bytes by its explicit length. */
    *ie++ = s->ap_ssid_len;
    memcpy(ie, s->ap_ssid, s->ap_ssid_len);
    ie += s->ap_ssid_len;
    *ie++ = 1; /* Basic OFDM 6, 12 and 24 Mbit/s; no synthetic HT rate IE. */
    *ie++ = 3;
    memcpy(ie, "\x8c\x98\xb0", 3);
    ie += 3;
    if (ds_parameter) {
        *ie++ = 3;
        *ie++ = 1;
        *ie++ = s->chanspec & CYW43455_CHANSPEC_CHANNEL_MASK;
    }
    return ie - output;
}

static uint32_t cyw43455_sdio_ap_ie_len(CYW43455SDIOState *s)
{
    return s->ap_ssid_len + 7 +
           ((s->chanspec & CYW43455_CHANSPEC_BAND_MASK) ? 0 : 3);
}

static void cyw43455_sdio_fill_bss_info(CYW43455SDIOState *s,
                                        uint8_t *bss, uint32_t length,
                                        bool include_ies, bool local_ap)
{
    const size_t ssid_len = local_ap ? s->ap_ssid_len :
                                      strlen(CYW43455_BSS_SSID);
    const uint8_t *ssid = local_ap ? s->ap_ssid :
                                    (const uint8_t *)CYW43455_BSS_SSID;
    uint32_t chanspec = local_ap ? s->chanspec : s->ap_chanspec;
    uint32_t ie_len = local_ap ? cyw43455_sdio_ap_ie_len(s) :
                                CYW43455_BSS_INFO_IE_BYTES;
    uint8_t bandwidth_mhz;
    uint8_t control_channel;

    assert(length >= CYW43455_BSS_INFO_LEN);
    assert(!include_ies || length >= CYW43455_BSS_INFO_LEN + ie_len);
    assert(cyw43455_sdio_chanspec_info(chanspec, &control_channel,
                                      &bandwidth_mhz));
    memset(bss, 0, length);
    stl_le_p(bss, CYW43455_BSS_INFO_VERSION);
    stl_le_p(bss + 4, length);
    memcpy(bss + CYW43455_BSS_INFO_BSSID_OFFSET,
           local_ap ? s->current_mac : cyw43455_sdio_peer_mac, ETH_ALEN);
    stw_le_p(bss + CYW43455_BSS_INFO_BEACON_OFFSET,
             local_ap ? s->ap_beacon_period : CYW43455_BSS_BEACON_PERIOD);
    stw_le_p(bss + CYW43455_BSS_INFO_CAP_OFFSET,
             CYW43455_BSS_CAPABILITIES);
    bss[CYW43455_BSS_INFO_SSID_LEN_OFFSET] = ssid_len;
    memcpy(bss + CYW43455_BSS_INFO_SSID_OFFSET,
           ssid, ssid_len);
    stl_le_p(bss + CYW43455_BSS_INFO_RATE_COUNT_OFFSET, local_ap ? 3 : 1);
    if (local_ap) {
        memcpy(bss + CYW43455_BSS_INFO_RATES_OFFSET, "\x8c\x98\xb0", 3);
    } else {
        bss[CYW43455_BSS_INFO_RATES_OFFSET] = 144; /* Historical STA HLE. */
    }
    stw_le_p(bss + CYW43455_BSS_INFO_CHANSPEC_OFFSET,
             chanspec);
    bss[CYW43455_BSS_INFO_DTIM_OFFSET] = local_ap ? s->ap_dtim_period :
                                                 CYW43455_BSS_DTIM_PERIOD;
    stw_le_p(bss + CYW43455_BSS_INFO_RSSI_OFFSET,
             (uint16_t)CYW43455_SIGNAL_DBM);
    bss[CYW43455_BSS_INFO_NOISE_OFFSET] = CYW43455_NOISE_DBM;
    bss[CYW43455_BSS_INFO_N_CAP_OFFSET] = !local_ap;
    bss[CYW43455_BSS_INFO_CTL_CH_OFFSET] = control_channel;
    stw_le_p(bss + CYW43455_BSS_INFO_IE_OFFSET,
             CYW43455_BSS_INFO_LEN);
    stl_le_p(bss + CYW43455_BSS_INFO_IE_LEN_OFFSET,
             include_ies ? ie_len : 0);
    stw_le_p(bss + CYW43455_BSS_INFO_SNR_OFFSET,
             CYW43455_SIGNAL_DBM - CYW43455_NOISE_DBM);

    if (include_ies) {
        uint8_t *ie = bss + CYW43455_BSS_INFO_LEN;

        if (local_ap) {
            cyw43455_sdio_ap_ies(s, ie,
                                !(chanspec & CYW43455_CHANSPEC_BAND_MASK));
            return;
        }
        *ie++ = 0; /* SSID */
        *ie++ = ssid_len;
        memcpy(ie, CYW43455_BSS_SSID, ssid_len);
        ie += ssid_len;
        *ie++ = 1; /* supported rates */
        *ie++ = 4;
        memcpy(ie, "\x82\x84\x8b\x96", 4);
        ie += 4;
        *ie++ = 3; /* DS parameter set */
        *ie++ = 1;
        *ie = control_channel;
    }
}

static void cyw43455_sdio_emit_event_addr(CYW43455SDIOState *s,
                                     uint32_t event_type, uint32_t status,
                                     uint16_t flags, const uint8_t *data,
                                     uint32_t data_len, const uint8_t *address)
{
    uint8_t *frame = s->f2_rx_frame;
    uint8_t *bcdc = frame + CYW43455_SDPCM_HEADER_LEN;
    uint8_t *ethernet = bcdc + CYW43455_BCDC_DATA_HEADER_LEN;
    uint8_t *brcm = ethernet + ETH_HLEN;
    uint8_t *event = brcm + CYW43455_BRCM_EVENT_HEADER_LEN;
    size_t frame_len = CYW43455_LINK_EVENT_LEN + data_len;
    size_t stored_len = QEMU_ALIGN_UP(frame_len, sizeof(uint32_t));

    assert(frame_len <= sizeof(s->f2_rx_frame));
    assert(!s->f2_rx_len);
    assert(!(s->sdio_intstatus & CYW43455_I_HMB_FRAME_IND));

    memset(frame, 0, stored_len);
    stw_le_p(frame, frame_len);
    stw_le_p(frame + 2, (uint16_t)~frame_len);
    frame[4] = s->f2_rx_seq++;
    frame[5] = CYW43455_SDPCM_EVENT_CHANNEL;
    frame[7] = CYW43455_SDPCM_HEADER_LEN;
    frame[9] = s->tx_credit_limit;
    s->tx_credit_update_pending = false;

    bcdc[0] = CYW43455_BCDC_PROTO_VER << 4;
    memcpy(ethernet, s->current_mac, ETH_ALEN);
    memcpy(ethernet + ETH_ALEN, cyw43455_sdio_peer_mac, ETH_ALEN);
    stw_be_p(ethernet + 2 * ETH_ALEN, CYW43455_EVENT_ETHERTYPE);

    stw_be_p(brcm, CYW43455_EVENT_VENDOR_LONG);
    stw_be_p(brcm + 2, CYW43455_EVENT_MSG_LEN + data_len);
    memcpy(brcm + 5, "\x00\x10\x18", 3);
    stw_be_p(brcm + 8, CYW43455_EVENT_USER_SUBTYPE);

    stw_be_p(event, CYW43455_EVENT_MSG_VERSION);
    stw_be_p(event + 2, flags);
    stl_be_p(event + 4, event_type);
    stl_be_p(event + 8, status);
    stl_be_p(event + 20, data_len);
    memcpy(event + 24, address, ETH_ALEN);
    memcpy(event + 30, "wlan0", sizeof("wlan0"));
    if (data_len) {
        memcpy(event + CYW43455_EVENT_MSG_LEN, data, data_len);
    }

    s->f2_rx_len = stored_len;
    s->f2_rx_pos = 0;
    s->sdio_intstatus |= CYW43455_I_HMB_FRAME_IND;
    cyw43455_sdio_update_irq(s);
}

static void cyw43455_sdio_emit_event(CYW43455SDIOState *s,
                                     uint32_t event_type, uint32_t status,
                                     uint16_t flags, const uint8_t *data,
                                     uint32_t data_len)
{
    cyw43455_sdio_emit_event_addr(s, event_type, status, flags, data, data_len,
                                 cyw43455_sdio_peer_mac);
}

static bool cyw43455_sdio_fws_event_enabled(CYW43455SDIOState *s,
                                            unsigned int event)
{
    return s->fws_event_mask[event / 8] & BIT(event % 8);
}

static bool cyw43455_sdio_fws_events_enabled(CYW43455SDIOState *s)
{
    return cyw43455_sdio_fws_event_enabled(s,
               CYW43455_EVENT_FIFO_CREDIT_MAP) &&
           cyw43455_sdio_fws_event_enabled(s,
               CYW43455_EVENT_BCMC_CREDIT_SUPPORT);
}

static bool cyw43455_sdio_fws_credits_pending(CYW43455SDIOState *s)
{
    unsigned int i;

    for (i = 0; i < CYW43455_FWS_FIFO_COUNT; i++) {
        if (s->fws_credit_pending[i]) {
            return true;
        }
    }
    return false;
}

static bool cyw43455_sdio_fws_block_migration(CYW43455SDIOState *s)
{
    Error *local_err = NULL;

    if (s->fws_migration_blocker) {
        return true;
    }
    error_setg(&s->fws_migration_blocker,
               "Private CYW43455 FWS state is not migratable");
    if (migrate_add_blocker(&s->fws_migration_blocker, &local_err) < 0) {
        error_free(local_err);
        return false;
    }
    return true;
}

static void cyw43455_sdio_fws_schedule(CYW43455SDIOState *s)
{
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    int64_t due = INT64_MAX;

    timer_del(s->fws_timer);
    if (!s->fws_negotiated) {
        return;
    }
    if (!s->fws_map_sent && cyw43455_sdio_fws_events_enabled(s)) {
        due = s->fws_map_due_ms;
    }
    if (s->fws_status_count) {
        CYW43455FWSStatus *status = &s->fws_status[s->fws_status_head];
        unsigned int fifo = extract32(status->tag, 24, 3);

        if (s->fws_credit_pending[fifo] < CYW43455_FWS_CREDIT_MAX) {
            due = MIN(due, status->due_ms);
        }
    }
    if (cyw43455_sdio_fws_credits_pending(s)) {
        due = MIN(due, s->fws_credit_due_ms);
    }
    /* A due notification blocked by RX ownership waits for advance_rx. */
    if (due != INT64_MAX && due > now) {
        timer_mod(s->fws_timer, due);
    }
}

static void cyw43455_sdio_fws_timeout(void *opaque)
{
    CYW43455SDIOState *s = opaque;

    cyw43455_sdio_advance_rx(s);
    cyw43455_sdio_fws_schedule(s);
}

static void cyw43455_sdio_fws_signal_block(CYW43455SDIOState *s,
                                          const uint8_t *tlvs,
                                          unsigned int length)
{
    uint8_t *frame = s->f2_rx_frame;
    uint8_t *bcdc = frame + CYW43455_SDPCM_HEADER_LEN;
    uint8_t *signal = bcdc + CYW43455_BCDC_DATA_HEADER_LEN;
    unsigned int signal_len = QEMU_ALIGN_UP(length, 4);
    unsigned int frame_len = CYW43455_SDPCM_HEADER_LEN +
                             CYW43455_BCDC_DATA_HEADER_LEN + signal_len;

    assert(length && length <= 6 * CYW43455_FWS_STATUS_BATCH_MAX);
    assert(!s->f2_rx_len);
    assert(!(s->sdio_intstatus & CYW43455_I_HMB_FRAME_IND));
    memset(frame, 0, frame_len);
    stw_le_p(frame, frame_len);
    stw_le_p(frame + 2, (uint16_t)~frame_len);
    frame[4] = s->f2_rx_seq++;
    frame[5] = CYW43455_SDPCM_DATA_CHANNEL;
    frame[7] = CYW43455_SDPCM_HEADER_LEN;
    frame[9] = s->tx_credit_limit;
    s->tx_credit_update_pending = false;
    bcdc[0] = CYW43455_BCDC_PROTO_VER << 4;
    bcdc[3] = signal_len / 4;
    memset(signal, CYW43455_FWS_FILLER, signal_len);
    memcpy(signal, tlvs, length);
    s->f2_rx_len = frame_len;
    s->f2_rx_pos = 0;
    s->sdio_intstatus |= CYW43455_I_HMB_FRAME_IND;
    cyw43455_sdio_update_irq(s);
}

static void cyw43455_sdio_fws_signal(CYW43455SDIOState *s, uint8_t type,
                                    const uint8_t *data, uint8_t length)
{
    uint8_t tlv[2 + CYW43455_FWS_FIFO_COUNT];

    assert(length <= CYW43455_FWS_FIFO_COUNT);
    tlv[0] = type;
    tlv[1] = length;
    memcpy(tlv + 2, data, length);
    cyw43455_sdio_fws_signal_block(s, tlv, length + 2);
}

static void cyw43455_sdio_fws_status_read(CYW43455SDIOState *s)
{
    unsigned int count = s->fws_retiring_count;

    if (!count) {
        return;
    }
    /* The caller invokes this only after the ENTIRE FIFO frame was read. */
    if (!cyw43455_sdio_fws_credits_pending(s)) {
        s->fws_credit_due_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                               s->fws_credit_delay_ms;
    }
    for (unsigned int i = 0; i < count; i++) {
        uint32_t tag = s->fws_retiring_tags[i];
        unsigned int fifo = extract32(tag, 24, 3);
        unsigned int hslot = extract32(tag, 8, 16);
        uint8_t *available = fifo == 4 ? &s->fws_bcmc_available :
                                       &s->fws_ac_available;

        /* All selected tags and per-FIFO credit slots were reserved at emit. */
        assert(s->fws_hanger_active[hslot]);
        assert(*available < s->fws_credit_budget * (fifo == 4 ? 1 : 4));
        assert(s->fws_credit_pending[fifo] < CYW43455_FWS_CREDIT_MAX);
        s->fws_hanger_active[hslot] = false;
        (*available)++;
        s->fws_credit_pending[fifo]++;
        s->fws_retiring_tags[i] = 0;
        s->fws_retiring_count--;
        trace_cyw43455_sdio_fws_status(tag, s->fws_status_count +
                                          s->fws_retiring_count);
    }
    cyw43455_sdio_fws_schedule(s);
}

/* Called only after current RX, committed glom, and control reply ownership. */
static bool cyw43455_sdio_fws_emit(CYW43455SDIOState *s)
{
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    unsigned int i;

    if (!s->fws_negotiated) {
        return false;
    }
    if (!s->fws_map_sent) {
        uint8_t credits[CYW43455_FWS_FIFO_COUNT];

        if (!cyw43455_sdio_fws_events_enabled(s) ||
            now < s->fws_map_due_ms) {
            return false;
        }
        /*
         * This bounded slice requires both subscriptions. Linux activates
         * flow control on the map, but bypasses BCMC credits until event 127.
         * Publish support first, before the map can schedule tagged traffic.
         * This is our explicit HLE contract, not a measured RF ordering.
         */
        if (!s->fws_bcmc_sent) {
            s->fws_bcmc_sent = true;
            cyw43455_sdio_emit_event(s, CYW43455_EVENT_BCMC_CREDIT_SUPPORT,
                                     0, 0, NULL, 0);
            return true;
        }
        for (i = 0; i < CYW43455_FWS_FIFO_COUNT; i++) {
            credits[i] = i < 5 ? s->fws_credit_budget : 0;
        }
        s->fws_ac_available = 4 * s->fws_credit_budget;
        s->fws_bcmc_available = s->fws_credit_budget;
        s->fws_map_sent = true;
        cyw43455_sdio_emit_event(s, CYW43455_EVENT_FIFO_CREDIT_MAP, 0, 0,
                                 credits, CYW43455_FWS_FIFO_COUNT);
        trace_cyw43455_sdio_fws_map(s->fws_credit_budget);
        cyw43455_sdio_fws_schedule(s);
        return true;
    }
    if (s->fws_status_count) {
        uint8_t tlvs[6 * CYW43455_FWS_STATUS_BATCH_MAX];
        uint8_t reserved[CYW43455_FWS_FIFO_COUNT] = { };

        assert(!s->fws_retiring_count);
        /* No extra wait: take only the oldest already-due prefix. */
        while (s->fws_status_count &&
               s->fws_retiring_count < s->fws_status_batch_max) {
            CYW43455FWSStatus *status = &s->fws_status[s->fws_status_head];
            unsigned int fifo = extract32(status->tag, 24, 3);
            unsigned int offset = 6 * s->fws_retiring_count;

            /* Reserve ALL eventual returns, not merely the first TLV's. */
            if (now < status->due_ms ||
                s->fws_credit_pending[fifo] + reserved[fifo] >=
                CYW43455_FWS_CREDIT_MAX) {
                break;
            }
            tlvs[offset] = CYW43455_FWS_TXSTATUS;
            tlvs[offset + 1] = 4;
            stl_le_p(tlvs + offset + 2, status->tag);
            s->fws_retiring_tags[s->fws_retiring_count++] = status->tag;
            reserved[fifo]++;
            s->fws_status_head = (s->fws_status_head + 1) %
                                 CYW43455_FWS_STATUS_MAX;
            s->fws_status_count--;
        }
        if (s->fws_retiring_count) {
            cyw43455_sdio_fws_signal_block(s, tlvs,
                                           6 * s->fws_retiring_count);
            cyw43455_sdio_fws_schedule(s);
            return true;
        }
    }
    if (cyw43455_sdio_fws_credits_pending(s) &&
        now >= s->fws_credit_due_ms) {
        cyw43455_sdio_fws_signal(s, CYW43455_FWS_FIFO_CREDITBACK,
                                 s->fws_credit_pending,
                                 CYW43455_FWS_FIFO_COUNT);
        memset(s->fws_credit_pending, 0, sizeof(s->fws_credit_pending));
        s->fws_credit_due_ms = -1;
        trace_cyw43455_sdio_fws_creditback();
        cyw43455_sdio_fws_schedule(s);
        return true;
    }
    if (s->fws_if_pending) {
        uint8_t ifidx = 0;
        uint8_t transition = s->fws_if_pending;
        bool reopen = s->fws_if_reopen_pending;

        assert(!reopen || transition == CYW43455_FWS_INTERFACE_CLOSE);
        s->fws_if_open = transition == CYW43455_FWS_INTERFACE_OPEN;
        cyw43455_sdio_fws_signal(s, transition, &ifidx, 1);
        trace_cyw43455_sdio_fws_interface(s->fws_if_open);
        s->fws_if_pending = reopen ? CYW43455_FWS_INTERFACE_OPEN : 0;
        s->fws_if_reopen_pending = false;
        return true;
    }
    return false;
}

static void cyw43455_sdio_emit_ap_event(CYW43455SDIOState *s)
{
    uint8_t ies[CYW43455_AP_SSID_MAX + 7];
    const uint8_t *address = s->current_mac;
    const uint8_t *data = NULL;
    uint32_t length = 0;
    uint32_t type = CYW43455_EVENT_LINK;
    uint16_t flags = 0;
    uint8_t bit;

    if (s->ap_events & CYW43455_AP_EVENT_UP) {
        bit = CYW43455_AP_EVENT_UP;
        flags = CYW43455_EVENT_MSG_LINK;
    } else if (s->ap_events & CYW43455_AP_EVENT_DISASSOC) {
        bit = CYW43455_AP_EVENT_DISASSOC;
        type = CYW43455_EVENT_DISASSOC_IND;
        address = cyw43455_sdio_peer_mac;
    } else if (s->ap_events & CYW43455_AP_EVENT_ASSOC) {
        bit = CYW43455_AP_EVENT_ASSOC;
        type = CYW43455_EVENT_ASSOC_IND;
        address = cyw43455_sdio_peer_mac;
        data = ies;
        length = cyw43455_sdio_ap_ies(s, ies, false);
        s->ap_peer_associated = true;
        s->association_time_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    } else {
        assert(s->ap_events & CYW43455_AP_EVENT_DOWN);
        bit = CYW43455_AP_EVENT_DOWN;
    }
    s->ap_events &= ~bit;
    cyw43455_sdio_emit_event_addr(s, type, CYW43455_EVENT_STATUS_SUCCESS,
                                 flags, data, length, address);
    trace_cyw43455_sdio_ap_event(type, flags, length,
                                 s->ap_peer_associated, s->ap_events);
}

static void cyw43455_sdio_emit_link_event(CYW43455SDIOState *s)
{
    uint8_t sequence = s->f2_rx_seq;
    bool ssid = s->link_event_up &&
                (!(s->explicit_association || s->explicit_role) ||
                 s->link_ssid_pending);

    assert(s->link_event_pending);
    cyw43455_sdio_emit_event(s,
                             ssid ? CYW43455_EVENT_SET_SSID :
                                    CYW43455_EVENT_LINK,
                             CYW43455_EVENT_STATUS_SUCCESS,
                             s->link_event_up ? CYW43455_EVENT_MSG_LINK : 0,
                             NULL, 0);
    s->link_ssid_pending = (s->explicit_association || s->explicit_role) &&
                           s->link_event_up && !ssid;
    s->link_event_pending = s->link_ssid_pending;
    s->link_event_sent = s->link_event_up;
    if (s->link_event_up && !s->firmware_connected) {
        s->association_time_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    } else if (!s->link_event_up) {
        s->association_time_ms = -1;
    }
    s->firmware_connected = s->link_event_up;
    s->backend_link_events_armed |= s->link_event_up;
    trace_cyw43455_sdio_link_event(s->link_event_up, sequence,
                                   s->f2_rx_frame[9]);
}

static void cyw43455_sdio_emit_escan_event(CYW43455SDIOState *s)
{
    uint8_t result[CYW43455_ESCAN_RESULT_LEN] = { };
    bool partial = s->escan_event_phase == CYW43455_ESCAN_EVENT_PARTIAL;
    uint32_t data_len = 0;
    uint32_t status = CYW43455_EVENT_STATUS_SUCCESS;

    assert(s->escan_event_phase != CYW43455_ESCAN_EVENT_NONE);
    if (partial) {
        data_len = sizeof(result);
        status = CYW43455_EVENT_STATUS_PARTIAL;
        stl_le_p(result, sizeof(result));
        stl_le_p(result + 4, CYW43455_ESCAN_RESULT_VERSION);
        stw_le_p(result + 8, s->escan_sync_id);
        stw_le_p(result + 10, 1);
        cyw43455_sdio_fill_bss_info(
            s, result + CYW43455_ESCAN_RESULT_FIXED_LEN,
            CYW43455_ESCAN_BSS_INFO_LEN, true, false);
    }

    cyw43455_sdio_emit_event(s, CYW43455_EVENT_ESCAN_RESULT, status, 0,
                             result, data_len);
    trace_cyw43455_sdio_escan_event(s->escan_sync_id, status,
                                    partial, s->f2_rx_len);
    s->escan_event_phase = partial ? CYW43455_ESCAN_EVENT_COMPLETE :
                                     CYW43455_ESCAN_EVENT_NONE;
}

static void cyw43455_sdio_advance_rx(CYW43455SDIOState *s)
{
    if (s->f2_rx_len) {
        return;
    }

    /* A descriptor commits the following FIFO read to its superframe. */
    if (s->rx_glom_super_pending) {
        s->f2_rx_len = QEMU_ALIGN_UP(s->rx_glom_len, 512);
        memcpy(s->f2_rx_frame, s->rx_glom_frame, s->f2_rx_len);
        s->f2_rx_pos = 0;
        trace_cyw43455_sdio_rx_glom(s->rx_glom_count, s->rx_glom_len, false);
        s->rx_glom_super_pending = false;
        s->rx_glom_count = 0;
        s->rx_glom_len = 0;
        s->sdio_intstatus |= CYW43455_I_HMB_FRAME_IND;
        cyw43455_sdio_update_irq(s);
        return;
    }

    if (s->sdio_intstatus & CYW43455_I_HMB_FRAME_IND) {
        return;
    }

    if (s->f2_rx_pending_len) {
        memcpy(s->f2_rx_frame, s->f2_rx_pending_frame,
               s->f2_rx_pending_len);
        s->f2_rx_len = s->f2_rx_pending_len;
        s->f2_rx_pos = 0;
        s->f2_rx_pending_len = 0;
        s->sdio_intstatus |= CYW43455_I_HMB_FRAME_IND;
        trace_cyw43455_sdio_rx_promoted(s->f2_rx_len);
        cyw43455_sdio_update_irq(s);
        return;
    }

    if (cyw43455_sdio_fws_emit(s)) {
        return;
    }

    if (s->ap_events) {
        cyw43455_sdio_emit_ap_event(s);
        return;
    }

    if (s->link_event_pending) {
        cyw43455_sdio_emit_link_event(s);
        return;
    }

    if (s->escan_event_phase) {
        cyw43455_sdio_emit_escan_event(s);
        return;
    }

    if (s->tx_credit_update_pending) {
        cyw43455_sdio_emit_tx_credit_frame(s);
        return;
    }

    if (s->rx_glom_ready) {
        cyw43455_sdio_emit_rx_glom(s);
        return;
    }

    cyw43455_sdio_flush_queued_packets(s);
}

static void cyw43455_sdio_clear_intstatus(CYW43455SDIOState *s,
                                           uint32_t value)
{
    bool frame_cleared = value & s->sdio_intstatus &
                         CYW43455_I_HMB_FRAME_IND;

    if (frame_cleared) {
        trace_cyw43455_sdio_rx_ack(s->f2_rx_len, s->f2_rx_pos);
    }
    s->sdio_intstatus &= ~value;
    cyw43455_sdio_update_irq(s);
    if (frame_cleared) {
        cyw43455_sdio_advance_rx(s);
    }
}

static bool cyw43455_sdio_iovar_name(const uint8_t *payload,
                                     uint32_t payload_len,
                                     char *name, size_t name_size)
{
    const uint8_t *terminator;
    size_t name_len;

    terminator = memchr(payload, '\0', payload_len);
    if (!terminator) {
        return false;
    }

    name_len = terminator - payload;
    if (!name_len || name_len >= name_size) {
        return false;
    }

    memcpy(name, payload, name_len);
    name[name_len] = '\0';
    return true;
}

static void cyw43455_sdio_store_u32(uint8_t *output, uint32_t output_len,
                                    uint32_t offset, uint32_t value)
{
    if (offset <= output_len && output_len - offset >= sizeof(value)) {
        stl_le_p(output + offset, value);
    }
}

static void cyw43455_sdio_store_string(uint8_t *output, uint32_t output_len,
                                       const char *value)
{
    size_t length = MIN(strlen(value) + 1, output_len);

    memcpy(output, value, length);
}

static int32_t cyw43455_sdio_wlc_version(CYW43455SDIOState *s, uint8_t *output,
                                          uint32_t output_len)
{
    if (output_len < CYW43455_WLC_VERSION_LEN) {
        return CYW43455_BCME_BADLEN;
    }

    /* Synthetic API version, selected from each board's captured FW family. */
    stw_le_p(output, CYW43455_WLC_VERSION_STRUCT_VERSION);
    stw_le_p(output + 2, CYW43455_WLC_VERSION_LEN);
    stw_le_p(output + 4, CYW43455_EPI_VERSION_MAJOR);
    stw_le_p(output + 6, s->hardware_profile == CYW43455_SDIO_PROFILE_PI5 ?
              45 : CYW43455_EPI_VERSION_MINOR);
    stw_le_p(output + 8, s->hardware_profile == CYW43455_SDIO_PROFILE_PI5 ?
              265 : CYW43455_EPI_VERSION_RC);
    stw_le_p(output + 10, s->hardware_profile == CYW43455_SDIO_PROFILE_PI5 ?
              0 : CYW43455_EPI_VERSION_INCREMENT);
    stw_le_p(output + 12, CYW43455_WLC_VERSION_MAJOR);
    stw_le_p(output + 14, s->hardware_profile == CYW43455_SDIO_PROFILE_PI5 ?
              45 : CYW43455_WLC_VERSION_MINOR);
    return 0;
}

static int32_t cyw43455_sdio_get_bw_cap(const uint8_t *data,
                                         uint32_t data_len,
                                         uint8_t *output,
                                         uint32_t output_len)
{
    uint32_t band;
    uint32_t capability;
    int32_t status = 0;

    if (data_len != sizeof(band) || output_len < sizeof(capability)) {
        return CYW43455_BCME_BADLEN;
    }
    band = ldl_le_p(data);
    switch (band) {
    case CYW43455_WLC_BAND_2G:
        capability = CYW43455_BW_CAP_2G;
        break;
    case CYW43455_WLC_BAND_5G:
        capability = CYW43455_BW_CAP_20MHZ;
        break;
    default:
        capability = 0;
        status = CYW43455_BCME_BADARG;
        break;
    }
    if (status == 0) {
        stl_le_p(output, capability);
    }
    trace_cyw43455_sdio_bw_cap(false, band, capability, status);
    return status;
}

static int32_t cyw43455_sdio_set_bw_cap(const uint8_t *data,
                                         uint32_t data_len)
{
    uint32_t band;
    uint32_t capability;
    int32_t status;

    if (data_len != 2 * sizeof(uint32_t)) {
        return CYW43455_BCME_BADLEN;
    }
    band = ldl_le_p(data);
    capability = ldl_le_p(data + sizeof(uint32_t));
    if ((band == CYW43455_WLC_BAND_2G &&
         capability == CYW43455_BW_CAP_2G) ||
        (band == CYW43455_WLC_BAND_5G &&
         capability == CYW43455_BW_CAP_20MHZ)) {
        status = 0;
    } else {
        status = CYW43455_BCME_BADARG;
    }
    trace_cyw43455_sdio_bw_cap(true, band, capability, status);
    return status;
}

static int32_t cyw43455_sdio_get_chanspecs(const uint8_t *data,
                                            uint32_t data_len,
                                            uint8_t *output,
                                            uint32_t output_len)
{
    uint32_t count;
    uint32_t filter;
    unsigned int index = 0;
    unsigned int channel;
    bool five_ghz;

    if (data_len < sizeof(uint16_t)) {
        trace_cyw43455_sdio_chanspecs(0, 0, CYW43455_BCME_BADLEN);
        return CYW43455_BCME_BADLEN;
    }
    filter = lduw_le_p(data);
    five_ghz = (filter & CYW43455_CHANSPEC_BAND_MASK) ==
               CYW43455_CHANSPEC_BAND_5G;
    if ((filter & (CYW43455_CHANSPEC_CHANNEL_MASK |
                   CYW43455_CHANSPEC_SIDEBAND_MASK)) != 0 ||
        ((filter & CYW43455_CHANSPEC_BAND_MASK) && !five_ghz)) {
        trace_cyw43455_sdio_chanspecs(filter, 0, CYW43455_BCME_BADARG);
        return CYW43455_BCME_BADARG;
    }
    switch (filter & CYW43455_CHANSPEC_BANDWIDTH_MASK) {
    case 0:
        count = five_ghz ? 1 : CYW43455_CHANSPEC_ALL_COUNT;
        break;
    case CYW43455_CHANSPEC_BANDWIDTH_20:
        count = five_ghz ? 1 : CYW43455_CHANSPEC_20_COUNT;
        break;
    case CYW43455_CHANSPEC_BANDWIDTH_40:
        if (five_ghz) {
            return CYW43455_BCME_BADARG;
        }
        count = CYW43455_CHANSPEC_40_COUNT;
        break;
    default:
        trace_cyw43455_sdio_chanspecs(filter, 0, CYW43455_BCME_BADARG);
        return CYW43455_BCME_BADARG;
    }
    if (output_len < CYW43455_CHANSPEC_LIST_LEN(count)) {
        trace_cyw43455_sdio_chanspecs(filter, count,
                                      CYW43455_BCME_BADLEN);
        return CYW43455_BCME_BADLEN;
    }

    stl_le_p(output, count);
    if (five_ghz) {
        stl_le_p(output + sizeof(uint32_t),
                 CYW43455_CHANSPEC_5G_CH36_20MHZ);
        trace_cyw43455_sdio_chanspecs(filter, count, 0);
        return 0;
    }
    if (filter == 0 || filter == CYW43455_CHANSPEC_BANDWIDTH_20) {
        for (channel = CYW43455_CHANNEL_2G_MIN;
             channel <= CYW43455_CHANNEL_2G_MAX; channel++) {
            stl_le_p(output + sizeof(uint32_t) * ++index,
                     CYW43455_CHANSPEC_BANDWIDTH_20 | channel);
        }
    }
    if (filter == 0 || filter == CYW43455_CHANSPEC_BANDWIDTH_40) {
        for (channel = CYW43455_CHANNEL_40_CENTER_MIN;
             channel <= CYW43455_CHANNEL_40_CENTER_MAX; channel++) {
            stl_le_p(output + sizeof(uint32_t) * ++index,
                     CYW43455_CHANSPEC_BANDWIDTH_40 |
                     CYW43455_CHANSPEC_SIDEBAND_UPPER | channel);
        }
        for (channel = CYW43455_CHANNEL_40_CENTER_MIN;
             channel <= CYW43455_CHANNEL_40_CENTER_MAX; channel++) {
            stl_le_p(output + sizeof(uint32_t) * ++index,
                     CYW43455_CHANSPEC_BANDWIDTH_40 |
                     CYW43455_CHANSPEC_SIDEBAND_LOWER | channel);
        }
    }
    if (filter == 0) {
        stl_le_p(output + sizeof(uint32_t) * ++index,
                 CYW43455_CHANSPEC_5G_CH36_20MHZ);
    }
    assert(index == count);
    trace_cyw43455_sdio_chanspecs(filter, count, 0);
    return 0;
}

static int32_t cyw43455_sdio_dump_obss(CYW43455SDIOState *s,
                                        const uint8_t *data,
                                        uint32_t data_len,
                                        uint8_t *output,
                                        uint32_t output_len)
{
    char results[CYW43455_SURVEY_RESULTS_MAX];
    uint32_t query;
    uint32_t duration;
    uint32_t obss;
    uint32_t ibss;
    uint32_t no_ctg;
    uint32_t no_pckt;
    uint32_t tx;
    uint32_t idle;
    int length;

    /* The four-byte integer query is brcmfmac's feature probe. */
    if (data_len == sizeof(uint32_t)) {
        cyw43455_sdio_store_u32(output, output_len, 0, 0);
        return 0;
    }
    if (data_len < 2 * sizeof(uint32_t) ||
        output_len < 2 * sizeof(uint32_t)) {
        return CYW43455_BCME_BADLEN;
    }

    query = ldl_le_p(data);
    duration = ldl_le_p(data + sizeof(uint32_t));
    if (query == 0) {
        if (!duration || duration > 60000) {
            return CYW43455_BCME_BADARG;
        }
        if (s->survey_active) {
            return CYW43455_BCME_BUSY;
        }
        s->survey_duration_ms = duration;
        s->survey_active = true;
        stl_le_p(output, duration);
        stl_le_p(output + sizeof(uint32_t), 0);
        trace_cyw43455_sdio_survey(query, s->chanspec, duration,
                                   s->survey_active);
        return 0;
    }
    if (query != 1 || !s->survey_active ||
        duration != s->survey_duration_ms) {
        return CYW43455_BCME_BADARG;
    }

    obss = duration * 3 / 20;
    ibss = duration * 3 / 80;
    no_ctg = duration / 20;
    no_pckt = duration / 40;
    tx = duration / 16;
    idle = duration * 5 / 8;
    length = g_snprintf(
        results, sizeof(results),
        "OBSS 0 0 0 0 0 0 0 0 0 0 0 0 0 0 %u\n"
        "IBSS 0 0 0 0 0 0 0 0 0 0 0 0 0 0 %u\n"
        "TXDur 0 0 0 0 0 0 0 0 0 0 0 0 0 %u\n"
        "Category 0 0 0 0 0 0 0 0 0 0 0 0 %u\n"
        "Packet 0 0 0 0 0 0 0 0 0 0 0 0 0 0 %u\n"
        "Opp(time): 0 0 0 0 0 0 0 0 0 0 0 %u\n",
        obss, ibss, tx, no_ctg, no_pckt, idle);
    if (length < 0 || length >= sizeof(results) ||
        (uint32_t)length + 2 * sizeof(uint32_t) + 1 > output_len) {
        return CYW43455_BCME_BADLEN;
    }

    stl_le_p(output, duration);
    stl_le_p(output + sizeof(uint32_t), 1);
    memcpy(output + 2 * sizeof(uint32_t), results, length + 1);
    s->survey_active = false;
    trace_cyw43455_sdio_survey(query, s->chanspec, duration,
                               s->survey_active);
    return 0;
}

static int32_t cyw43455_sdio_station_info(CYW43455SDIOState *s,
                                          const uint8_t *data,
                                          uint32_t data_len,
                                          uint8_t *output,
                                          uint32_t output_len)
{
    uint64_t tx_unicast_bytes = s->tx_bytes - s->tx_multicast_bytes;
    uint64_t rx_unicast_bytes = s->rx_bytes - s->rx_multicast_bytes;
    uint32_t tx_unicast = s->tx_packets - s->tx_multicast_packets;
    uint32_t rx_unicast = s->rx_packets - s->rx_multicast_packets;
    uint32_t connected_seconds = 0;
    int64_t now_ms;

    if (s->ap_mode ? !(s->ap_started && s->ap_peer_associated) :
                     !s->firmware_connected) {
        return CYW43455_BCME_NOTASSOCIATED;
    }
    if (data_len < ETH_ALEN ||
        memcmp(data, cyw43455_sdio_peer_mac, ETH_ALEN)) {
        return CYW43455_BCME_BADARG;
    }
    if (output_len < CYW43455_STA_INFO_LEN) {
        return CYW43455_BCME_BADLEN;
    }

    now_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    if (s->association_time_ms >= 0 && now_ms > s->association_time_ms) {
        connected_seconds = MIN((uint64_t)(now_ms -
                                            s->association_time_ms) / 1000,
                                (uint64_t)UINT32_MAX);
    }

    stw_le_p(output, CYW43455_STA_INFO_VERSION);
    stw_le_p(output + 2, CYW43455_STA_INFO_LEN);
    stw_le_p(output + 4, BIT(10)); /* short-slot-time capability */
    stl_le_p(output + CYW43455_STA_INFO_FLAGS_OFFSET,
             s->ap_mode ? CYW43455_STA_FLAGS & ~CYW43455_STA_WME :
                          CYW43455_STA_FLAGS);
    stl_le_p(output + CYW43455_STA_INFO_IDLE_OFFSET, 0);
    memcpy(output + CYW43455_STA_INFO_EA_OFFSET,
           cyw43455_sdio_peer_mac, ETH_ALEN);
    stl_le_p(output + CYW43455_STA_INFO_COUNT_OFFSET, s->ap_mode ? 3 : 1);
    if (s->ap_mode) {
        memcpy(output + CYW43455_STA_INFO_RATES_OFFSET, "\x8c\x98\xb0", 3);
    } else {
        output[CYW43455_STA_INFO_RATES_OFFSET] = 144; /* Historical STA HLE. */
    }
    stl_le_p(output + CYW43455_STA_INFO_IN_OFFSET, connected_seconds);
    stl_le_p(output + CYW43455_STA_INFO_TX_PKTS_OFFSET, tx_unicast);
    stl_le_p(output + CYW43455_STA_INFO_TX_FAIL_OFFSET, 0);
    stl_le_p(output + CYW43455_STA_INFO_RX_UCAST_OFFSET, rx_unicast);
    stl_le_p(output + CYW43455_STA_INFO_RX_MCAST_OFFSET,
             s->rx_multicast_packets);
    stl_le_p(output + CYW43455_STA_INFO_TX_RATE_OFFSET,
             s->ap_mode ? 24000 : CYW43455_LINK_RATE_KBPS);
    stl_le_p(output + CYW43455_STA_INFO_RX_RATE_OFFSET,
             s->ap_mode ? 24000 : CYW43455_LINK_RATE_KBPS);
    stl_le_p(output + CYW43455_STA_INFO_TX_TOTAL_OFFSET, s->tx_packets);
    stl_le_p(output + CYW43455_STA_INFO_RX_TOTAL_OFFSET, s->rx_packets);
    stl_le_p(output + CYW43455_STA_INFO_TX_MCAST_OFFSET,
             s->tx_multicast_packets);
    stq_le_p(output + CYW43455_STA_INFO_TX_BYTES_OFFSET, s->tx_bytes);
    stq_le_p(output + CYW43455_STA_INFO_RX_BYTES_OFFSET, s->rx_bytes);
    stq_le_p(output + CYW43455_STA_INFO_TX_UCAST_BYTES_OFFSET,
             tx_unicast_bytes);
    stq_le_p(output + CYW43455_STA_INFO_TX_MCAST_BYTES_OFFSET,
             s->tx_multicast_bytes);
    stq_le_p(output + CYW43455_STA_INFO_RX_UCAST_BYTES_OFFSET,
             rx_unicast_bytes);
    stq_le_p(output + CYW43455_STA_INFO_RX_MCAST_BYTES_OFFSET,
             s->rx_multicast_bytes);
    output[CYW43455_STA_INFO_RSSI_OFFSET] = CYW43455_SIGNAL_DBM;
    output[CYW43455_STA_INFO_NF_OFFSET] = CYW43455_NOISE_DBM;
    stw_le_p(output + CYW43455_STA_INFO_AID_OFFSET, 1);
    output[CYW43455_STA_INFO_RX_LAST_RSSI_OFFSET] = CYW43455_SIGNAL_DBM;

    trace_cyw43455_sdio_station(s->tx_packets, s->rx_packets,
                                s->tx_bytes, s->rx_bytes,
                                CYW43455_SIGNAL_DBM);
    return 0;
}

static int32_t cyw43455_sdio_bss_info(CYW43455SDIOState *s,
                                      const uint8_t *input,
                                      uint32_t input_len,
                                      uint8_t *output,
                                      uint32_t output_len)
{
    uint8_t *bss;
    uint32_t requested_len;
    bool include_ies = s->ap_mode || s->explicit_association || s->explicit_role;
    uint32_t bss_len = s->ap_mode ? CYW43455_BSS_INFO_LEN +
                                   cyw43455_sdio_ap_ie_len(s) :
                      include_ies ? CYW43455_ESCAN_BSS_INFO_LEN :
                                    CYW43455_BSS_INFO_LEN;

    if (s->ap_mode ? !s->ap_started : !s->firmware_connected) {
        return CYW43455_BCME_NOTASSOCIATED;
    }
    if (input_len < CYW43455_BSS_INFO_PREFIX_LEN) {
        return CYW43455_BCME_BADLEN;
    }

    requested_len = ldl_le_p(input);
    if (requested_len < CYW43455_BSS_INFO_PREFIX_LEN +
                        bss_len ||
        requested_len > output_len) {
        return CYW43455_BCME_BADLEN;
    }

    bss = output + CYW43455_BSS_INFO_PREFIX_LEN;
    stl_le_p(output, requested_len);
    cyw43455_sdio_fill_bss_info(s, bss, bss_len, include_ies, s->ap_mode);

    trace_cyw43455_sdio_bss(CYW43455_BSS_BEACON_PERIOD,
                            CYW43455_BSS_DTIM_PERIOD,
                            CYW43455_SIGNAL_DBM,
                            CYW43455_NOISE_DBM);
    return 0;
}

static bool cyw43455_sdio_scan_ssid_match(const uint8_t *ssid)
{
    uint32_t length = ldl_le_p(ssid + CYW43455_SCAN_SSID_LEN_OFFSET);

    return length == 0 ||
           (length == strlen(CYW43455_BSS_SSID) &&
            !memcmp(ssid + CYW43455_SCAN_SSID_OFFSET,
                    CYW43455_BSS_SSID, length));
}

static int32_t cyw43455_sdio_start_escan(CYW43455SDIOState *s,
                                         const uint8_t *data,
                                         uint32_t data_len)
{
    static const uint8_t broadcast[ETH_ALEN] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    };
    CYW43455ScanLayout layout;
    const uint8_t *params = data + CYW43455_ESCAN_HEADER_LEN;
    const uint8_t *channels;
    const uint8_t *ssids;
    uint32_t version;
    uint32_t counts;
    uint32_t channel_count;
    uint32_t ssid_count;
    uint8_t ap_bandwidth_mhz;
    uint8_t ap_control_channel;
    size_t escan_fixed_len;
    size_t scan_end;
    size_t channel_bytes;
    size_t ssid_offset;
    bool channel_match;
    bool ssid_match = false;
    bool bssid_match;
    bool bss_type_match;
    unsigned int i;

    if (s->escan_event_phase) {
        return CYW43455_BCME_BUSY;
    }
    if (data_len < CYW43455_ESCAN_HEADER_LEN) {
        return CYW43455_BCME_BADLEN;
    }
    if (lduw_le_p(data + 4) != CYW43455_ESCAN_ACTION_START) {
        return CYW43455_BCME_BADARG;
    }

    version = ldl_le_p(data);
    switch (version) {
    case CYW43455_ESCAN_REQ_VERSION_V1:
        layout = (CYW43455ScanLayout) {
            .params_fixed_len = CYW43455_SCAN_PARAMS_V1_FIXED_LEN,
            .ssid_offset = 0,
            .bssid_offset = 36,
            .bss_type_offset = 42,
            .channel_num_offset = 60,
        };
        scan_end = data_len;
        break;
    case CYW43455_ESCAN_REQ_VERSION_V2:
        layout = (CYW43455ScanLayout) {
            .params_fixed_len = CYW43455_SCAN_PARAMS_V2_FIXED_LEN,
            .ssid_offset = 4,
            .bssid_offset = 40,
            .bss_type_offset = 46,
            .channel_num_offset = 68,
        };
        if (data_len < CYW43455_ESCAN_HEADER_LEN +
                       layout.params_fixed_len) {
            return CYW43455_BCME_BADLEN;
        }
        if (lduw_le_p(params + CYW43455_SCAN_PARAMS_V2_VERSION_OFFSET) !=
            CYW43455_ESCAN_REQ_VERSION_V2) {
            return CYW43455_BCME_BADARG;
        }
        scan_end = lduw_le_p(params +
                             CYW43455_SCAN_PARAMS_V2_LENGTH_OFFSET);
        if (scan_end < layout.params_fixed_len ||
            scan_end > data_len - CYW43455_ESCAN_HEADER_LEN) {
            return CYW43455_BCME_BADLEN;
        }
        scan_end += CYW43455_ESCAN_HEADER_LEN;
        break;
    default:
        return CYW43455_BCME_BADARG;
    }

    escan_fixed_len = CYW43455_ESCAN_HEADER_LEN +
                      layout.params_fixed_len;
    if (data_len < escan_fixed_len) {
        return CYW43455_BCME_BADLEN;
    }

    counts = ldl_le_p(params + layout.channel_num_offset);
    channel_count = extract32(counts, 0, 16);
    ssid_count = extract32(counts, 16, 16);
    if (channel_count >
        (scan_end - escan_fixed_len) / sizeof(uint16_t)) {
        return CYW43455_BCME_BADLEN;
    }
    channel_bytes = channel_count * sizeof(uint16_t);
    ssid_offset = QEMU_ALIGN_UP(escan_fixed_len + channel_bytes,
                                sizeof(uint32_t));
    if (ssid_offset > scan_end ||
        ssid_count > (scan_end - ssid_offset) / CYW43455_SCAN_SSID_LEN) {
        return CYW43455_BCME_BADLEN;
    }

    channels = data + escan_fixed_len;
    ssids = data + ssid_offset;
    assert(cyw43455_sdio_chanspec_info(s->ap_chanspec,
                                      &ap_control_channel,
                                      &ap_bandwidth_mhz));
    channel_match = channel_count == 0;
    for (i = 0; i < channel_count; i++) {
        uint32_t requested = lduw_le_p(channels + i * sizeof(uint16_t));
        uint8_t requested_bandwidth_mhz;
        uint8_t requested_control_channel;

        if (!cyw43455_sdio_chanspec_info(requested,
                                         &requested_control_channel,
                                         &requested_bandwidth_mhz)) {
            requested_control_channel = requested &
                                        CYW43455_CHANSPEC_CHANNEL_MASK;
        }
        if (requested_control_channel == ap_control_channel) {
            channel_match = true;
        }
    }

    if (ssid_count == 0) {
        const uint8_t *ssid = params + layout.ssid_offset;
        uint32_t length = ldl_le_p(ssid +
                                   CYW43455_SCAN_SSID_LEN_OFFSET);

        if (length > CYW43455_SCAN_SSID_MAX_LEN) {
            return CYW43455_BCME_BADARG;
        }
        ssid_match = cyw43455_sdio_scan_ssid_match(ssid);
    } else {
        for (i = 0; i < ssid_count; i++) {
            const uint8_t *ssid = ssids + i * CYW43455_SCAN_SSID_LEN;
            uint32_t length = ldl_le_p(
                ssid + CYW43455_SCAN_SSID_LEN_OFFSET);

            if (length > CYW43455_SCAN_SSID_MAX_LEN) {
                return CYW43455_BCME_BADARG;
            }
            ssid_match |= cyw43455_sdio_scan_ssid_match(ssid);
        }
    }

    bssid_match = !memcmp(params + layout.bssid_offset,
                           broadcast, ETH_ALEN) ||
                   !memcmp(params + layout.bssid_offset,
                           cyw43455_sdio_peer_mac, ETH_ALEN);
    if (params[layout.bss_type_offset] > 2) {
        return CYW43455_BCME_BADARG;
    }
    bss_type_match = params[layout.bss_type_offset] != 1;

    s->escan_sync_id = lduw_le_p(data + 6);
    s->escan_event_phase = channel_match && ssid_match && bssid_match &&
                           bss_type_match ? CYW43455_ESCAN_EVENT_PARTIAL :
                                            CYW43455_ESCAN_EVENT_COMPLETE;
    trace_cyw43455_sdio_escan_request(s->escan_sync_id, version,
                                      channel_count, ssid_count,
                                      s->escan_event_phase ==
                                      CYW43455_ESCAN_EVENT_PARTIAL);
    return 0;
}

static bool cyw43455_sdio_supported_set_iovar(const char *name)
{
    static const char *const supported[] = {
        "bus:txglomalign",
        "bus:rxglom",
        "clmload",
        "mpc",
        "join_pref",
        "event_msgs",
        "event_msgs_ext",
    };
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(supported); i++) {
        if (!strcmp(name, supported[i])) {
            return true;
        }
    }

    return false;
}

static void cyw43455_sdio_pfn_clear(CYW43455SDIOState *s)
{
    memset(s->pfn_channels, 0, sizeof(s->pfn_channels));
    memset(s->pfn_ssid_len, 0, sizeof(s->pfn_ssid_len));
    memset(s->pfn_ssids, 0, sizeof(s->pfn_ssids));
    memset(s->pfn_network_flags, 0, sizeof(s->pfn_network_flags));
    s->pfn_scan_freq = 0;
    s->pfn_channel_count = 0;
    s->pfn_network_count = 0;
    s->pfn_bucket_count = 0;
}

static int32_t cyw43455_sdio_set_pfn(CYW43455SDIOState *s,
                                      const char *name,
                                      const uint8_t *data,
                                      uint32_t data_len)
{
    uint32_t value;
    unsigned int i;

    if (!strcmp(name, "pfn")) {
        int32_t status = 0;

        if (data_len != sizeof(value)) {
            status = CYW43455_BCME_BADLEN;
        } else {
            value = ldl_le_p(data);
            if (value > 1 ||
                (value && (!s->pfn_scan_freq ||
                           !s->pfn_channel_count ||
                           !s->pfn_network_count ||
                           !s->pfn_bucket_count))) {
                status = CYW43455_BCME_BADARG;
            } else {
                s->pfn_enabled = value;
            }
        }
        trace_cyw43455_sdio_pno(true, s->pfn_enabled,
                                s->pfn_scan_freq, s->pfn_channel_count,
                                s->pfn_network_count, status);
        return status;
    }
    if (!strcmp(name, "pfnclear")) {
        if (data_len) {
            return CYW43455_BCME_BADLEN;
        }
        if (s->pfn_enabled) {
            return CYW43455_BCME_BUSY;
        }
        cyw43455_sdio_pfn_clear(s);
        return 0;
    }
    if (!strcmp(name, "pfn_set")) {
        uint32_t scan_freq;

        if (data_len != CYW43455_PNO_PARAM_LEN) {
            return CYW43455_BCME_BADLEN;
        }
        scan_freq = ldl_le_p(data + 4);
        if (ldl_le_p(data) != CYW43455_PNO_VERSION ||
            scan_freq < CYW43455_PNO_MIN_PERIOD ||
            scan_freq > CYW43455_PNO_MAX_PERIOD ||
            ldl_le_p(data + 8) || lduw_le_p(data + 12) != 0x48 ||
            lduw_le_p(data + 14) || data[16] || data[17] ||
            data[18] != 4 || data[19] != 3 || ldl_le_p(data + 20)) {
            return CYW43455_BCME_BADARG;
        }
        s->pfn_scan_freq = scan_freq;
        return 0;
    }
    if (!strcmp(name, "pfn_cfg")) {
        uint32_t count;

        if (data_len != CYW43455_PNO_CONFIG_LEN) {
            return CYW43455_BCME_BADLEN;
        }
        count = ldl_le_p(data + 4);
        if (ldl_le_p(data) || ldl_le_p(data + 136) || !count ||
            count > CYW43455_PNO_MAX_CHANNELS) {
            return CYW43455_BCME_BADARG;
        }
        for (i = 0; i < count; i++) {
            uint16_t channel = lduw_le_p(data + 8 + i * sizeof(channel));

            if (channel < CYW43455_CHANNEL_2G_MIN ||
                channel > CYW43455_CHANNEL_2G_MAX) {
                return CYW43455_BCME_BADARG;
            }
            s->pfn_channels[i] = channel;
        }
        memset(s->pfn_channels + count, 0,
               sizeof(s->pfn_channels) - count * sizeof(s->pfn_channels[0]));
        s->pfn_channel_count = count;
        return 0;
    }
    if (!strcmp(name, "pfn_gscan_cfg")) {
        uint8_t count;

        if (data_len < CYW43455_PNO_GSCAN_HEADER_LEN) {
            return CYW43455_BCME_BADLEN;
        }
        count = data[6];
        if (data_len != CYW43455_PNO_GSCAN_HEADER_LEN +
                        count * CYW43455_PNO_GSCAN_BUCKET_LEN) {
            return CYW43455_BCME_BADLEN;
        }
        if (lduw_le_p(data) != CYW43455_PNO_GSCAN_VERSION ||
            !count || count > CYW43455_PNO_MAX_BUCKETS ||
            (data[2] & ~BIT(3)) ||
            data[CYW43455_PNO_GSCAN_HEADER_LEN] >=
                s->pfn_channel_count) {
            return CYW43455_BCME_BADARG;
        }
        s->pfn_bucket_count = count;
        return 0;
    }
    if (!strcmp(name, "pfn_macaddr")) {
        static const uint8_t zero_mac[ETH_ALEN];

        if (data_len != CYW43455_PNO_MACADDR_LEN) {
            return CYW43455_BCME_BADLEN;
        }
        if (data[0] != CYW43455_PNO_MACADDR_VERSION ||
            (data[1] & ~CYW43455_PNO_MAC_FLAGS_MASK) ||
            (data[2] & 1) || !memcmp(data + 2, zero_mac, ETH_ALEN)) {
            return CYW43455_BCME_BADARG;
        }
        s->pfn_scan_mac_flags = data[1];
        memcpy(s->pfn_scan_mac, data + 2, ETH_ALEN);
        return 0;
    }
    if (!strcmp(name, "pfn_add")) {
        uint32_t ssid_len;
        uint32_t flags;

        if (data_len != CYW43455_PNO_NET_PARAM_LEN) {
            return CYW43455_BCME_BADLEN;
        }
        ssid_len = ldl_le_p(data);
        flags = ldl_le_p(data + 36);
        if (!ssid_len || ssid_len > CYW43455_PNO_SSID_MAX_LEN ||
            (flags & ~CYW43455_PNO_HIDDEN) ||
            ldl_le_p(data + 40) != 1 || ldl_le_p(data + 44) ||
            ldl_le_p(data + 48) != UINT32_MAX || ldl_le_p(data + 52) ||
            s->pfn_network_count == CYW43455_PNO_MAX_NETWORKS) {
            return CYW43455_BCME_BADARG;
        }
        i = s->pfn_network_count++;
        s->pfn_ssid_len[i] = ssid_len;
        memcpy(s->pfn_ssids[i], data + 4, ssid_len);
        s->pfn_network_flags[i] = flags;
        return 0;
    }
    return CYW43455_BCME_UNSUPPORTED;
}

static int32_t cyw43455_sdio_set_iovar(CYW43455SDIOState *s,
                                       const char *name,
                                       const uint8_t *data,
                                       uint32_t data_len)
{
    uint32_t count;

    if (s->fws_enabled && !strcmp(name, "event_msgs")) {
        bool was_enabled = cyw43455_sdio_fws_events_enabled(s);

        if (data_len < CYW43455_FWS_EVENT_MASK_MIN ||
            data_len > sizeof(s->fws_event_mask)) {
            return CYW43455_BCME_BADLEN;
        }
        if (!cyw43455_sdio_fws_block_migration(s)) {
            return CYW43455_BCME_BUSY;
        }
        memset(s->fws_event_mask, 0, sizeof(s->fws_event_mask));
        memcpy(s->fws_event_mask, data, data_len);
        if (!was_enabled && !s->fws_map_sent &&
            cyw43455_sdio_fws_events_enabled(s)) {
            s->fws_map_due_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                                s->fws_startup_delay_ms;
        }
        cyw43455_sdio_fws_schedule(s);
        return 0;
    }
    if (s->fws_enabled && !strcmp(name, "event_msgs_ext")) {
        /* Do not ACK an extended mask that this first slice cannot retain. */
        return CYW43455_BCME_UNSUPPORTED;
    }
    if (s->fws_enabled && !strcmp(name, "wlfc_mode")) {
        if (data_len != sizeof(uint32_t)) {
            return CYW43455_BCME_BADLEN;
        }
        /* No sequence reuse, suppression, or compressed status capability. */
        return ldl_le_p(data) == 0 ? 0 : CYW43455_BCME_UNSUPPORTED;
    }
    if (s->fws_enabled && !strcmp(name, "tlv")) {
        uint32_t value;

        if (data_len != sizeof(value)) {
            return CYW43455_BCME_BADLEN;
        }
        value = ldl_le_p(data);
        if (value != 0 && value != CYW43455_FWS_TLV_FLAGS) {
            return CYW43455_BCME_UNSUPPORTED;
        }
        if (!value) {
            if (s->fws_status_count || s->fws_retiring_count ||
                cyw43455_sdio_fws_credits_pending(s) || s->fws_if_pending ||
                s->f2_rx_len || s->f2_rx_pending_len ||
                (s->sdio_intstatus & CYW43455_I_HMB_FRAME_IND)) {
                return CYW43455_BCME_BUSY;
            }
            s->fws_negotiated = false;
            s->fws_map_sent = false;
            s->fws_bcmc_sent = false;
            s->fws_if_open = true;
            s->fws_status_head = 0;
            memset(s->fws_hanger_active, 0, sizeof(s->fws_hanger_active));
            memset(s->fws_status, 0, sizeof(s->fws_status));
            timer_del(s->fws_timer);
            /* Only a cold reset retires the unsaved negotiation history. */
            return 0;
        }
        if (s->fws_negotiated) {
            return 0;
        }
        if (!cyw43455_sdio_fws_block_migration(s)) {
            return CYW43455_BCME_BUSY;
        }
        s->fws_negotiated = true;
        s->fws_if_open = true;
        s->fws_map_due_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                            s->fws_startup_delay_ms;
        trace_cyw43455_sdio_fws_negotiate(value);
        cyw43455_sdio_fws_schedule(s);
        return 0;
    }

    if (!strcmp(name, "apsta") || !strcmp(name, "closednet")) {
        uint32_t value;

        if (data_len != sizeof(value)) {
            return CYW43455_BCME_BADLEN;
        }
        value = ldl_le_p(data);
        if (!strcmp(name, "apsta")) {
            return value == 0 ? 0 : CYW43455_BCME_UNSUPPORTED;
        }
        if (value > 1) {
            return CYW43455_BCME_BADARG;
        }
        s->ap_hidden = value;
        return 0;
    }
    /* No RX reordering, additional BSS, P2P or key-management simulation. */
    if (!strcmp(name, "tlv") || !strcmp(name, "wlfc_mode") ||
        !strcmp(name, "ampdu_hostreorder") || !strcmp(name, "mbss") ||
        !strcmp(name, "bss") || !strncmp(name, "bsscfg:", 7) ||
        !strncmp(name, "p2p", 3)) {
        return CYW43455_BCME_UNSUPPORTED;
    }
    if (!strcmp(name, "bus:txglomalign")) {
        uint32_t alignment;

        if (data_len != sizeof(uint32_t)) {
            return CYW43455_BCME_BADLEN;
        }
        alignment = ldl_le_p(data);
        if (alignment < 4 || alignment > 128 ||
            (alignment & (alignment - 1))) {
            return CYW43455_BCME_BADARG;
        }
        if (s->rx_glom_count) {
            return CYW43455_BCME_BUSY;
        }
        s->rx_glom_align = alignment;
        return 0;
    }

    if (!strcmp(name, "wpa_auth") || !strcmp(name, "wsec") ||
        !strcmp(name, "auth") ||
        !strcmp(name, "sup_wpa")) {
        if (data_len != sizeof(uint32_t)) {
            return CYW43455_BCME_BADLEN;
        }
        /* The reference traffic profile connects to an open synthetic BSS. */
        return ldl_le_p(data) == 0 ? 0 : CYW43455_BCME_UNSUPPORTED;
    }
    if (!strcmp(name, "wpaie")) {
        return data_len == 0 ? 0 : CYW43455_BCME_UNSUPPORTED;
    }
    if (s->ap_mode && !strcmp(name, "mfp")) {
        if (data_len != sizeof(uint32_t)) {
            return CYW43455_BCME_BADLEN;
        }
        if (ldl_le_p(data)) {
            return CYW43455_BCME_UNSUPPORTED;
        }
        s->mfp_mode = CYW43455_MFP_NONE;
        return 0;
    }

    if (!strcmp(name, "pfn") || !strcmp(name, "pfnclear") ||
        !strcmp(name, "pfn_set") || !strcmp(name, "pfn_cfg") ||
        !strcmp(name, "pfn_gscan_cfg") ||
        !strcmp(name, "pfn_macaddr") || !strcmp(name, "pfn_add")) {
        return cyw43455_sdio_set_pfn(s, name, data, data_len);
    }

    if (!strcmp(name, "cur_etheraddr")) {
        static const uint8_t zero_mac[ETH_ALEN];

        if (data_len != ETH_ALEN) {
            return CYW43455_BCME_BADLEN;
        }
        if ((data[0] & 1) || !memcmp(data, zero_mac, ETH_ALEN)) {
            return CYW43455_BCME_BADARG;
        }
        if (s->ap_started) {
            return CYW43455_BCME_BUSY;
        }
        memcpy(s->current_mac, data, ETH_ALEN);
        qemu_format_nic_info_str(qemu_get_queue(s->nic), s->current_mac);
        trace_cyw43455_sdio_mac(s->current_mac[0], s->current_mac[1],
                                s->current_mac[2], s->current_mac[3],
                                s->current_mac[4], s->current_mac[5]);
        return 0;
    }
    if (!strcmp(name, "escan")) {
        if (s->ap_mode) {
            return CYW43455_BCME_UNSUPPORTED;
        }
        return cyw43455_sdio_start_escan(s, data, data_len);
    }
    if (!strcmp(name, "bw_cap")) {
        return cyw43455_sdio_set_bw_cap(data, data_len);
    }
    if (!strcmp(name, "chanspec")) {
        uint32_t chanspec;
        uint8_t bandwidth_mhz;
        uint8_t control_channel;

        if (data_len != sizeof(chanspec)) {
            return CYW43455_BCME_BADLEN;
        }
        chanspec = ldl_le_p(data);
        if (!cyw43455_sdio_chanspec_info(chanspec, &control_channel,
                                         &bandwidth_mhz)) {
            return CYW43455_BCME_BADARG;
        }
        if (s->ap_mode && bandwidth_mhz != 20) {
            return CYW43455_BCME_UNSUPPORTED;
        }
        if (s->ap_started && chanspec != s->chanspec) {
            return CYW43455_BCME_BUSY;
        }
        s->chanspec = chanspec;
        if (s->ap_mode) {
            s->ap_channel_configured = true;
        }
        return 0;
    }
    if (!strcmp(name, "mcast_list")) {
        if (data_len < sizeof(count)) {
            return CYW43455_BCME_BADLEN;
        }
        count = ldl_le_p(data);
        if (count > CYW43455_MULTICAST_MAX) {
            return CYW43455_BCME_BADARG;
        }
        if (data_len != sizeof(count) + count * ETH_ALEN) {
            return CYW43455_BCME_BADLEN;
        }
        memset(s->multicast, 0, sizeof(s->multicast));
        memcpy(s->multicast, data + sizeof(count), count * ETH_ALEN);
        s->multicast_count = count;
        trace_cyw43455_sdio_filter(s->multicast_count, s->allmulti,
                                   s->promiscuous);
        return 0;
    }
    if (!strcmp(name, "allmulti")) {
        if (data_len != sizeof(uint32_t)) {
            return CYW43455_BCME_BADLEN;
        }
        s->allmulti = ldl_le_p(data) != 0;
        trace_cyw43455_sdio_filter(s->multicast_count, s->allmulti,
                                   s->promiscuous);
        return 0;
    }
    if (!strcmp(name, "arp_ol")) {
        if (data_len != sizeof(uint32_t)) {
            return CYW43455_BCME_BADLEN;
        }
        s->arp_mode = ldl_le_p(data);
        return 0;
    }
    if (!strcmp(name, "arpoe")) {
        if (data_len != sizeof(uint32_t)) {
            return CYW43455_BCME_BADLEN;
        }
        s->arp_offload = ldl_le_p(data) != 0;
        return 0;
    }
    if (!strcmp(name, "arp_hostip_clear")) {
        if (data_len) {
            return CYW43455_BCME_BADLEN;
        }
        memset(s->arp_hostip, 0, sizeof(s->arp_hostip));
        s->arp_hostip_count = 0;
        return 0;
    }
    if (!strcmp(name, "arp_hostip")) {
        static const uint8_t zero_ip[sizeof(uint32_t)];
        unsigned int i;

        if (data_len != sizeof(uint32_t)) {
            return CYW43455_BCME_BADLEN;
        }
        if (!memcmp(data, zero_ip, sizeof(zero_ip))) {
            return CYW43455_BCME_BADARG;
        }
        for (i = 0; i < s->arp_hostip_count; i++) {
            if (!memcmp(data, s->arp_hostip[i], sizeof(uint32_t))) {
                return 0;
            }
        }
        if (s->arp_hostip_count == CYW43455_ARP_HOSTIP_MAX) {
            return CYW43455_BCME_BADARG;
        }
        memcpy(s->arp_hostip[s->arp_hostip_count++], data,
               sizeof(uint32_t));
        return 0;
    }
    if (!strcmp(name, "ndoe")) {
        if (data_len != sizeof(uint32_t)) {
            return CYW43455_BCME_BADLEN;
        }
        s->nd_offload = ldl_le_p(data) != 0;
        return 0;
    }
    if (!strcmp(name, "nd_hostip_clear")) {
        if (data_len) {
            return CYW43455_BCME_BADLEN;
        }
        memset(s->nd_hostip, 0, sizeof(s->nd_hostip));
        s->nd_hostip_count = 0;
        return 0;
    }
    if (!strcmp(name, "nd_hostip")) {
        static const uint8_t zero_ip[16];
        unsigned int i;

        if (data_len != sizeof(s->nd_hostip[0])) {
            return CYW43455_BCME_BADLEN;
        }
        if (!memcmp(data, zero_ip, sizeof(zero_ip))) {
            return CYW43455_BCME_BADARG;
        }
        for (i = 0; i < s->nd_hostip_count; i++) {
            if (!memcmp(data, s->nd_hostip[i], sizeof(s->nd_hostip[0]))) {
                return 0;
            }
        }
        if (s->nd_hostip_count == CYW43455_ND_HOSTIP_MAX) {
            return CYW43455_BCME_BADARG;
        }
        memcpy(s->nd_hostip[s->nd_hostip_count++], data,
               sizeof(s->nd_hostip[0]));
        return 0;
    }
    if (!strcmp(name, "pm2_sleep_ret")) {
        if (data_len != sizeof(uint32_t)) {
            return CYW43455_BCME_BADLEN;
        }
        s->pm2_sleep_ret = ldl_le_p(data);
        return 0;
    }
    if (!strcmp(name, "obss_coex")) {
        int32_t mode;

        if (data_len != sizeof(mode)) {
            trace_cyw43455_sdio_obss_coex(0, CYW43455_BCME_BADLEN);
            return CYW43455_BCME_BADLEN;
        }
        mode = ldl_le_p(data);
        if (mode < CYW43455_OBSS_COEX_AUTO ||
            mode > CYW43455_OBSS_COEX_ON) {
            trace_cyw43455_sdio_obss_coex(mode, CYW43455_BCME_BADARG);
            return CYW43455_BCME_BADARG;
        }
        s->obss_coex = mode;
        trace_cyw43455_sdio_obss_coex(mode, 0);
        return 0;
    }
    if (!strcmp(name, "qtxpower")) {
        uint32_t qtxpower = 0;
        int32_t status = 0;

        if (data_len != sizeof(qtxpower)) {
            status = CYW43455_BCME_BADLEN;
        } else {
            qtxpower = ldl_le_p(data);
            if (qtxpower & ~(CYW43455_QTXPOWER_OVERRIDE |
                             CYW43455_QTXPOWER_MAX_QDBM)) {
                status = CYW43455_BCME_BADARG;
            } else {
                s->qtxpower = qtxpower;
            }
        }
        trace_cyw43455_sdio_tx_power(true,
                                     qtxpower & ~CYW43455_QTXPOWER_OVERRIDE,
                                     qtxpower & CYW43455_QTXPOWER_OVERRIDE,
                                     status);
        return status;
    }
    if (!strcmp(name, "mfp")) {
        uint32_t mode = 0;
        int32_t status = 0;

        if (data_len != sizeof(mode)) {
            status = CYW43455_BCME_BADLEN;
        } else {
            mode = ldl_le_p(data);
            if (mode > CYW43455_MFP_REQUIRED) {
                status = CYW43455_BCME_BADARG;
            } else {
                s->mfp_mode = mode;
            }
        }
        trace_cyw43455_sdio_mfp(true, mode, status);
        return status;
    }
    if (!strcmp(name, "tdls_enable")) {
        uint32_t enabled = 0;
        int32_t status = 0;

        if (data_len != sizeof(enabled)) {
            status = CYW43455_BCME_BADLEN;
        } else {
            enabled = ldl_le_p(data);
            if (enabled > 1) {
                status = CYW43455_BCME_BADARG;
            } else {
                s->tdls_enabled = enabled;
            }
        }
        trace_cyw43455_sdio_tdls(true, s->tdls_enabled, status);
        return status;
    }
    if (!strcmp(name, "roam_off")) {
        uint32_t disabled = 0;
        int32_t status = 0;

        if (data_len != sizeof(disabled)) {
            status = CYW43455_BCME_BADLEN;
        } else {
            disabled = ldl_le_p(data);
            if (disabled > 1) {
                status = CYW43455_BCME_BADARG;
            } else {
                s->roam_off = disabled;
            }
        }
        trace_cyw43455_sdio_roam_off(true, s->roam_off, status);
        return status;
    }
    if (!strcmp(name, "bcn_timeout")) {
        uint32_t timeout = 0;
        int32_t status = 0;

        if (data_len != sizeof(timeout)) {
            status = CYW43455_BCME_BADLEN;
        } else {
            timeout = ldl_le_p(data);
            s->bcn_timeout = timeout;
        }
        trace_cyw43455_sdio_bcn_timeout(true, s->bcn_timeout, status);
        return status;
    }
    return cyw43455_sdio_supported_set_iovar(name) ?
           0 : CYW43455_BCME_UNSUPPORTED;
}

static int32_t cyw43455_sdio_roam_parameter(CYW43455SDIOState *s,
                                             bool set, bool trigger,
                                             const uint8_t *input,
                                             uint32_t input_len,
                                             uint8_t *output,
                                             uint32_t output_len)
{
    int32_t *parameter = trigger ? &s->roam_trigger : &s->roam_delta;
    int32_t value = 0;
    uint32_t band = CYW43455_WLC_BAND_AUTO;
    int32_t status = 0;

    if (input_len != 2 * sizeof(uint32_t) ||
        (!set && output_len < 2 * sizeof(uint32_t))) {
        status = CYW43455_BCME_BADLEN;
    } else {
        value = ldl_le_p(input);
        band = ldl_le_p(input + sizeof(uint32_t));
        if (band != CYW43455_WLC_BAND_2G &&
            band != CYW43455_WLC_BAND_ALL) {
            status = CYW43455_BCME_BADARG;
        } else if (set) {
            *parameter = value;
        } else {
            value = *parameter;
            stl_le_p(output, value);
            stl_le_p(output + sizeof(uint32_t), band);
        }
    }
    trace_cyw43455_sdio_roam_parameter(set, trigger, value, band, status);
    return status;
}

/* Preserve already serialized frames and a descriptor-committed superframe. */
static void cyw43455_sdio_discard_uncommitted_rx(CYW43455SDIOState *s)
{
    if (!s->rx_glom_super_pending) {
        timer_del(s->rx_glom_timer);
        s->rx_glom_count = 0;
        s->rx_glom_len = 0;
        s->rx_glom_ready = false;
    }
    qemu_purge_queued_packets(qemu_get_queue(s->nic));
}

static void cyw43455_sdio_stop_ap(CYW43455SDIOState *s)
{
    bool started = s->ap_started;
    bool associated = s->ap_peer_associated;

    s->ap_events &= CYW43455_AP_EVENT_DISASSOC | CYW43455_AP_EVENT_DOWN;
    if (associated) {
        s->ap_events |= CYW43455_AP_EVENT_DISASSOC;
    }
    if (started) {
        s->ap_events |= CYW43455_AP_EVENT_DOWN;
    }
    s->ap_started = false;
    s->ap_peer_associated = false;
    s->ap_ssid_len = 0;
    memset(s->ap_ssid, 0, sizeof(s->ap_ssid));
    s->association_time_ms = -1;
    s->firmware_connected = false;
    s->association_requested = false;
    s->backend_link_events_armed = false;
    s->link_event_pending = false;
    s->link_ssid_pending = false;
    cyw43455_sdio_discard_uncommitted_rx(s);
    trace_cyw43455_sdio_ap_state(s->ap_mode, s->ap_started,
                                 s->ap_peer_associated, s->chanspec, 0);
}

static int32_t cyw43455_sdio_ap_ssid(CYW43455SDIOState *s,
                                    const uint8_t *input, uint32_t input_len)
{
    static const uint8_t zero_assoc[16];
    uint32_t length;
    uint8_t channel;
    uint8_t bandwidth;

    if (input_len != CYW43455_AP_JOIN_LEN) {
        return CYW43455_BCME_BADLEN;
    }
    length = ldl_le_p(input);
    if (length > CYW43455_AP_SSID_MAX ||
        memcmp(input + 36, zero_assoc, sizeof(zero_assoc))) {
        return CYW43455_BCME_BADARG;
    }
    if (!length) {
        cyw43455_sdio_stop_ap(s);
        return 0;
    }
    if (!s->infra || !s->rx_data_enabled || !s->ap_channel_configured) {
        return CYW43455_BCME_BADARG;
    }
    if (!cyw43455_sdio_chanspec_info(s->chanspec, &channel, &bandwidth) ||
        bandwidth != 20) {
        return CYW43455_BCME_BADARG;
    }
    if (s->ap_started || s->ap_events) {
        return CYW43455_BCME_BUSY;
    }
    s->ap_ssid_len = length;
    memset(s->ap_ssid, 0, sizeof(s->ap_ssid));
    memcpy(s->ap_ssid, input + 4, length);
    s->ap_started = true;
    s->ap_peer_associated = false;
    s->ap_events = CYW43455_AP_EVENT_UP;
    if (!qemu_get_queue(s->nic)->link_down) {
        s->ap_events |= CYW43455_AP_EVENT_ASSOC;
    }
    trace_cyw43455_sdio_ap_state(s->ap_mode, true, false,
                                 s->chanspec, length);
    return 0;
}

static int32_t cyw43455_sdio_control_reply(CYW43455SDIOState *s,
                                           uint32_t command, bool set,
                                           const uint8_t *input,
                                           uint32_t input_len,
                                           uint8_t *output,
                                           uint32_t output_len,
                                           char *iovar,
                                           size_t iovar_size)
{
    const uint8_t *iovar_data = NULL;
    uint32_t iovar_data_len = 0;

    if (command == CYW43455_C_GET_VAR || command == CYW43455_C_SET_VAR) {
        if (!cyw43455_sdio_iovar_name(input, input_len,
                                      iovar, iovar_size)) {
            return CYW43455_BCME_UNSUPPORTED;
        }
        iovar_data_len = strlen(iovar) + 1;
        iovar_data = input + iovar_data_len;
        iovar_data_len = input_len - iovar_data_len;
    }

    if (set) {
        if (command == CYW43455_C_GET_AP ||
            command == CYW43455_C_GET_INFRA ||
            command == CYW43455_C_GET_SSID ||
            command == CYW43455_C_GET_REGULATORY ||
            command == CYW43455_C_GET_BCNPRD ||
            command == CYW43455_C_GET_DTIMPRD ||
            command == CYW43455_C_GET_ASSOCLIST) {
            return CYW43455_BCME_UNSUPPORTED;
        }
        if (command == CYW43455_C_SET_AP ||
            command == CYW43455_C_SET_INFRA ||
            command == CYW43455_C_SET_REGULATORY ||
            command == CYW43455_C_SET_BCNPRD ||
            command == CYW43455_C_SET_DTIMPRD) {
            uint32_t value;

            if (input_len != sizeof(value)) {
                return CYW43455_BCME_BADLEN;
            }
            value = ldl_le_p(input);
            switch (command) {
            case CYW43455_C_SET_AP:
                if (value > 1) {
                    return CYW43455_BCME_BADARG;
                }
                if (value && (s->rx_data_enabled || !s->infra)) {
                    return CYW43455_BCME_BUSY;
                }
                cyw43455_sdio_stop_ap(s);
                s->explicit_role = true;
                s->ap_mode = value;
                if (value) {
                    s->mfp_mode = CYW43455_MFP_NONE;
                }
                s->ap_channel_configured = false;
                s->link_event_sent = false;
                return 0;
            case CYW43455_C_SET_INFRA:
                if (value != 1) {
                    return CYW43455_BCME_UNSUPPORTED;
                }
                if (s->rx_data_enabled && s->ap_mode) {
                    return CYW43455_BCME_BUSY;
                }
                s->infra = true;
                return 0;
            case CYW43455_C_SET_REGULATORY:
                if (value > 1) {
                    return CYW43455_BCME_BADARG;
                }
                s->regulatory = value;
                return 0;
            case CYW43455_C_SET_BCNPRD:
                if (!value || value > UINT16_MAX) {
                    return CYW43455_BCME_BADARG;
                }
                if (s->ap_started) {
                    return CYW43455_BCME_BUSY;
                }
                s->ap_beacon_period = value;
                return 0;
            case CYW43455_C_SET_DTIMPRD:
                if (!value || value > UINT8_MAX) {
                    return CYW43455_BCME_BADARG;
                }
                if (s->ap_started) {
                    return CYW43455_BCME_BUSY;
                }
                s->ap_dtim_period = value;
                return 0;
            }
        }
        if (command == CYW43455_C_UP || command == CYW43455_C_DOWN) {
            bool reopen_after_close =
                command == CYW43455_C_UP && s->fws_map_sent &&
                s->fws_if_open &&
                s->fws_if_pending == CYW43455_FWS_INTERFACE_CLOSE;

            if (input_len != 0 && input_len != sizeof(uint32_t)) {
                return CYW43455_BCME_BADLEN;
            }
            if (command == CYW43455_C_UP && s->ap_mode &&
                !s->ap_channel_configured) {
                return CYW43455_BCME_BADARG;
            }
            if (s->fws_map_sent &&
                ((!reopen_after_close && s->fws_if_pending) ||
                 s->fws_status_count ||
                 s->fws_retiring_count ||
                 cyw43455_sdio_fws_credits_pending(s))) {
                /* Do not ACK teardown before held host hangers are retired. */
                return CYW43455_BCME_BUSY;
            }
            if (reopen_after_close) {
                /*
                 * brcmfmac brings the primary interface back up immediately
                 * while stopping an AP.  Preserve both ordered notifications
                 * even when the close has not reached the host yet.
                 */
                s->fws_if_reopen_pending = true;
            } else if (s->fws_map_sent &&
                       s->fws_if_open != (command == CYW43455_C_UP)) {
                s->fws_if_pending = command == CYW43455_C_UP ?
                    CYW43455_FWS_INTERFACE_OPEN :
                    CYW43455_FWS_INTERFACE_CLOSE;
            }
            s->rx_data_enabled = command == CYW43455_C_UP;
            if (command == CYW43455_C_UP) {
                return 0;
            }
        }
        if (command == CYW43455_C_DISASSOC ||
            command == CYW43455_C_DOWN) {
            if (command == CYW43455_C_DISASSOC &&
                input_len != 0 && input_len != 12) {
                return CYW43455_BCME_BADLEN;
            }
            if (s->ap_mode) {
                cyw43455_sdio_stop_ap(s);
                return 0;
            }
            s->association_requested = false;
            if (!s->rx_glom_super_pending) {
                timer_del(s->rx_glom_timer);
                s->rx_glom_count = 0;
                s->rx_glom_len = 0;
                s->rx_glom_ready = false;
            }
            s->link_ssid_pending = false;
            s->link_event_up = false;
            s->link_event_pending = s->firmware_connected;
            s->link_event_sent = false;
            return 0;
        }
        if (command == CYW43455_C_SET_SSID ||
            (command == CYW43455_C_SET_VAR && !strcmp(iovar, "join"))) {
            const uint8_t *ssid = command == CYW43455_C_SET_SSID ?
                                  input : iovar_data;
            uint32_t length = command == CYW43455_C_SET_SSID ?
                              input_len : iovar_data_len;

            if (s->ap_mode) {
                return command == CYW43455_C_SET_SSID ?
                       cyw43455_sdio_ap_ssid(s, ssid, length) :
                       CYW43455_BCME_UNSUPPORTED;
            }
            if (s->ap_events) {
                return CYW43455_BCME_BUSY;
            }
            /* Both join layouts begin with a 32-byte, length-prefixed SSID. */
            if (length < 36) {
                return CYW43455_BCME_BADLEN;
            }
            if (ldl_le_p(ssid) != strlen(CYW43455_BSS_SSID) ||
                !cyw43455_sdio_scan_ssid_match(ssid)) {
                return CYW43455_BCME_BADARG;
            }
            if (s->explicit_role) {
                s->chanspec = s->ap_chanspec;
            }
            s->association_requested = true;
            if (s->auto_associate && s->rx_data_enabled &&
                !qemu_get_queue(s->nic)->link_down) {
                s->link_event_up = true;
                s->link_ssid_pending = false;
                s->link_event_pending = true;
            }
            return 0;
        }
        if (command == CYW43455_C_SET_PM) {
            uint32_t power_mode = UINT32_MAX;
            int32_t status = 0;

            if (input_len != sizeof(power_mode)) {
                status = CYW43455_BCME_BADLEN;
            } else {
                power_mode = ldl_le_p(input);
                if (power_mode > CYW43455_PM_FAST) {
                    status = CYW43455_BCME_BADARG;
                } else {
                    s->power_mode = power_mode;
                }
            }
            trace_cyw43455_sdio_power_mode(true, power_mode, status);
            return status;
        }
        if (command == CYW43455_C_SET_PROMISC) {
            if (input_len != sizeof(uint32_t)) {
                return CYW43455_BCME_BADLEN;
            }
            s->promiscuous = ldl_le_p(input) != 0;
            trace_cyw43455_sdio_filter(s->multicast_count, s->allmulti,
                                       s->promiscuous);
            return 0;
        }
        if (command == CYW43455_C_SET_ROAM_TRIGGER ||
            command == CYW43455_C_SET_ROAM_DELTA) {
            return cyw43455_sdio_roam_parameter(
                s, true, command == CYW43455_C_SET_ROAM_TRIGGER,
                input, input_len, output, output_len);
        }
        if (command != CYW43455_C_SET_VAR) {
            return s->explicit_role ? CYW43455_BCME_UNSUPPORTED : 0;
        }
        return cyw43455_sdio_set_iovar(s, iovar, iovar_data,
                                       iovar_data_len);
    }

    switch (command) {
    case CYW43455_C_GET_AP:
    case CYW43455_C_GET_INFRA:
    case CYW43455_C_GET_REGULATORY:
    case CYW43455_C_GET_BCNPRD:
    case CYW43455_C_GET_DTIMPRD:
        if (output_len < sizeof(uint32_t)) {
            return CYW43455_BCME_BADLEN;
        }
        stl_le_p(output,
                 command == CYW43455_C_GET_AP ? s->ap_mode :
                 command == CYW43455_C_GET_INFRA ? s->infra :
                 command == CYW43455_C_GET_REGULATORY ? s->regulatory :
                 command == CYW43455_C_GET_BCNPRD ? s->ap_beacon_period :
                                                   s->ap_dtim_period);
        return 0;
    case CYW43455_C_GET_SSID:
        if (output_len < 36) {
            return CYW43455_BCME_BADLEN;
        }
        if (s->ap_mode) {
            stl_le_p(output, s->ap_ssid_len);
            memcpy(output + 4, s->ap_ssid, s->ap_ssid_len);
        } else if (s->firmware_connected) {
            stl_le_p(output, strlen(CYW43455_BSS_SSID));
            memcpy(output + 4, CYW43455_BSS_SSID, strlen(CYW43455_BSS_SSID));
        }
        return 0;
    case CYW43455_C_GET_ASSOCLIST: {
        bool associated = s->ap_mode ? s->ap_peer_associated :
                                       s->firmware_connected;

        if (input_len < sizeof(uint32_t) ||
            output_len < sizeof(uint32_t) ||
            (associated && (ldl_le_p(input) < 1 ||
                            output_len < sizeof(uint32_t) + ETH_ALEN))) {
            return CYW43455_BCME_BADLEN;
        }
        stl_le_p(output, associated ? 1 : 0);
        if (associated) {
            memcpy(output + sizeof(uint32_t), cyw43455_sdio_peer_mac,
                   ETH_ALEN);
        }
        return 0;
    }
    case CYW43455_C_GET_ROAM_TRIGGER:
    case CYW43455_C_GET_ROAM_DELTA:
        return cyw43455_sdio_roam_parameter(
            s, false, command == CYW43455_C_GET_ROAM_TRIGGER,
            input, input_len, output, output_len);
    case CYW43455_C_GET_PM:
        if (output_len < sizeof(s->power_mode)) {
            trace_cyw43455_sdio_power_mode(false, s->power_mode,
                                           CYW43455_BCME_BADLEN);
            return CYW43455_BCME_BADLEN;
        }
        cyw43455_sdio_store_u32(output, output_len, 0, s->power_mode);
        trace_cyw43455_sdio_power_mode(false, s->power_mode, 0);
        return 0;
    case CYW43455_C_GET_VERSION:
        cyw43455_sdio_store_u32(output, output_len, 0,
                                CYW43455_D11AC_IOTYPE);
        return 0;
    case CYW43455_C_GET_REVINFO:
        /* Layout matches the 17 little-endian words in brcmf_rev_info_le. */
        cyw43455_sdio_store_u32(output, output_len, 0, 0x14e4);
        cyw43455_sdio_store_u32(output, output_len, 4,
                                cyw43455_sdio_device_id(s));
        cyw43455_sdio_store_u32(output, output_len, 12,
                                cyw43455_sdio_chip_revision(s));
        cyw43455_sdio_store_u32(output, output_len, 44, 4345);
        return 0;
    case CYW43455_C_GET_PHY_NOISE:
        cyw43455_sdio_store_u32(output, output_len, 0,
                                (uint32_t)(int32_t)CYW43455_NOISE_DBM);
        return 0;
    case CYW43455_C_GET_BSS_INFO:
        return cyw43455_sdio_bss_info(s, input, input_len,
                                      output, output_len);
    case CYW43455_C_GET_BANDLIST:
        if (output_len < 3 * sizeof(uint32_t)) {
            return CYW43455_BCME_BADLEN;
        }
        cyw43455_sdio_store_u32(output, output_len, 0, 2);
        cyw43455_sdio_store_u32(output, output_len, 4,
                                CYW43455_WLC_BAND_2G);
        cyw43455_sdio_store_u32(output, output_len, 8,
                                CYW43455_WLC_BAND_5G);
        return 0;
    case CYW43455_C_GET_VAR:
        if (s->fws_enabled && !strcmp(iovar, "event_msgs")) {
            if (output_len < CYW43455_FWS_EVENT_MASK_MIN) {
                return CYW43455_BCME_BADLEN;
            }
            memcpy(output, s->fws_event_mask,
                   MIN(output_len, sizeof(s->fws_event_mask)));
        } else if (s->fws_enabled && (!strcmp(iovar, "wlfc_mode") ||
                                     !strcmp(iovar, "tlv"))) {
            if (output_len < sizeof(uint32_t)) {
                return CYW43455_BCME_BADLEN;
            }
            stl_le_p(output, !strcmp(iovar, "tlv") && s->fws_negotiated ?
                             CYW43455_FWS_TLV_FLAGS : 0);
        } else if (!strcmp(iovar, "apsta") || !strcmp(iovar, "closednet")) {
            if (output_len < sizeof(uint32_t)) {
                return CYW43455_BCME_BADLEN;
            }
            stl_le_p(output, !strcmp(iovar, "closednet") ? s->ap_hidden : 0);
        } else if (!strcmp(iovar, "assoc_info")) {
            if (output_len < 2 * sizeof(uint32_t)) {
                return CYW43455_BCME_BADLEN;
            }
            /* No separate association request/response IEs in this profile. */
            memset(output, 0, output_len);
        } else if (!strcmp(iovar, "wpa_auth") || !strcmp(iovar, "wsec") ||
            !strcmp(iovar, "auth") ||
            !strcmp(iovar, "sup_wpa")) {
            cyw43455_sdio_store_u32(output, output_len, 0, 0);
        } else if (!strcmp(iovar, "cur_etheraddr")) {
            memcpy(output, s->current_mac,
                   MIN(sizeof(s->current_mac), output_len));
        } else if (!strcmp(iovar, "ver")) {
            cyw43455_sdio_store_string(output, output_len,
                                       "QEMU CYW43455 synthetic 1.0\n");
        } else if (!strcmp(iovar, "clmver")) {
            cyw43455_sdio_store_string(output, output_len,
                                       "QEMU synthetic CLM 1.0\n");
        } else if (!strcmp(iovar, "pfn")) {
            cyw43455_sdio_store_u32(output, output_len, 0,
                                    s->pfn_enabled);
            trace_cyw43455_sdio_pno(false, s->pfn_enabled,
                                    s->pfn_scan_freq,
                                    s->pfn_channel_count,
                                    s->pfn_network_count, 0);
        } else if (!strcmp(iovar, "pfn_macaddr")) {
            if (output_len < CYW43455_PNO_MACADDR_LEN) {
                return CYW43455_BCME_BADLEN;
            }
            output[0] = CYW43455_PNO_MACADDR_VERSION;
            output[1] = s->pfn_scan_mac_flags;
            memcpy(output + 2, s->pfn_scan_mac, ETH_ALEN);
        } else if (!strcmp(iovar, "cap") ||
                   !strcmp(iovar, "event_msgs") ||
                   !strcmp(iovar, "clmload_status") ||
                   !strcmp(iovar, "per_chan_info") ||
                   !strcmp(iovar, "toe_ol")) {
            /* The zero-filled response is the intentionally empty value. */
        } else if (!strcmp(iovar, "dump_obss")) {
            return cyw43455_sdio_dump_obss(s, iovar_data,
                                           iovar_data_len, output,
                                           output_len);
        } else if (!strcmp(iovar, "scan_ver")) {
            cyw43455_sdio_store_u32(output, output_len, 0,
                                    CYW43455_ESCAN_REQ_VERSION_V2);
        } else if (!strcmp(iovar, "wlc_ver")) {
            return cyw43455_sdio_wlc_version(s, output, output_len);
        } else if (!strcmp(iovar, "bw_cap")) {
            return cyw43455_sdio_get_bw_cap(iovar_data, iovar_data_len,
                                            output, output_len);
        } else if (!strcmp(iovar, "nmode")) {
            /* Keep cfg80211 HT state consistent with our HT chanspec/BSS. */
            cyw43455_sdio_store_u32(output, output_len, 0, 1);
        } else if (!strcmp(iovar, "vhtmode") ||
                   !strcmp(iovar, "mimo_bw_cap")) {
            cyw43455_sdio_store_u32(output, output_len, 0, 0);
        } else if (!strcmp(iovar, "rxchain")) {
            cyw43455_sdio_store_u32(output, output_len, 0, 1);
        } else if (!strcmp(iovar, "chanspec")) {
            cyw43455_sdio_store_u32(output, output_len, 0, s->chanspec);
        } else if (!strcmp(iovar, "chanspecs")) {
            return cyw43455_sdio_get_chanspecs(iovar_data, iovar_data_len,
                                               output, output_len);
        } else if (!strcmp(iovar, "allmulti")) {
            cyw43455_sdio_store_u32(output, output_len, 0, s->allmulti);
        } else if (!strcmp(iovar, "arp_ol")) {
            cyw43455_sdio_store_u32(output, output_len, 0, s->arp_mode);
        } else if (!strcmp(iovar, "arpoe")) {
            cyw43455_sdio_store_u32(output, output_len, 0,
                                    s->arp_offload);
        } else if (!strcmp(iovar, "arp_version")) {
            cyw43455_sdio_store_u32(output, output_len, 0, 1);
        } else if (!strcmp(iovar, "arp_hostip")) {
            memcpy(output, s->arp_hostip,
                   MIN(output_len, s->arp_hostip_count *
                                   sizeof(s->arp_hostip[0])));
        } else if (!strcmp(iovar, "ndoe")) {
            cyw43455_sdio_store_u32(output, output_len, 0,
                                    s->nd_offload);
        } else if (!strcmp(iovar, "nd_hostip")) {
            memcpy(output, s->nd_hostip,
                   MIN(output_len, s->nd_hostip_count *
                                   sizeof(s->nd_hostip[0])));
        } else if (!strcmp(iovar, "sta_info")) {
            return cyw43455_sdio_station_info(s, iovar_data,
                                              iovar_data_len, output,
                                              output_len);
        } else if (!strcmp(iovar, "pm2_sleep_ret")) {
            cyw43455_sdio_store_u32(output, output_len, 0,
                                    s->pm2_sleep_ret);
        } else if (!strcmp(iovar, "obss_coex")) {
            cyw43455_sdio_store_u32(output, output_len, 0,
                                    s->obss_coex);
        } else if (!strcmp(iovar, "qtxpower")) {
            cyw43455_sdio_store_u32(output, output_len, 0,
                                    s->qtxpower);
            trace_cyw43455_sdio_tx_power(
                false, s->qtxpower & ~CYW43455_QTXPOWER_OVERRIDE,
                s->qtxpower & CYW43455_QTXPOWER_OVERRIDE, 0);
        } else if (!strcmp(iovar, "mfp")) {
            cyw43455_sdio_store_u32(output, output_len, 0,
                                    s->mfp_mode);
            trace_cyw43455_sdio_mfp(false, s->mfp_mode, 0);
        } else if (!strcmp(iovar, "tdls_enable")) {
            cyw43455_sdio_store_u32(output, output_len, 0,
                                    s->tdls_enabled);
            trace_cyw43455_sdio_tdls(false, s->tdls_enabled, 0);
        } else if (!strcmp(iovar, "roam_off")) {
            cyw43455_sdio_store_u32(output, output_len, 0, s->roam_off);
            trace_cyw43455_sdio_roam_off(false, s->roam_off, 0);
        } else if (!strcmp(iovar, "bcn_timeout")) {
            cyw43455_sdio_store_u32(output, output_len, 0,
                                    s->bcn_timeout);
            trace_cyw43455_sdio_bcn_timeout(false, s->bcn_timeout, 0);
        } else if (!strcmp(iovar, "mcast_list")) {
            uint32_t list_len = output_len > sizeof(uint32_t) ?
                MIN(output_len - sizeof(uint32_t),
                    s->multicast_count * ETH_ALEN) : 0;

            cyw43455_sdio_store_u32(output, output_len, 0,
                                    s->multicast_count);
            if (output_len > sizeof(uint32_t)) {
                memcpy(output + sizeof(uint32_t), s->multicast, list_len);
            }
        } else {
            return CYW43455_BCME_UNSUPPORTED;
        }
        return 0;
    default:
        return CYW43455_BCME_UNSUPPORTED;
    }
}

static bool cyw43455_sdio_fws_parse(const uint8_t *signal,
                                    unsigned int length, uint32_t *tag,
                                    bool *tim)
{
    bool have_tag = false;
    bool have_pending = false;
    unsigned int offset = 0;

    while (offset < length) {
        uint8_t type = signal[offset++];
        uint8_t len;

        if (type == CYW43455_FWS_FILLER) {
            continue;
        }
        if (offset == length) {
            return false;
        }
        len = signal[offset++];
        if (len > length - offset) {
            return false;
        }
        if (type == CYW43455_FWS_PKTTAG && len == 4 && !have_tag) {
            *tag = ldl_le_p(signal + offset);
            have_tag = true;
        } else if (type == CYW43455_FWS_PENDING_TRAFFIC && len == 2 &&
                   !have_pending && !signal[offset] &&
                   !(signal[offset + 1] & ~0x1f)) {
            have_pending = true;
        } else {
            /* Includes sequence reuse, duplicate tag, and unknown TLVs. */
            return false;
        }
        offset += len;
    }
    *tim = have_tag && !*tag && have_pending;
    /* Linux can piggyback the primary pending bitmap on an ordinary tag. */
    return have_tag;
}

static bool cyw43455_sdio_fws_accept(CYW43455SDIOState *s, uint32_t tag,
                                     bool tossed)
{
    unsigned int fifo = extract32(tag, 24, 3);
    unsigned int hslot = extract32(tag, 8, 16);
    unsigned int tail;
    uint8_t *available = fifo == 4 ? &s->fws_bcmc_available :
                                   &s->fws_ac_available;

    /* Host AC borrowing is not on the wire; reserve the shared AC pool. */
    if ((tag & CYW43455_FWS_TAG_FLAGS) != CYW43455_FWS_TAG_FROM_HOST ||
        fifo >= 5 || hslot >= CYW43455_FWS_HANGER_SLOTS ||
        s->fws_hanger_active[hslot] ||
        !*available ||
        s->fws_status_count + s->fws_retiring_count >=
        CYW43455_FWS_STATUS_MAX) {
        trace_cyw43455_sdio_fws_drop(tag, 2);
        return false;
    }
    tail = (s->fws_status_head + s->fws_status_count) %
           CYW43455_FWS_STATUS_MAX;
    s->fws_status[tail].tag = (tag & ~CYW43455_FWS_TAG_FLAGS) |
                             (tossed ? CYW43455_FWS_STATUS_TOSSED : 0);
    s->fws_status[tail].due_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                                s->fws_status_delay_ms;
    s->fws_status_count++;
    s->fws_hanger_active[hslot] = true;
    (*available)--;
    trace_cyw43455_sdio_fws_accept(tag, s->fws_status_count +
                                   s->fws_retiring_count, *available);
    cyw43455_sdio_fws_schedule(s);
    return true;
}

static void cyw43455_sdio_forward_data(CYW43455SDIOState *s,
                                       const uint8_t *request,
                                       uint32_t request_len,
                                       uint8_t software_offset,
                                       uint8_t data_offset)
{
    const struct eth_header *eth;
    const struct ip_header *ip;
    const struct udp_header *udp;
    const uint8_t *packet;
    uint32_t ethernet_offset;
    uint32_t ethernet_len;
    uint32_t frame_len = request_len;
    uint32_t ip_header_len;
    uint32_t signal_len;
    uint16_t ethertype;
    uint16_t source_port = 0;
    uint16_t destination_port = 0;
    uint8_t ip_protocol = 0;
    uint32_t fws_tag = 0;
    bool fws_active = s->fws_negotiated && s->fws_map_sent;
    bool tim = false;
    bool drop;

    if (software_offset > 4) {
        /* The extension length excludes only the four-byte frame tag. */
        frame_len = lduw_le_p(request + 4) + 4;
        if (frame_len > request_len || frame_len < software_offset + 8) {
            return;
        }
    }
    if (data_offset < software_offset + 8 || data_offset > frame_len ||
        frame_len - data_offset < CYW43455_BCDC_DATA_HEADER_LEN) {
        return;
    }

    signal_len = request[data_offset + 3] * sizeof(uint32_t);
    if (s->ap_mode && ((!fws_active && signal_len) ||
                       (request[data_offset + 2] & 0x0f))) {
        /* SDPCM credits are not proprietary FWS TLV/status support. */
        trace_cyw43455_sdio_ap_drop(true, 1, request_len);
        return;
    }
    if (signal_len > frame_len - data_offset -
                     CYW43455_BCDC_DATA_HEADER_LEN) {
        return;
    }
    ethernet_offset = data_offset + CYW43455_BCDC_DATA_HEADER_LEN +
                      signal_len;
    ethernet_len = frame_len - ethernet_offset;
    if (fws_active) {
        if ((request[data_offset] >> 4) != CYW43455_BCDC_PROTO_VER ||
            (request[data_offset + 2] & 0x0f) ||
            !cyw43455_sdio_fws_parse(
                request + data_offset + CYW43455_BCDC_DATA_HEADER_LEN,
                signal_len, &fws_tag, &tim)) {
            trace_cyw43455_sdio_fws_drop(fws_tag, 1);
            return;
        }
        if (tim && !ethernet_len) {
            /* Signal-only TIM has no host hanger or Ethernet ownership. */
            return;
        }
    } else if (s->fws_enabled && signal_len) {
        return;
    }
    if (ethernet_len < ETH_HLEN) {
        return;
    }

    packet = request + ethernet_offset;
    drop = !cyw43455_sdio_link_available(s) ||
           (fws_active && !s->fws_if_open) ||
           (s->ap_mode && !(packet[0] & 1) &&
            memcmp(packet, cyw43455_sdio_peer_mac, ETH_ALEN));
    if (fws_active &&
        !cyw43455_sdio_fws_accept(s, fws_tag, drop)) {
        return;
    }
    if (drop) {
        trace_cyw43455_sdio_ap_drop(true, 2, ethernet_len);
        return;
    }
    eth = (const struct eth_header *)packet;
    ethertype = be16_to_cpu(eth->h_proto);
    if (ethertype == ETH_P_IP &&
        ethernet_len >= ETH_HLEN + sizeof(struct ip_header)) {
        ip = (const struct ip_header *)(packet + ETH_HLEN);
        ip_header_len = (ip->ip_ver_len & 0xf) * sizeof(uint32_t);
        if ((ip->ip_ver_len >> 4) == 4 &&
            ip_header_len >= sizeof(struct ip_header) &&
            ethernet_len >= ETH_HLEN + ip_header_len) {
            ip_protocol = ip->ip_p;
            if (ip_protocol == IP_PROTO_UDP &&
                ethernet_len >= ETH_HLEN + ip_header_len +
                                sizeof(struct udp_header)) {
                udp = (const struct udp_header *)(packet + ETH_HLEN +
                                                  ip_header_len);
                source_port = be16_to_cpu(udp->uh_sport);
                destination_port = be16_to_cpu(udp->uh_dport);
            }
        }
    }

    trace_cyw43455_sdio_data(ethernet_len, ethertype, ip_protocol,
                             source_port, destination_port);
    s->tx_packets++;
    s->tx_bytes += ethernet_len;
    if (packet[0] & 1) {
        s->tx_multicast_packets++;
        s->tx_multicast_bytes += ethernet_len;
    }
    qemu_send_packet(qemu_get_queue(s->nic), packet, ethernet_len);
}

static uint8_t cyw43455_sdio_forward_glom(CYW43455SDIOState *s,
                                          const uint8_t *request,
                                          uint32_t request_len)
{
    uint32_t offset = 0;
    unsigned int frames = 0;
    uint8_t last_sequence = request[CYW43455_SDPCM_HW_HEADER_LEN +
                                    CYW43455_SDPCM_HWEXT_LEN];

    while (request_len - offset >= CYW43455_SDPCM_HEADER_LEN +
                                   CYW43455_SDPCM_HWEXT_LEN) {
        const uint8_t *frame = request + offset;
        uint32_t extension = ldl_le_p(frame +
                                      CYW43455_SDPCM_HW_HEADER_LEN);
        uint32_t extension2 = ldl_le_p(
            frame + CYW43455_SDPCM_HW_HEADER_LEN + sizeof(extension));
        uint32_t frame_len = (extension &
                              CYW43455_SDPCM_HWEXT_LENGTH_MASK) +
                             CYW43455_SDPCM_HW_HEADER_LEN;
        uint32_t tail_pad = extract32(extension2, 16, 16);
        uint32_t stored_len;
        uint8_t software_offset = CYW43455_SDPCM_HW_HEADER_LEN +
                                  CYW43455_SDPCM_HWEXT_LEN;
        uint8_t data_offset;

        if (frame_len > request_len - offset ||
            tail_pad > request_len - offset - frame_len) {
            break;
        }
        stored_len = frame_len + tail_pad;
        data_offset = frame[software_offset + 3];
        if ((frame[software_offset + 1] & 0xf) !=
            CYW43455_SDPCM_DATA_CHANNEL ||
            data_offset < software_offset + 8 ||
            data_offset > frame_len) {
            break;
        }

        cyw43455_sdio_forward_data(s, frame, frame_len,
                                   software_offset, data_offset);
        last_sequence = frame[software_offset];
        frames++;
        if (extension & CYW43455_SDPCM_HWEXT_LAST) {
            break;
        }
        if (!stored_len) {
            break;
        }
        offset += stored_len;
    }

    trace_cyw43455_sdio_glom(request_len, frames, last_sequence);
    return last_sequence;
}

static uint8_t cyw43455_sdio_tx_max(CYW43455SDIOState *s,
                                    uint8_t tx_sequence)
{
    return tx_sequence + s->tx_credit_window;
}

static void cyw43455_sdio_emit_tx_credit_frame(CYW43455SDIOState *s)
{
    uint8_t *response = s->f2_rx_frame;

    assert(s->tx_credit_update_pending);
    memset(response, 0, CYW43455_SDPCM_HEADER_LEN);
    stw_le_p(response, CYW43455_SDPCM_HEADER_LEN);
    stw_le_p(response + 2, (uint16_t)~CYW43455_SDPCM_HEADER_LEN);
    response[4] = s->f2_rx_seq++;
    /* Observed firmware uses the event channel for header-only updates. */
    response[5] = CYW43455_SDPCM_EVENT_CHANNEL;
    response[7] = CYW43455_SDPCM_HEADER_LEN;
    response[9] = s->tx_credit_limit;

    s->f2_rx_len = CYW43455_SDPCM_HEADER_LEN;
    s->f2_rx_pos = 0;
    s->tx_credit_update_pending = false;
    s->sdio_intstatus |= CYW43455_I_HMB_FRAME_IND;
    trace_cyw43455_sdio_credit_emitted(s->tx_credit_seq, s->tx_credit_limit);
    cyw43455_sdio_update_irq(s);
}

static void cyw43455_sdio_emit_tx_credit(void *opaque)
{
    CYW43455SDIOState *s = opaque;

    if (!s->tx_credit_pending) {
        return;
    }
    s->tx_credit_limit = cyw43455_sdio_tx_max(s, s->tx_credit_seq);
    s->tx_credit_pending = false;
    s->tx_credit_update_pending = true;
    /* Coalesce notifications only while the guest has read none of it. */
    if (s->f2_rx_len == CYW43455_SDPCM_HEADER_LEN && !s->f2_rx_pos &&
        s->f2_rx_frame[5] == CYW43455_SDPCM_EVENT_CHANNEL &&
        s->f2_rx_frame[7] == CYW43455_SDPCM_HEADER_LEN) {
        s->f2_rx_frame[9] = s->tx_credit_limit;
        s->tx_credit_update_pending = false;
    }
    /* A busy receive FIFO defers notification, not credit retirement. */
    cyw43455_sdio_advance_rx(s);
}

static void cyw43455_sdio_schedule_tx_credit(CYW43455SDIOState *s,
                                             uint8_t tx_sequence)
{
    s->tx_credit_seq = tx_sequence;
    trace_cyw43455_sdio_credit_scheduled(
        tx_sequence, cyw43455_sdio_tx_max(s, tx_sequence),
        s->tx_credit_delay_ms);
    if (!s->tx_credit_delay_ms) {
        s->tx_credit_limit = cyw43455_sdio_tx_max(s, tx_sequence);
    }
    if (!s->tx_credit_pending) {
        s->tx_credit_pending = true;
        timer_mod(s->tx_credit_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                  s->tx_credit_delay_ms);
    }
}

static void cyw43455_sdio_process_tx_frame(CYW43455SDIOState *s)
{
    const uint8_t *request = s->f2_tx_frame;
    uint8_t *response;
    uint16_t request_len;
    uint16_t request_checksum;
    uint8_t data_offset;
    uint8_t channel;
    uint8_t software_offset;
    uint32_t command;
    uint32_t data_len;
    uint32_t flags;
    uint32_t payload_len;
    uint32_t response_payload_len;
    uint32_t response_len;
    int32_t status;
    bool response_queued;
    bool set;
    char iovar[64] = { };

    if (s->f2_tx_len < CYW43455_SDPCM_HEADER_LEN +
                       CYW43455_BCDC_DATA_HEADER_LEN) {
        return;
    }

    request_len = lduw_le_p(request);
    request_checksum = lduw_le_p(request + 2);
    if ((uint16_t)~request_len != request_checksum ||
        request_len > s->f2_tx_len) {
        return;
    }

    software_offset = 4;
    channel = request[software_offset + 1] & 0xf;
    data_offset = request[software_offset + 3];
    if ((data_offset < software_offset + 8 ||
         data_offset > request_len) &&
        request_len >= CYW43455_SDPCM_HEADER_LEN + 8 +
                       CYW43455_BCDC_HEADER_LEN) {
        /* Transmit glomming inserts an eight-byte hardware extension. */
        software_offset += 8;
        channel = request[software_offset + 1] & 0xf;
        data_offset = request[software_offset + 3];
    }
    trace_cyw43455_sdio_frame(request_len, request[software_offset],
                              channel, data_offset);
    if (channel == CYW43455_SDPCM_DATA_CHANNEL) {
        uint8_t tx_sequence = request[software_offset];

        if (software_offset > CYW43455_SDPCM_HW_HEADER_LEN) {
            tx_sequence = cyw43455_sdio_forward_glom(s, request,
                                                     request_len);
        } else {
            cyw43455_sdio_forward_data(s, request, request_len,
                                       software_offset, data_offset);
        }
        cyw43455_sdio_schedule_tx_credit(s, tx_sequence);
        if (s->fws_negotiated) {
            cyw43455_sdio_advance_rx(s);
        }
        return;
    }
    if (channel != CYW43455_SDPCM_CONTROL_CHANNEL ||
        request_len < CYW43455_SDPCM_HEADER_LEN +
                      CYW43455_BCDC_HEADER_LEN ||
        data_offset < software_offset + 8 ||
        data_offset > request_len - CYW43455_BCDC_HEADER_LEN) {
        return;
    }

    command = ldl_le_p(request + data_offset);
    data_len = ldl_le_p(request + data_offset + 4);
    flags = ldl_le_p(request + data_offset + 8);
    set = flags & CYW43455_BCDC_DCMD_SET;
    payload_len = MIN(data_len, request_len - data_offset -
                                CYW43455_BCDC_HEADER_LEN);
    response_payload_len = set ? payload_len : data_len;
    response_len = CYW43455_SDPCM_HEADER_LEN + CYW43455_BCDC_HEADER_LEN +
                   response_payload_len;
    if (response_len > CYW43455_F2_FRAME_MAX) {
        return;
    }

    response_queued = s->f2_rx_len || s->f2_rx_pending_len ||
                      (s->sdio_intstatus & CYW43455_I_HMB_FRAME_IND);
    if (response_queued) {
        if (s->f2_rx_pending_len) {
            trace_cyw43455_sdio_control_dropped(command,
                                                extract32(flags, 16, 16));
            return;
        }
        response = s->f2_rx_pending_frame;
    } else {
        response = s->f2_rx_frame;
    }

    memset(response, 0, QEMU_ALIGN_UP(response_len, 4));
    stw_le_p(response, response_len);
    stw_le_p(response + 2, (uint16_t)~response_len);
    response[4] = s->f2_rx_seq++;
    response[5] = CYW43455_SDPCM_CONTROL_CHANNEL;
    response[7] = CYW43455_SDPCM_HEADER_LEN;
    /* A control response also acknowledges outstanding host transmit data. */
    response[9] = cyw43455_sdio_tx_max(s, request[software_offset]);

    s->tx_credit_seq = request[software_offset];
    s->tx_credit_limit = response[9];
    s->tx_credit_pending = false;
    s->tx_credit_update_pending = false;
    timer_del(s->tx_credit_timer);

    stl_le_p(response + CYW43455_SDPCM_HEADER_LEN, command);
    stl_le_p(response + CYW43455_SDPCM_HEADER_LEN + 4, data_len);
    status = extract32(flags, 12, 4) ? CYW43455_BCME_UNSUPPORTED :
             cyw43455_sdio_control_reply(
        s, command, set,
        request + data_offset + CYW43455_BCDC_HEADER_LEN, payload_len,
        response + CYW43455_SDPCM_HEADER_LEN + CYW43455_BCDC_HEADER_LEN,
        response_payload_len, iovar, sizeof(iovar));
    if (status < 0) {
        flags |= CYW43455_BCDC_DCMD_ERROR;
    }
    stl_le_p(response + CYW43455_SDPCM_HEADER_LEN + 8, flags);
    stl_le_p(response + CYW43455_SDPCM_HEADER_LEN + 12, status);

    if (response_queued) {
        s->f2_rx_pending_len = QEMU_ALIGN_UP(response_len, 4);
        trace_cyw43455_sdio_control_queued(s->f2_rx_pending_len);
    } else {
        s->f2_rx_len = QEMU_ALIGN_UP(response_len, 4);
        s->f2_rx_pos = 0;
        s->sdio_intstatus |= CYW43455_I_HMB_FRAME_IND;
    }
    trace_cyw43455_sdio_control(command, set,
                                extract32(flags, 16, 16), data_len);
    if (iovar[0]) {
        trace_cyw43455_sdio_iovar(iovar, set,
                                  extract32(flags, 16, 16), status);
    }
    if (status == 0 && !s->explicit_role && !set &&
        command == CYW43455_C_GET_VAR &&
        !strcmp(iovar, "arpoe")) {
        /* This is the final probe in the synthetic startup profile. */
        s->rx_data_enabled = true;
    }
    if (!set && command == CYW43455_C_GET_VAR &&
        !strcmp(iovar, "toe_ol") && s->link_event_sent) {
        s->link_event_sent = false;
        trace_cyw43455_sdio_link_rearmed();
    }
    if (s->auto_associate && !s->explicit_association &&
        !s->explicit_role && set &&
        command == CYW43455_C_SET_PROMISC &&
        status == 0 && !s->link_event_sent && !s->link_event_pending &&
        !qemu_get_queue(s->nic)->link_down) {
        /* Netdev open has completed before this asynchronous filter update. */
        s->link_event_up = true;
        s->association_requested = true;
        s->link_event_pending = true;
    }
    if (!response_queued) {
        cyw43455_sdio_update_irq(s);
    }
}

static uint32_t cyw43455_sdio_backplane_window(CYW43455SDIOState *s)
{
    uint8_t *regs = s->regs[1];

    return (uint32_t)regs[CYW43455_SBADDRLOW] << 8 |
           (uint32_t)regs[CYW43455_SBADDRMID] << 16 |
           (uint32_t)regs[CYW43455_SBADDRHIGH] << 24;
}

static uint32_t cyw43455_sdio_erom_word(unsigned int index)
{
    unsigned int core_index = index / 5;
    unsigned int descriptor = index % 5;
    const CYW43455CoreInfo *core;

    if (core_index == ARRAY_SIZE(cyw43455_sdio_cores)) {
        return descriptor == 0 ? CYW43455_DMP_EOT : 0;
    }
    if (core_index > ARRAY_SIZE(cyw43455_sdio_cores)) {
        return 0;
    }

    core = &cyw43455_sdio_cores[core_index];
    switch (descriptor) {
    case 0:
        return (core->id << 8) | CYW43455_DMP_COMPONENT;
    case 1:
        return (core->revision << 24) | CYW43455_DMP_NUM_MWRAP |
               CYW43455_DMP_COMPONENT;
    case 2:
        return CYW43455_DMP_MASTER_PORT;
    case 3:
        return core->base | CYW43455_DMP_ADDRESS;
    case 4:
        return core->wrapper | CYW43455_DMP_MWRAP |
               CYW43455_DMP_ADDRESS;
    default:
        g_assert_not_reached();
    }
}

static uint32_t cyw43455_sdio_backplane_readl(CYW43455SDIOState *s,
                                               uint32_t address)
{
    unsigned int i;
    uint32_t sdio_offset;

    address &= ~3U;
    if (address == CYW43455_ENUM_BASE) {
        return (CYW43455_CHIP_ID & ~0xf0000U) |
               ((uint32_t)cyw43455_sdio_chip_revision(s) << 16);
    }
    if (address == CYW43455_EROM_POINTER) {
        return CYW43455_EROM_BASE;
    }
    if (address >= CYW43455_EROM_BASE &&
        address <= CYW43455_EROM_BASE +
                   ARRAY_SIZE(cyw43455_sdio_cores) * 5 * sizeof(uint32_t)) {
        return cyw43455_sdio_erom_word((address - CYW43455_EROM_BASE) / 4);
    }

    for (i = 0; i < ARRAY_SIZE(cyw43455_sdio_cores); i++) {
        if (address == cyw43455_sdio_cores[i].wrapper +
                       CYW43455_BCMA_IOCTL) {
            return s->wrapper_ioctl[i];
        }
        if (address == cyw43455_sdio_cores[i].wrapper +
                       CYW43455_BCMA_RESET_CTL) {
            return s->wrapper_reset[i];
        }
    }

    if (address == cyw43455_sdio_cores[2].base + CYW43455_ARMCR4_CAP) {
        /* Two 256 KiB banks and one 128 KiB bank. */
        return 0x12;
    }
    if (address == cyw43455_sdio_cores[2].base + CYW43455_ARMCR4_BANKIDX) {
        return s->armcr4_bankidx;
    }
    if (address == cyw43455_sdio_cores[2].base + CYW43455_ARMCR4_BANKINFO) {
        return s->armcr4_bankidx < CYW43455_ARMCR4_BANK_COUNT - 1 ?
               0x1f : 0x0f;
    }

    sdio_offset = address -
                  cyw43455_sdio_cores[CYW43455_SDIO_CORE_INDEX].base;
    switch (sdio_offset) {
    case CYW43455_SDIO_INTSTATUS:
        return s->sdio_intstatus;
    case CYW43455_SDIO_HOSTINTMASK:
        return s->sdio_hostintmask;
    case CYW43455_SDIO_TOSBMAILBOX:
        return s->sdio_tosbmailbox;
    case CYW43455_SDIO_TOHOSTMAILBOX:
        return s->sdio_tohostmailbox;
    case CYW43455_SDIO_TOSBMAILBOXDATA:
        return s->sdio_tosbmailboxdata;
    case CYW43455_SDIO_TOHOSTMAILBOXDATA:
        return s->sdio_tohostmailboxdata;
    default:
        break;
    }

    return 0;
}

static void cyw43455_sdio_backplane_writel(CYW43455SDIOState *s,
                                            uint32_t address, uint32_t value)
{
    unsigned int i;
    uint32_t sdio_offset;

    address &= ~3U;
    for (i = 0; i < ARRAY_SIZE(cyw43455_sdio_cores); i++) {
        if (address == cyw43455_sdio_cores[i].wrapper +
                       CYW43455_BCMA_IOCTL) {
            s->wrapper_ioctl[i] = value;
            return;
        }
        if (address == cyw43455_sdio_cores[i].wrapper +
                       CYW43455_BCMA_RESET_CTL) {
            s->wrapper_reset[i] = value;
            return;
        }
    }

    if (address == cyw43455_sdio_cores[2].base + CYW43455_ARMCR4_BANKIDX) {
        s->armcr4_bankidx = value;
        return;
    }

    sdio_offset = address -
                  cyw43455_sdio_cores[CYW43455_SDIO_CORE_INDEX].base;
    switch (sdio_offset) {
    case CYW43455_SDIO_INTSTATUS:
        cyw43455_sdio_clear_intstatus(s, value);
        break;
    case CYW43455_SDIO_HOSTINTMASK:
        s->sdio_hostintmask = value;
        cyw43455_sdio_update_irq(s);
        break;
    case CYW43455_SDIO_TOSBMAILBOX:
        s->sdio_tosbmailbox = value;
        break;
    case CYW43455_SDIO_TOHOSTMAILBOX:
        s->sdio_tohostmailbox = value;
        break;
    case CYW43455_SDIO_TOSBMAILBOXDATA:
        s->sdio_tosbmailboxdata = value;
        break;
    case CYW43455_SDIO_TOHOSTMAILBOXDATA:
        s->sdio_tohostmailboxdata = value;
        break;
    default:
        break;
    }
}

static uint8_t cyw43455_sdio_backplane_readb(CYW43455SDIOState *s,
                                              uint32_t address)
{
    return cyw43455_sdio_backplane_readl(s, address) >>
           ((address & 3) * 8);
}

static void cyw43455_sdio_backplane_writeb(CYW43455SDIOState *s,
                                            uint32_t address, uint8_t value)
{
    uint32_t word_address = address & ~3U;
    unsigned int shift = (address & 3) * 8;
    uint32_t word = cyw43455_sdio_backplane_readl(s, word_address);

    if (word_address ==
        cyw43455_sdio_cores[CYW43455_SDIO_CORE_INDEX].base +
        CYW43455_SDIO_INTSTATUS) {
        cyw43455_sdio_clear_intstatus(s, (uint32_t)value << shift);
        return;
    }

    word &= ~(0xffU << shift);
    word |= (uint32_t)value << shift;
    cyw43455_sdio_backplane_writel(s, word_address, word);
}

static void cyw43455_sdio_set_cis_pointer(CYW43455SDIOState *s,
                                          unsigned int function,
                                          uint32_t pointer)
{
    uint32_t base = function * SDIO_FBR_SIZE + SDIO_FBR_CIS_PTR;

    s->regs[0][base] = pointer;
    s->regs[0][base + 1] = pointer >> 8;
    s->regs[0][base + 2] = pointer >> 16;
}

static uint32_t cyw43455_sdio_add_manfid(CYW43455SDIOState *s,
                                         uint32_t offset)
{
    uint8_t *regs = s->regs[0];
    uint16_t device_id = cyw43455_sdio_device_id(s);

    regs[offset++] = SDIO_CISTPL_MANFID;
    regs[offset++] = 4;
    regs[offset++] = CYW43455_SDIO_VENDOR_ID & 0xff;
    regs[offset++] = CYW43455_SDIO_VENDOR_ID >> 8;
    regs[offset++] = device_id & 0xff;
    regs[offset++] = device_id >> 8;
    return offset;
}

static void cyw43455_sdio_add_function_cis(CYW43455SDIOState *s,
                                           uint32_t offset,
                                           uint16_t block_size)
{
    uint8_t *regs = s->regs[0];
    uint32_t tuple;

    offset = cyw43455_sdio_add_manfid(s, offset);
    regs[offset++] = SDIO_CISTPL_FUNCID;
    regs[offset++] = 2;
    regs[offset++] = 0x0c; /* SDIO function */
    regs[offset++] = 0x00;

    regs[offset++] = SDIO_CISTPL_FUNCE;
    regs[offset++] = 42;
    tuple = offset;
    memset(&regs[tuple], 0, 42);
    regs[tuple] = 0x01; /* function-extension tuple */
    regs[tuple + 12] = block_size;
    regs[tuple + 13] = block_size >> 8;
    regs[tuple + 28] = 100; /* 100 * 10 ms enable timeout */
    offset += 42;
    regs[offset] = SDIO_CISTPL_END;
}

static void cyw43455_sdio_init_function3(CYW43455SDIOState *s)
{
    uint8_t *regs = s->regs[0];
    uint32_t base = 3 * SDIO_FBR_SIZE;

    regs[base + SDIO_FBR_STD_IF] = 0x02;
    cyw43455_sdio_set_cis_pointer(s, 3, CYW43455_CIS_FUNC3);
    regs[base + SDIO_FBR_BLOCK_SIZE] = 0;
    regs[base + SDIO_FBR_BLOCK_SIZE + 1] = 2;
    cyw43455_sdio_add_function_cis(s, CYW43455_CIS_FUNC3, 512);
}

static void cyw43455_sdio_init_registers(CYW43455SDIOState *s)
{
    uint8_t *regs = s->regs[0];
    uint32_t offset;

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->f3_regs, 0, sizeof(s->f3_regs));

    /* CCCR 1.20, SDIO 2.00, SD Physical Layer 2.00. */
    regs[SDIO_CCCR_REV] = 0x32;
    regs[SDIO_CCCR_SD_REV] = 0x02;
    regs[SDIO_CCCR_CAPS] = 0x03; /* direct-during-transfer, multi-block */
    regs[SDIO_CCCR_POWER] = 0x01; /* high-power mode supported */
    regs[SDIO_CCCR_SPEED] = 0x01; /* high-speed mode supported */

    cyw43455_sdio_set_cis_pointer(s, 0, CYW43455_CIS_COMMON);
    cyw43455_sdio_set_cis_pointer(s, 1, CYW43455_CIS_FUNC1);
    cyw43455_sdio_set_cis_pointer(s, 2, CYW43455_CIS_FUNC2);

    /* Function zero and function one use 64-byte blocks initially. */
    regs[SDIO_FBR_BLOCK_SIZE] = 64;
    regs[SDIO_FBR_SIZE + SDIO_FBR_BLOCK_SIZE] = 64;
    /* Function two is the frame transport and starts at 512 bytes. */
    regs[2 * SDIO_FBR_SIZE + SDIO_FBR_BLOCK_SIZE] = 0;
    regs[2 * SDIO_FBR_SIZE + SDIO_FBR_BLOCK_SIZE + 1] = 2;

    offset = cyw43455_sdio_add_manfid(s, CYW43455_CIS_COMMON);
    regs[offset++] = SDIO_CISTPL_FUNCE;
    regs[offset++] = 4;
    regs[offset++] = 0x00; /* common-extension tuple */
    regs[offset++] = 0x00; /* 512-byte maximum block size */
    regs[offset++] = 0x02;
    regs[offset++] = 0x32; /* 25 Mbit/s encoded transfer speed */
    regs[offset] = SDIO_CISTPL_END;

    cyw43455_sdio_add_function_cis(s, CYW43455_CIS_FUNC1, 512);
    cyw43455_sdio_add_function_cis(s, CYW43455_CIS_FUNC2, 512);
    cyw43455_sdio_init_function3(s);
}

static void cyw43455_sdio_reset_ap_state(CYW43455SDIOState *s)
{
    s->explicit_role = false;
    s->ap_mode = false;
    s->ap_started = false;
    s->ap_peer_associated = false;
    s->ap_channel_configured = false;
    s->ap_hidden = false;
    s->infra = true;
    s->regulatory = false;
    s->ap_beacon_period = CYW43455_BSS_BEACON_PERIOD;
    s->ap_dtim_period = CYW43455_BSS_DTIM_PERIOD;
    s->ap_ssid_len = 0;
    memset(s->ap_ssid, 0, sizeof(s->ap_ssid));
    s->ap_events = 0;
}

static void cyw43455_sdio_reset_state(CYW43455SDIOState *s)
{
    SDState *sd = &s->parent_obj;

    if (s->explicit_role && s->nic) {
        qemu_purge_queued_packets(qemu_get_queue(s->nic));
    }
    timer_del(s->tx_credit_timer);
    timer_del(s->rx_glom_timer);
    timer_del(s->fws_timer);
    if (s->fws_migration_blocker) {
        migrate_del_blocker(&s->fws_migration_blocker);
    }
    s->fws_negotiated = false;
    s->fws_map_sent = false;
    s->fws_bcmc_sent = false;
    s->fws_if_open = true;
    s->fws_if_pending = 0;
    s->fws_if_reopen_pending = false;
    s->fws_status_head = 0;
    s->fws_status_count = 0;
    s->fws_retiring_count = 0;
    memset(s->fws_retiring_tags, 0, sizeof(s->fws_retiring_tags));
    s->fws_map_due_ms = -1;
    s->fws_credit_due_ms = -1;
    memset(s->fws_event_mask, 0, sizeof(s->fws_event_mask));
    s->fws_ac_available = 0;
    s->fws_bcmc_available = 0;
    memset(s->fws_credit_pending, 0, sizeof(s->fws_credit_pending));
    memset(s->fws_hanger_active, 0, sizeof(s->fws_hanger_active));
    memset(s->fws_status, 0, sizeof(s->fws_status));
    cyw43455_sdio_init_registers(s);
    memset(s->wrapper_ioctl, 0, sizeof(s->wrapper_ioctl));
    memset(s->wrapper_reset, 0, sizeof(s->wrapper_reset));
    s->armcr4_bankidx = 0;
    s->sdio_intstatus = 0;
    s->sdio_hostintmask = 0;
    s->sdio_tosbmailbox = 0;
    s->sdio_tohostmailbox = 0;
    s->sdio_tosbmailboxdata = 0;
    s->sdio_tohostmailboxdata = 0;
    memset(s->f2_tx_frame, 0, sizeof(s->f2_tx_frame));
    memset(s->f2_rx_frame, 0, sizeof(s->f2_rx_frame));
    memset(s->f2_rx_pending_frame, 0, sizeof(s->f2_rx_pending_frame));
    s->f2_tx_len = 0;
    s->f2_rx_len = 0;
    s->f2_rx_pos = 0;
    s->f2_rx_pending_len = 0;
    s->f2_rx_seq = 0;
    s->rx_glom_count = 0;
    s->rx_glom_align = 4;
    s->rx_glom_len = 0;
    s->rx_glom_ready = false;
    s->rx_glom_super_pending = false;
    memset(s->rx_glom_frame, 0, sizeof(s->rx_glom_frame));
    memset(s->rx_glom_lengths, 0, sizeof(s->rx_glom_lengths));
    s->tx_credit_seq = 0;
    s->tx_credit_limit = s->tx_credit_window;
    s->tx_credit_pending = false;
    s->tx_credit_update_pending = false;
    s->rx_data_enabled = false;
    s->link_event_pending = false;
    s->link_event_sent = false;
    s->link_event_up = true;
    s->firmware_connected = false;
    s->backend_link_events_armed = false;
    cyw43455_sdio_reset_ap_state(s);
    s->association_requested = false;
    s->link_ssid_pending = false;
    s->escan_event_phase = CYW43455_ESCAN_EVENT_NONE;
    s->escan_sync_id = 0;
    s->chanspec = s->ap_chanspec;
    s->survey_duration_ms = 0;
    s->survey_active = false;
    memcpy(s->current_mac, s->conf.macaddr.a, ETH_ALEN);
    if (s->nic) {
        qemu_format_nic_info_str(qemu_get_queue(s->nic), s->current_mac);
    }
    memset(s->multicast, 0, sizeof(s->multicast));
    s->multicast_count = 0;
    memset(s->arp_hostip, 0, sizeof(s->arp_hostip));
    s->arp_hostip_count = 0;
    s->arp_mode = 0;
    memset(s->nd_hostip, 0, sizeof(s->nd_hostip));
    s->nd_hostip_count = 0;
    s->pm2_sleep_ret = 0;
    s->power_mode = CYW43455_PM_OFF;
    s->obss_coex = CYW43455_OBSS_COEX_OFF;
    s->qtxpower = CYW43455_QTXPOWER_DEFAULT_QDBM;
    s->mfp_mode = CYW43455_MFP_NONE;
    cyw43455_sdio_pfn_clear(s);
    memset(s->pfn_scan_mac, 0, sizeof(s->pfn_scan_mac));
    s->pfn_scan_mac_flags = 0;
    s->pfn_enabled = false;
    s->tdls_enabled = false;
    s->roam_off = false;
    s->bcn_timeout = 0;
    s->roam_trigger = 0;
    s->roam_delta = 0;
    s->tx_bytes = 0;
    s->rx_bytes = 0;
    s->tx_multicast_bytes = 0;
    s->rx_multicast_bytes = 0;
    s->tx_packets = 0;
    s->rx_packets = 0;
    s->tx_multicast_packets = 0;
    s->rx_multicast_packets = 0;
    s->association_time_ms = -1;
    s->arp_offload = false;
    s->nd_offload = false;
    s->allmulti = false;
    s->promiscuous = false;
    s->transfer_addr = 0;
    s->transfer_remaining = 0;
    s->transfer_func = 0;
    s->transfer_write = false;
    s->transfer_increment = false;
    s->selected = false;
    sd->dat_lines = 0xf;
    sd->cmd_line = true;
    cyw43455_sdio_update_irq(s);
}

static uint8_t cyw43455_sdio_read_reg(CYW43455SDIOState *s,
                                      unsigned int function,
                                      uint32_t address)
{
    if (function == 1 && address >= CYW43455_SB_ACCESS_START &&
        address < CYW43455_SB_ACCESS_END) {
        uint32_t backplane_address = cyw43455_sdio_backplane_window(s) |
                                     (address & CYW43455_SB_OFFSET_MASK);

        return cyw43455_sdio_backplane_readb(s, backplane_address);
    }

    return cyw43455_sdio_func_regs(s, function)[address];
}

static void cyw43455_sdio_write_reg(CYW43455SDIOState *s,
                                    unsigned int function,
                                    uint32_t address, uint8_t value)
{
    uint8_t *regs = cyw43455_sdio_func_regs(s, function);

    if (function != 0) {
        if (function == 1 && address >= CYW43455_SB_ACCESS_START &&
            address < CYW43455_SB_ACCESS_END) {
            uint32_t backplane_address = cyw43455_sdio_backplane_window(s) |
                                         (address & CYW43455_SB_OFFSET_MASK);

            cyw43455_sdio_backplane_writeb(s, backplane_address, value);
            return;
        }
        if (function == 1 && address == CYW43455_CHIPCLKCSR) {
            /*
             * The clock requests complete immediately in this transport
             * model.  Availability bits are read-only status, rather than
             * ordinary bytes that the guest can overwrite.
             */
            regs[address] = value & CYW43455_CLKCSR_CONTROL_MASK;
            if (value & (CYW43455_ALP_AVAIL_REQ | CYW43455_FORCE_ALP)) {
                regs[address] |= CYW43455_ALP_AVAIL;
            }
            if (value & (CYW43455_HT_AVAIL_REQ | CYW43455_FORCE_HT)) {
                regs[address] |= CYW43455_ALP_AVAIL | CYW43455_HT_AVAIL;
            }
            return;
        }
        regs[address] = value;
        return;
    }

    switch (address) {
    case SDIO_CCCR_IO_ENABLE:
        value &= CYW43455_SDIO_FUNC_MASK;
        regs[SDIO_CCCR_IO_ENABLE] = value;
        /* The model has no power-up latency, so functions are ready now. */
        regs[SDIO_CCCR_IO_READY] = value;
        cyw43455_sdio_flush_queued_packets(s);
        break;
    case SDIO_CCCR_INT_ENABLE:
        regs[address] = value & CYW43455_SDIO_INT_MASK;
        cyw43455_sdio_update_irq(s);
        break;
    case SDIO_CCCR_INT_PENDING:
        break;
    case SDIO_CCCR_ABORT:
        if (value & BIT(3)) {
            cyw43455_sdio_reset_state(s);
        } else {
            regs[address] = value & 0x7;
        }
        break;
    case SDIO_CCCR_BUS_IF:
        regs[address] = value & 0x83;
        break;
    case SDIO_CCCR_SPEED:
        regs[address] = 0x01 | (value & 0x02);
        break;
    case SDIO_CCCR_REV:
    case SDIO_CCCR_SD_REV:
    case SDIO_CCCR_IO_READY:
    case SDIO_CCCR_CAPS:
    case SDIO_CCCR_CIS_PTR ... SDIO_CCCR_CIS_PTR + 2:
    case SDIO_CCCR_POWER:
        break;
    default:
        /* Includes FBR block sizes and Broadcom's vendor CCCR range. */
        regs[address] = value;
        break;
    }
}

static size_t cyw43455_sdio_response(uint8_t *response, size_t response_size,
                                     uint32_t value)
{
    if (response_size < sizeof(value)) {
        return 0;
    }
    stl_be_p(response, value);
    return sizeof(value);
}

static size_t cyw43455_sdio_do_command(SDState *sd, SDRequest *request,
                                       uint8_t *response,
                                       size_t response_size)
{
    CYW43455SDIOState *s = CYW43455_SDIO(sd);
    uint32_t argument = request->arg;
    uint32_t address;
    uint32_t count;
    uint32_t status = 0;
    unsigned int function;
    uint8_t value = 0;

    switch (request->cmd) {
    case 0: /* GO_IDLE_STATE */
        cyw43455_sdio_reset_state(s);
        return 0;
    case 5: /* IO_SEND_OP_COND (R4) */
        status = SDIO_R4_READY |
                 (CYW43455_SDIO_NUM_FUNCS << SDIO_R4_NUM_FUNCS_SHIFT) |
                 CYW43455_SDIO_OCR;
        return cyw43455_sdio_response(response, response_size, status);
    case 3: /* SEND_RELATIVE_ADDR (R6) */
        status = CYW43455_SDIO_RCA << 16;
        return cyw43455_sdio_response(response, response_size, status);
    case 7: /* SELECT/DESELECT_CARD (R1) */
        s->selected = (argument >> 16) == CYW43455_SDIO_RCA;
        return cyw43455_sdio_response(response, response_size, 0);
    case 13: /* SEND_STATUS (R1) */
        return cyw43455_sdio_response(response, response_size, 0);
    case 52: /* IO_RW_DIRECT (R5) */
        function = extract32(argument, SDIO_CMD52_FUNC_SHIFT, 3);
        address = extract32(argument, SDIO_CMD52_ADDR_SHIFT, 17);
        if (function > CYW43455_SDIO_NUM_FUNCS) {
            status = SDIO_R5_FUNCTION_NUMBER;
        } else if (address >= CYW43455_SDIO_ADDR_SPACE_SIZE) {
            status = SDIO_R5_OUT_OF_RANGE;
        } else {
            if (argument & SDIO_CMD52_WRITE) {
                cyw43455_sdio_write_reg(s, function, address, argument);
                value = argument & SDIO_CMD52_RAW ?
                        cyw43455_sdio_read_reg(s, function, address) :
                        extract32(argument, 0, 8);
            } else {
                value = cyw43455_sdio_read_reg(s, function, address);
            }
            status = value;
        }
        return cyw43455_sdio_response(response, response_size, status);
    case 53: /* IO_RW_EXTENDED (R5) */
        function = extract32(argument, SDIO_CMD53_FUNC_SHIFT, 3);
        address = extract32(argument, SDIO_CMD53_ADDR_SHIFT, 17);
        if (function > CYW43455_SDIO_NUM_FUNCS) {
            status = SDIO_R5_FUNCTION_NUMBER;
        } else if (address >= CYW43455_SDIO_ADDR_SPACE_SIZE) {
            status = SDIO_R5_OUT_OF_RANGE;
        } else {
            count = extract32(argument, 0, 9);
            count = count ? count : 512;
            if (argument & SDIO_CMD53_BLOCK_MODE) {
                uint32_t base = function * SDIO_FBR_SIZE;
                uint32_t block_size;

                block_size = s->regs[0][base + SDIO_FBR_BLOCK_SIZE];
                block_size |= s->regs[0][base + SDIO_FBR_BLOCK_SIZE + 1]
                              << 8;
                if (block_size == 0) {
                    block_size = 512;
                }
                count *= block_size;
            }
            s->transfer_func = function;
            s->transfer_addr = address;
            s->transfer_remaining = count;
            s->transfer_write = argument & SDIO_CMD53_WRITE;
            s->transfer_increment = argument & SDIO_CMD53_INCREMENT;
            if (function == 2 && !s->transfer_write) {
                uint32_t first_word = UINT32_MAX;

                if (s->f2_rx_pos <= s->f2_rx_len &&
                    s->f2_rx_len - s->f2_rx_pos >= sizeof(first_word)) {
                    first_word = ldl_le_p(s->f2_rx_frame + s->f2_rx_pos);
                }
                trace_cyw43455_sdio_rx_read(count, s->f2_rx_len,
                                            s->f2_rx_pos, first_word);
            }
        }
        return cyw43455_sdio_response(response, response_size, status);
    default:
        /* Unsupported commands time out, as they do on an SDIO-only card. */
        return 0;
    }
}

static size_t cyw43455_sdio_read_data(SDState *sd, void *buffer, size_t length)
{
    CYW43455SDIOState *s = CYW43455_SDIO(sd);
    uint8_t *data = buffer;
    size_t i;

    for (i = 0; i < length; i++) {
        if (!s->transfer_write && s->transfer_remaining) {
            if (s->transfer_func == 2) {
                if (s->f2_rx_len) {
                    data[i] = s->f2_rx_pos < s->f2_rx_len ?
                              s->f2_rx_frame[s->f2_rx_pos] : 0;
                    s->f2_rx_pos++;
                } else {
                    /* An empty SDPCM FIFO is the all-zero end marker. */
                    data[i] = 0;
                }
            } else {
                data[i] = cyw43455_sdio_read_reg(s, s->transfer_func,
                                                 s->transfer_addr);
            }
            s->transfer_remaining--;
            if (s->transfer_increment &&
                s->transfer_addr + 1 < CYW43455_SDIO_ADDR_SPACE_SIZE) {
                s->transfer_addr++;
            }
        } else {
            data[i] = 0xff;
        }
    }
    if (!s->transfer_write && s->transfer_func == 2 &&
        !s->transfer_remaining && s->f2_rx_len &&
        s->f2_rx_pos >= s->f2_rx_len) {
        cyw43455_sdio_fws_status_read(s);
        s->f2_rx_len = 0;
        s->f2_rx_pos = 0;
        cyw43455_sdio_advance_rx(s);
    }
    return length;
}

static size_t cyw43455_sdio_write_data(SDState *sd, const void *buffer,
                                       size_t length)
{
    CYW43455SDIOState *s = CYW43455_SDIO(sd);
    const uint8_t *data = buffer;
    size_t i;

    for (i = 0; i < length; i++) {
        if (s->transfer_write && s->transfer_remaining) {
            if (s->transfer_func == 2 &&
                s->f2_tx_len < sizeof(s->f2_tx_frame)) {
                s->f2_tx_frame[s->f2_tx_len++] = data[i];
            }
            cyw43455_sdio_write_reg(s, s->transfer_func,
                                    s->transfer_addr, data[i]);
            s->transfer_remaining--;
            if (s->transfer_increment &&
                s->transfer_addr + 1 < CYW43455_SDIO_ADDR_SPACE_SIZE) {
                s->transfer_addr++;
            }
            if (s->transfer_func == 2 && !s->transfer_remaining) {
                /* Linux may split a frame into block and byte CMD53 writes. */
                if (s->f2_tx_len >= CYW43455_SDPCM_HW_HEADER_LEN) {
                    uint16_t frame_len = lduw_le_p(s->f2_tx_frame);
                    uint16_t checksum = lduw_le_p(s->f2_tx_frame + 2);

                    if ((uint16_t)~frame_len != checksum ||
                        frame_len < CYW43455_SDPCM_HEADER_LEN ||
                        frame_len > sizeof(s->f2_tx_frame)) {
                        s->f2_tx_len = 0;
                    } else if (frame_len <= s->f2_tx_len) {
                        cyw43455_sdio_process_tx_frame(s);
                        s->f2_tx_len = 0;
                    }
                }
            }
        }
    }
    return length;
}

static bool cyw43455_sdio_receive_ready(SDState *sd)
{
    CYW43455SDIOState *s = CYW43455_SDIO(sd);

    return s->transfer_write && s->transfer_remaining;
}

static bool cyw43455_sdio_data_ready(SDState *sd)
{
    CYW43455SDIOState *s = CYW43455_SDIO(sd);

    return !s->transfer_write && s->transfer_remaining;
}

static bool cyw43455_sdio_get_inserted(SDState *sd)
{
    return true;
}

static bool cyw43455_sdio_get_readonly(SDState *sd)
{
    return false;
}

static uint8_t cyw43455_sdio_get_dat_lines(SDState *sd)
{
    CYW43455SDIOState *s = CYW43455_SDIO(sd);

    /* In-band SDIO interrupts pull DAT1 low. */
    return cyw43455_sdio_card_irq_level(s) ? 0xd : 0xf;
}

static bool cyw43455_sdio_get_cmd_line(SDState *sd)
{
    return true;
}

static void cyw43455_sdio_set_voltage(SDState *sd, uint16_t millivolts)
{
}

static void cyw43455_sdio_reset(DeviceState *device)
{
    cyw43455_sdio_reset_state(CYW43455_SDIO(device));
}

static bool cyw43455_sdio_can_receive(NetClientState *nc)
{
    CYW43455SDIOState *s = qemu_get_nic_opaque(nc);

    /* Drop disconnected input instead of replaying it after association. */
    return !cyw43455_sdio_link_available(s) ||
           cyw43455_sdio_rx_ready(s) ||
           (s->arp_offload &&
            (s->arp_mode & CYW43455_ARP_OL_AGENT) &&
            s->arp_hostip_count) ||
           (s->nd_offload && s->nd_hostip_count);
}

static bool cyw43455_sdio_arp_offload(CYW43455SDIOState *s,
                                      const uint8_t *buffer, size_t size)
{
    const uint8_t *arp;
    const uint8_t *target_ip;
    uint8_t reply[60] = { };
    uint8_t *reply_arp = reply + ETH_HLEN;
    unsigned int i;

    if (!s->arp_offload || !(s->arp_mode & CYW43455_ARP_OL_AGENT) ||
        size < ETH_HLEN + 28 || lduw_be_p(buffer + 12) != ETH_P_ARP) {
        return false;
    }

    arp = buffer + ETH_HLEN;
    if (lduw_be_p(arp) != 1 || lduw_be_p(arp + 2) != ETH_P_IP ||
        arp[4] != ETH_ALEN || arp[5] != sizeof(uint32_t) ||
        lduw_be_p(arp + 6) != 1) {
        return false;
    }

    target_ip = arp + 24;
    for (i = 0; i < s->arp_hostip_count; i++) {
        if (!memcmp(target_ip, s->arp_hostip[i], sizeof(uint32_t))) {
            break;
        }
    }
    if (i == s->arp_hostip_count) {
        return false;
    }

    memcpy(reply, arp + 8, ETH_ALEN);
    memcpy(reply + ETH_ALEN, s->current_mac, ETH_ALEN);
    stw_be_p(reply + 2 * ETH_ALEN, ETH_P_ARP);
    stw_be_p(reply_arp, 1);
    stw_be_p(reply_arp + 2, ETH_P_IP);
    reply_arp[4] = ETH_ALEN;
    reply_arp[5] = sizeof(uint32_t);
    stw_be_p(reply_arp + 6, 2);
    memcpy(reply_arp + 8, s->current_mac, ETH_ALEN);
    memcpy(reply_arp + 14, target_ip, sizeof(uint32_t));
    memcpy(reply_arp + 18, arp + 8, ETH_ALEN);
    memcpy(reply_arp + 24, arp + 14, sizeof(uint32_t));

    qemu_send_packet(qemu_get_queue(s->nic), reply, sizeof(reply));
    trace_cyw43455_sdio_arp_reply(target_ip[0], target_ip[1],
                                  target_ip[2], target_ip[3]);
    return true;
}

static uint16_t cyw43455_sdio_icmp6_checksum(const uint8_t *source,
                                             const uint8_t *destination,
                                             uint8_t *payload,
                                             uint32_t payload_len)
{
    uint8_t pseudo[40] = { };
    uint32_t sum;

    memcpy(pseudo, source, 16);
    memcpy(pseudo + 16, destination, 16);
    stl_be_p(pseudo + 32, payload_len);
    pseudo[39] = CYW43455_ICMP6_NEXT_HEADER;
    sum = net_checksum_add(sizeof(pseudo), pseudo);
    sum += net_checksum_add(payload_len, payload);
    return net_checksum_finish(sum);
}

static bool cyw43455_sdio_nd_offload(CYW43455SDIOState *s,
                                     const uint8_t *buffer, size_t size)
{
    static const uint8_t zero_ip[16];
    const uint8_t *ip6;
    const uint8_t *icmp6;
    const uint8_t *target_ip;
    uint8_t reply[ETH_HLEN + 40 + CYW43455_ND_PAYLOAD_LEN] = { };
    uint8_t *reply_ip6 = reply + ETH_HLEN;
    uint8_t *reply_icmp6 = reply_ip6 + 40;
    uint16_t payload_len;
    unsigned int i;

    if (!s->nd_offload ||
        size < ETH_HLEN + 40 + 24 ||
        lduw_be_p(buffer + 12) != ETH_P_IPV6) {
        return false;
    }

    ip6 = buffer + ETH_HLEN;
    payload_len = lduw_be_p(ip6 + 4);
    if ((ip6[0] >> 4) != 6 || !memcmp(ip6 + 8, zero_ip, sizeof(zero_ip)) ||
        ip6[6] != CYW43455_ICMP6_NEXT_HEADER || ip6[7] != 255 ||
        payload_len < 24 || size < ETH_HLEN + 40 + payload_len) {
        return false;
    }

    icmp6 = ip6 + 40;
    if (icmp6[0] != CYW43455_ICMP6_NS || icmp6[1] ||
        cyw43455_sdio_icmp6_checksum(ip6 + 8, ip6 + 24,
                                     (uint8_t *)icmp6, payload_len)) {
        return false;
    }

    target_ip = icmp6 + 8;
    for (i = 0; i < s->nd_hostip_count; i++) {
        if (!memcmp(target_ip, s->nd_hostip[i], sizeof(s->nd_hostip[0]))) {
            break;
        }
    }
    if (i == s->nd_hostip_count) {
        return false;
    }

    memcpy(reply, buffer + ETH_ALEN, ETH_ALEN);
    memcpy(reply + ETH_ALEN, s->current_mac, ETH_ALEN);
    stw_be_p(reply + 2 * ETH_ALEN, ETH_P_IPV6);
    stl_be_p(reply_ip6, 6U << 28);
    stw_be_p(reply_ip6 + 4, CYW43455_ND_PAYLOAD_LEN);
    reply_ip6[6] = CYW43455_ICMP6_NEXT_HEADER;
    reply_ip6[7] = 255;
    memcpy(reply_ip6 + 8, target_ip, 16);
    memcpy(reply_ip6 + 24, ip6 + 8, 16);

    reply_icmp6[0] = CYW43455_ICMP6_NA;
    stl_be_p(reply_icmp6 + 4, BIT(30) | BIT(29));
    memcpy(reply_icmp6 + 8, target_ip, 16);
    reply_icmp6[24] = 2; /* target link-layer address */
    reply_icmp6[25] = 1; /* option length in eight-byte units */
    memcpy(reply_icmp6 + 26, s->current_mac, ETH_ALEN);
    stw_be_p(reply_icmp6 + 2,
             cyw43455_sdio_icmp6_checksum(reply_ip6 + 8, reply_ip6 + 24,
                                          reply_icmp6,
                                          CYW43455_ND_PAYLOAD_LEN));

    qemu_send_packet(qemu_get_queue(s->nic), reply, sizeof(reply));
    trace_cyw43455_sdio_nd_reply(target_ip[12], target_ip[13],
                                 target_ip[14], target_ip[15]);
    return true;
}

static bool cyw43455_sdio_accept_packet(CYW43455SDIOState *s,
                                        const uint8_t *buffer, size_t size)
{
    static const uint8_t broadcast[ETH_ALEN] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    };
    const uint8_t *destination = buffer;
    unsigned int i;

    if (size < ETH_ALEN) {
        return false;
    }
    if (s->promiscuous ||
        !memcmp(destination, s->current_mac, ETH_ALEN) ||
        !memcmp(destination, broadcast, ETH_ALEN)) {
        return true;
    }
    if (!(destination[0] & 1)) {
        return false;
    }
    if (s->allmulti) {
        return true;
    }
    for (i = 0; i < s->multicast_count; i++) {
        if (!memcmp(destination, s->multicast[i], ETH_ALEN)) {
            return true;
        }
    }
    return false;
}

static void cyw43455_sdio_emit_rx_glom(CYW43455SDIOState *s)
{
    uint8_t *frame = s->f2_rx_frame;
    uint32_t head_len = QEMU_ALIGN_UP(CYW43455_SDPCM_HEADER_LEN,
                                     s->rx_glom_align);
    uint32_t offset = head_len;
    uint32_t descriptor_len = CYW43455_SDPCM_HEADER_LEN +
                              2 * s->rx_glom_count;
    unsigned int i;

    assert(s->rx_glom_count && s->rx_glom_ready);
    timer_del(s->rx_glom_timer);
    s->rx_glom_ready = false;
    if (s->rx_glom_count == 1) {
        /* Sparse traffic keeps the ordinary data-channel framing. */
        s->f2_rx_len = s->rx_glom_len - head_len;
        memcpy(frame, s->rx_glom_frame + head_len,
               s->f2_rx_len);
        frame[4] = s->f2_rx_seq++;
        frame[9] = s->tx_credit_limit;
        s->rx_glom_count = 0;
        s->rx_glom_len = 0;
    } else {
        memset(frame, 0, QEMU_ALIGN_UP(descriptor_len, 4));
        stw_le_p(frame, descriptor_len);
        stw_le_p(frame + 2, (uint16_t)~descriptor_len);
        frame[4] = s->f2_rx_seq++;
        frame[5] = CYW43455_SDPCM_GLOM_CHANNEL | 0x80;
        frame[7] = CYW43455_SDPCM_HEADER_LEN;
        frame[9] = s->tx_credit_limit;
        stw_le_p(s->rx_glom_frame, s->rx_glom_len);
        stw_le_p(s->rx_glom_frame + 2, (uint16_t)~s->rx_glom_len);
        s->rx_glom_frame[4] = s->f2_rx_seq;
        s->rx_glom_frame[5] = CYW43455_SDPCM_GLOM_CHANNEL;
        s->rx_glom_frame[7] = head_len;
        s->rx_glom_frame[9] = s->tx_credit_limit;
        for (i = 0; i < s->rx_glom_count; i++) {
            stw_le_p(frame + CYW43455_SDPCM_HEADER_LEN + 2 * i,
                     s->rx_glom_lengths[i]);
            s->rx_glom_frame[offset + 4] = s->f2_rx_seq++;
            s->rx_glom_frame[offset + 9] = s->tx_credit_limit;
            offset += s->rx_glom_lengths[i] -
                      (i == 0 ? head_len : 0);
        }
        s->f2_rx_len = QEMU_ALIGN_UP(descriptor_len, 4);
        s->rx_glom_super_pending = true;
        trace_cyw43455_sdio_rx_glom(s->rx_glom_count, s->rx_glom_len, true);
    }
    s->tx_credit_update_pending = false;
    s->f2_rx_pos = 0;
    s->sdio_intstatus |= CYW43455_I_HMB_FRAME_IND;
    cyw43455_sdio_update_irq(s);
}

static void cyw43455_sdio_rx_glom_timeout(void *opaque)
{
    CYW43455SDIOState *s = opaque;

    if (s->rx_glom_count) {
        s->rx_glom_ready = true;
        cyw43455_sdio_advance_rx(s);
    }
}

static ssize_t cyw43455_sdio_receive(NetClientState *nc,
                                     const uint8_t *buffer, size_t size)
{
    CYW43455SDIOState *s = qemu_get_nic_opaque(nc);
    uint8_t *frame = s->f2_rx_frame;
    size_t frame_len = CYW43455_SDPCM_HEADER_LEN +
                       CYW43455_BCDC_DATA_HEADER_LEN + size;
    size_t stored_len;

    if (!cyw43455_sdio_link_available(s)) {
        return size;
    }
    if (s->ap_mode && (size < ETH_HLEN ||
        memcmp(buffer + ETH_ALEN, cyw43455_sdio_peer_mac, ETH_ALEN))) {
        trace_cyw43455_sdio_ap_drop(false, 2, size);
        return size;
    }
    if (cyw43455_sdio_arp_offload(s, buffer, size)) {
        return size;
    }
    if (cyw43455_sdio_nd_offload(s, buffer, size)) {
        return size;
    }
    if (frame_len > sizeof(s->f2_rx_frame)) {
        trace_cyw43455_sdio_rx_dropped((uint32_t)size);
        return size;
    }
    if (!cyw43455_sdio_rx_ready(s)) {
        return 0;
    }
    if (!cyw43455_sdio_accept_packet(s, buffer, size)) {
        trace_cyw43455_sdio_rx_filtered((uint32_t)size,
                                        size >= ETH_ALEN && (buffer[0] & 1));
        return size;
    }

    stored_len = QEMU_ALIGN_UP(frame_len, sizeof(uint32_t));
    if (s->rx_glom_max_frames > 1) {
        uint32_t head_len = QEMU_ALIGN_UP(CYW43455_SDPCM_HEADER_LEN,
                                         s->rx_glom_align);
        uint32_t offset = s->rx_glom_count ? s->rx_glom_len :
                                           head_len;

        stored_len = QEMU_ALIGN_UP(frame_len, s->rx_glom_align);

        if (QEMU_ALIGN_UP(offset + stored_len, 512) >
            sizeof(s->rx_glom_frame)) {
            if (s->rx_glom_count) {
                s->rx_glom_ready = true;
                cyw43455_sdio_advance_rx(s);
                return 0;
            }
            /* An individual large packet falls back to ordinary framing. */
        } else {
            if (!s->rx_glom_count) {
                memset(s->rx_glom_frame, 0, sizeof(s->rx_glom_frame));
                timer_mod(s->rx_glom_timer,
                          qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                          s->rx_glom_delay_ms);
            }
            frame = s->rx_glom_frame + offset;
            s->rx_glom_lengths[s->rx_glom_count] = stored_len +
                (s->rx_glom_count == 0 ? head_len : 0);
            s->rx_glom_len = offset + stored_len;
            s->rx_glom_count++;
        }
    }
    memset(frame, 0, stored_len);
    stw_le_p(frame, frame_len);
    stw_le_p(frame + 2, (uint16_t)~frame_len);
    frame[4] = frame == s->f2_rx_frame ? s->f2_rx_seq++ : 0;
    frame[5] = CYW43455_SDPCM_DATA_CHANNEL;
    frame[7] = CYW43455_SDPCM_HEADER_LEN;
    frame[9] = s->tx_credit_limit;
    if (frame == s->f2_rx_frame) {
        s->tx_credit_update_pending = false;
    }
    frame[CYW43455_SDPCM_HEADER_LEN] =
        CYW43455_BCDC_PROTO_VER << 4;
    memcpy(frame + CYW43455_SDPCM_HEADER_LEN +
                   CYW43455_BCDC_DATA_HEADER_LEN,
           buffer, size);

    s->rx_packets++;
    s->rx_bytes += size;
    if (buffer[0] & 1) {
        s->rx_multicast_packets++;
        s->rx_multicast_bytes += size;
    }

    if (frame != s->f2_rx_frame) {
        if (s->rx_glom_count == s->rx_glom_max_frames) {
            s->rx_glom_ready = true;
            cyw43455_sdio_advance_rx(s);
        }
        return size;
    }
    s->f2_rx_len = stored_len;
    s->f2_rx_pos = 0;
    s->sdio_intstatus |= CYW43455_I_HMB_FRAME_IND;
    trace_cyw43455_sdio_rx_data((uint32_t)size, (uint32_t)frame_len,
                                frame[4], frame[9]);
    cyw43455_sdio_update_irq(s);
    return size;
}

static void cyw43455_sdio_link_status_changed(NetClientState *nc)
{
    CYW43455SDIOState *s = qemu_get_nic_opaque(nc);
    bool link_up = !nc->link_down;

    if (s->ap_mode) {
        if (!s->ap_started) {
            return;
        }
        if (!link_up) {
            s->ap_events &= ~CYW43455_AP_EVENT_ASSOC;
            if (s->ap_peer_associated) {
                s->ap_events |= CYW43455_AP_EVENT_DISASSOC;
            }
            s->ap_peer_associated = false;
            s->association_time_ms = -1;
            cyw43455_sdio_discard_uncommitted_rx(s);
        } else if (!s->ap_peer_associated) {
            s->ap_events |= CYW43455_AP_EVENT_ASSOC;
        }
        cyw43455_sdio_advance_rx(s);
        return;
    }
    if (!s->auto_associate || !s->backend_link_events_armed ||
        !s->association_requested) {
        return;
    }

    if (link_up == s->firmware_connected) {
        if (s->link_event_pending && s->link_event_up != link_up) {
            s->link_event_pending = false;
        }
        return;
    }

    s->link_event_up = link_up;
    if (!link_up && !s->rx_glom_super_pending) {
        timer_del(s->rx_glom_timer);
        s->rx_glom_count = 0;
        s->rx_glom_len = 0;
        s->rx_glom_ready = false;
    }
    s->link_ssid_pending = false;
    s->link_event_pending = true;
    cyw43455_sdio_advance_rx(s);
    if (link_up) {
        qemu_flush_queued_packets(nc);
    }
}

static NetClientInfo cyw43455_sdio_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = cyw43455_sdio_can_receive,
    .receive = cyw43455_sdio_receive,
    .link_status_changed = cyw43455_sdio_link_status_changed,
};

static void cyw43455_sdio_realize(DeviceState *device, Error **errp)
{
    static const MACAddr zero_mac;
    CYW43455SDIOState *s = CYW43455_SDIO(device);
    BusState *bus = qdev_get_parent_bus(device);
    uint8_t bandwidth_mhz;
    uint8_t control_channel;

    if (bus == NULL || !object_dynamic_cast(OBJECT(bus), TYPE_SD_BUS)) {
        error_setg(errp, "CYW43455 SDIO endpoint requires an SD bus");
        return;
    }
    if (s->hardware_profile > CYW43455_SDIO_PROFILE_PI5) {
        error_setg(errp, "hardware-profile must be 0 (Pi 400) or 1 (Pi 5)");
        return;
    }
    if (s->tx_credit_window < 2 ||
        s->tx_credit_window > CYW43455_TX_CREDIT_WINDOW_MAX) {
        error_setg(errp, "tx-credit-window must be between 2 and %u",
                   CYW43455_TX_CREDIT_WINDOW_MAX);
        return;
    }
    if (!s->rx_glom_max_frames ||
        s->rx_glom_max_frames > CYW43455_RX_GLOM_MAX ||
        s->rx_glom_delay_ms > 1000) {
        error_setg(errp, "rx-glom-max-frames must be 1..%u and "
                   "rx-glom-delay-ms must be 0..1000", CYW43455_RX_GLOM_MAX);
        return;
    }
    if (!s->fws_status_batch_max ||
        s->fws_status_batch_max > CYW43455_FWS_STATUS_BATCH_MAX) {
        error_setg(errp, "fws-status-batch-max must be 1..%u",
                   CYW43455_FWS_STATUS_BATCH_MAX);
        return;
    }
    if (!s->fws_credit_budget || s->fws_credit_budget > 8 ||
        s->fws_startup_delay_ms > 1000 || s->fws_status_delay_ms > 1000 ||
        s->fws_credit_delay_ms > 1000) {
        error_setg(errp, "fws-credit-budget must be 1..8 and FWS delays "
                   "must be 0..1000 milliseconds");
        return;
    }
    if ((s->ap_chanspec & CYW43455_CHANSPEC_BAND_MASK) ||
        !cyw43455_sdio_chanspec_info(s->ap_chanspec, &control_channel,
                                     &bandwidth_mhz)) {
        error_setg(errp, "invalid 2.4 GHz AP chanspec 0x%04x",
                   s->ap_chanspec);
        return;
    }

    s->tx_credit_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                      cyw43455_sdio_emit_tx_credit, s);
    s->rx_glom_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                    cyw43455_sdio_rx_glom_timeout, s);
    s->fws_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                cyw43455_sdio_fws_timeout, s);

    if (!memcmp(&s->conf.macaddr, &zero_mac, sizeof(zero_mac))) {
        memcpy(s->conf.macaddr.a, cyw43455_sdio_default_mac,
               sizeof(s->conf.macaddr.a));
    }
    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&cyw43455_sdio_net_info, &s->conf,
                          object_get_typename(OBJECT(device)), device->id,
                          &device->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);

    cyw43455_sdio_reset(device);
    sdbus_set_inserted(SD_BUS(bus), true);
    sdbus_set_readonly(SD_BUS(bus), false);
}

static void cyw43455_sdio_unrealize(DeviceState *device)
{
    CYW43455SDIOState *s = CYW43455_SDIO(device);
    BusState *bus = qdev_get_parent_bus(device);

    if (bus != NULL) {
        sdbus_set_card_irq(SD_BUS(bus), false);
        sdbus_set_inserted(SD_BUS(bus), false);
    }
    timer_free(s->tx_credit_timer);
    s->tx_credit_timer = NULL;
    timer_free(s->rx_glom_timer);
    s->rx_glom_timer = NULL;
    timer_free(s->fws_timer);
    s->fws_timer = NULL;
    s->fws_retiring_count = 0;
    memset(s->fws_retiring_tags, 0, sizeof(s->fws_retiring_tags));
    if (s->fws_migration_blocker) {
        migrate_del_blocker(&s->fws_migration_blocker);
    }
    qemu_del_nic(s->nic);
    s->nic = NULL;
}

static int cyw43455_sdio_post_load(void *opaque, int version_id)
{
    CYW43455SDIOState *s = opaque;
    uint8_t bandwidth_mhz;
    uint8_t control_channel;
    unsigned int i;

    if (s->fws_migration_blocker) {
        return -EINVAL;
    }

    if (version_id < 29 &&
        s->hardware_profile != CYW43455_SDIO_PROFILE_PI400) {
        return -EINVAL;
    }
    if (version_id < 30) {
        cyw43455_sdio_reset_ap_state(s);
        /* The old station model could not emit a five-GHz state stream. */
        if ((s->chanspec | s->ap_chanspec) & CYW43455_CHANSPEC_BAND_MASK) {
            return -EINVAL;
        }
    }
    if (version_id < 10) {
        cyw43455_sdio_init_function3(s);
    }
    if (version_id < 11) {
        s->link_event_up = true;
        s->firmware_connected = s->link_event_sent;
        s->backend_link_events_armed = s->link_event_sent;
    }
    if (version_id < 12) {
        memcpy(s->current_mac, s->conf.macaddr.a, ETH_ALEN);
    }
    if (version_id < 13) {
        memset(s->arp_hostip, 0, sizeof(s->arp_hostip));
        s->arp_hostip_count = 0;
        s->arp_mode = 0;
        s->arp_offload = false;
    }
    if (version_id < 14) {
        memset(s->nd_hostip, 0, sizeof(s->nd_hostip));
        s->nd_hostip_count = 0;
        s->nd_offload = false;
    }
    if (version_id < 15) {
        s->tx_bytes = 0;
        s->rx_bytes = 0;
        s->tx_multicast_bytes = 0;
        s->rx_multicast_bytes = 0;
        s->tx_packets = 0;
        s->rx_packets = 0;
        s->tx_multicast_packets = 0;
        s->rx_multicast_packets = 0;
        s->association_time_ms = s->firmware_connected ?
            qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) : -1;
    }
    if (version_id < 16) {
        s->escan_event_phase = CYW43455_ESCAN_EVENT_NONE;
        s->escan_sync_id = 0;
    }
    if (version_id < 17) {
        s->chanspec = CYW43455_CHANSPEC_2G_CH1_20MHZ;
        s->survey_duration_ms = 0;
        s->survey_active = false;
    }
    if (version_id < 19) {
        s->power_mode = CYW43455_PM_OFF;
    }
    if (version_id < 20) {
        s->obss_coex = CYW43455_OBSS_COEX_OFF;
    }
    if (version_id < 21) {
        s->qtxpower = CYW43455_QTXPOWER_DEFAULT_QDBM;
    }
    if (version_id < 22) {
        s->mfp_mode = CYW43455_MFP_NONE;
    }
    if (version_id < 23) {
        cyw43455_sdio_pfn_clear(s);
        memset(s->pfn_scan_mac, 0, sizeof(s->pfn_scan_mac));
        s->pfn_scan_mac_flags = 0;
        s->pfn_enabled = false;
    }
    if (version_id < 24) {
        s->tdls_enabled = false;
    }
    if (version_id < 25) {
        s->roam_off = false;
    }
    if (version_id < 26) {
        s->bcn_timeout = 0;
    }
    if (version_id < 27) {
        s->roam_trigger = 0;
        s->roam_delta = 0;
    }
    if (version_id < 28) {
        s->rx_glom_align = 4;
        /* Older streams retained the last completed TX frame in this buffer. */
        if (!s->transfer_write || s->transfer_func != 2 ||
            !s->transfer_remaining) {
            s->f2_tx_len = 0;
        }
        s->tx_credit_limit = s->tx_credit_seq + s->tx_credit_window;
        s->tx_credit_update_pending = false;
        s->association_requested = s->firmware_connected ||
                                   (s->link_event_pending && s->link_event_up);
        s->link_ssid_pending = false;
        s->rx_glom_count = 0;
        s->rx_glom_len = 0;
        s->rx_glom_ready = false;
        s->rx_glom_super_pending = false;
    }
    if (s->rx_glom_align < 4 || s->rx_glom_align > 128 ||
        (s->rx_glom_align & (s->rx_glom_align - 1)) ||
        s->rx_glom_count > s->rx_glom_max_frames ||
        s->rx_glom_count > CYW43455_RX_GLOM_MAX ||
        s->rx_glom_len > sizeof(s->rx_glom_frame) ||
        (s->rx_glom_count && s->rx_glom_len < CYW43455_SDPCM_HEADER_LEN) ||
        (!s->rx_glom_count && (s->rx_glom_ready || s->rx_glom_super_pending)) ||
        (s->rx_glom_ready && s->rx_glom_super_pending)) {
        return -EINVAL;
    }
    if (s->rx_glom_count) {
        uint32_t total = 0;

        for (i = 0; i < s->rx_glom_count; i++) {
            if (s->rx_glom_lengths[i] < CYW43455_SDPCM_HEADER_LEN +
                (i == 0 ? CYW43455_SDPCM_HEADER_LEN : 0) ||
                s->rx_glom_lengths[i] % s->rx_glom_align) {
                return -EINVAL;
            }
            total += s->rx_glom_lengths[i];
        }
        if (total != s->rx_glom_len) {
            return -EINVAL;
        }
    }
    if (s->multicast_count > CYW43455_MULTICAST_MAX) {
        return -EINVAL;
    }
    if (s->arp_hostip_count > CYW43455_ARP_HOSTIP_MAX) {
        return -EINVAL;
    }
    if (s->nd_hostip_count > CYW43455_ND_HOSTIP_MAX) {
        return -EINVAL;
    }
    if (s->escan_event_phase > CYW43455_ESCAN_EVENT_COMPLETE) {
        return -EINVAL;
    }
    if (s->pfn_channel_count > CYW43455_PNO_MAX_CHANNELS ||
        s->pfn_network_count > CYW43455_PNO_MAX_NETWORKS ||
        s->pfn_bucket_count > CYW43455_PNO_MAX_BUCKETS ||
        (s->pfn_scan_freq &&
         (s->pfn_scan_freq < CYW43455_PNO_MIN_PERIOD ||
          s->pfn_scan_freq > CYW43455_PNO_MAX_PERIOD)) ||
        (s->pfn_scan_mac_flags & ~CYW43455_PNO_MAC_FLAGS_MASK) ||
        (s->pfn_enabled &&
         (!s->pfn_scan_freq || !s->pfn_channel_count ||
          !s->pfn_network_count || !s->pfn_bucket_count))) {
        return -EINVAL;
    }
    for (i = 0; i < s->pfn_channel_count; i++) {
        if (s->pfn_channels[i] < CYW43455_CHANNEL_2G_MIN ||
            s->pfn_channels[i] > CYW43455_CHANNEL_2G_MAX) {
            return -EINVAL;
        }
    }
    for (i = 0; i < s->pfn_network_count; i++) {
        if (!s->pfn_ssid_len[i] ||
            s->pfn_ssid_len[i] > CYW43455_PNO_SSID_MAX_LEN ||
            (s->pfn_network_flags[i] & ~CYW43455_PNO_HIDDEN)) {
            return -EINVAL;
        }
    }
    if ((s->ap_chanspec & CYW43455_CHANSPEC_BAND_MASK) ||
        !cyw43455_sdio_chanspec_info(s->ap_chanspec, &control_channel,
                                     &bandwidth_mhz) ||
        !cyw43455_sdio_chanspec_info(s->chanspec, &control_channel,
                                     &bandwidth_mhz) ||
        s->power_mode > CYW43455_PM_FAST ||
        s->obss_coex < CYW43455_OBSS_COEX_AUTO ||
        s->obss_coex > CYW43455_OBSS_COEX_ON ||
        (s->qtxpower & ~(CYW43455_QTXPOWER_OVERRIDE |
                         CYW43455_QTXPOWER_MAX_QDBM)) ||
        s->mfp_mode > CYW43455_MFP_REQUIRED ||
        s->survey_duration_ms > 60000 ||
        (s->survey_active && !s->survey_duration_ms)) {
        return -EINVAL;
    }
    if (!s->infra || !s->ap_beacon_period || !s->ap_dtim_period ||
        s->ap_ssid_len > CYW43455_AP_SSID_MAX ||
        (s->ap_events & ~CYW43455_AP_EVENT_MASK) ||
        (s->ap_mode && !s->explicit_role) ||
        (s->ap_mode && (s->firmware_connected || s->link_event_pending ||
                       s->association_requested ||
                       s->backend_link_events_armed || s->mfp_mode)) ||
        (s->ap_channel_configured && !s->ap_mode) ||
        (s->ap_started != (s->ap_ssid_len != 0)) ||
        (s->ap_started && (!s->ap_mode || !s->rx_data_enabled ||
                          !s->ap_channel_configured ||
                          s->firmware_connected || s->link_event_pending ||
                          s->association_requested)) ||
        (s->ap_peer_associated && !s->ap_started) ||
        (s->ap_peer_associated && s->ap_events) ||
        ((s->ap_events & CYW43455_AP_EVENT_DOWN) && s->ap_started) ||
        ((s->ap_events & (CYW43455_AP_EVENT_UP | CYW43455_AP_EVENT_ASSOC)) &&
         !s->ap_started) ||
        (s->ap_events && !s->explicit_role) ||
        (s->ap_channel_configured &&
         (!cyw43455_sdio_chanspec_info(s->chanspec, &control_channel,
                                       &bandwidth_mhz) ||
          bandwidth_mhz != 20))) {
        return -EINVAL;
    }
    cyw43455_sdio_update_irq(s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->current_mac);
    cyw43455_sdio_advance_rx(s);
    return 0;
}

static int cyw43455_sdio_pre_save(void *opaque)
{
    CYW43455SDIOState *s = opaque;

    return s->fws_migration_blocker ? -EBUSY : 0;
}

static const VMStateDescription cyw43455_sdio_vmstate = {
    .name = TYPE_CYW43455_SDIO,
    .version_id = 30,
    .minimum_version_id = 1,
    .pre_save = cyw43455_sdio_pre_save,
    .post_load = cyw43455_sdio_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_2DARRAY(regs, CYW43455SDIOState,
                             CYW43455_SDIO_LEGACY_REG_BANKS,
                             CYW43455_SDIO_ADDR_SPACE_SIZE),
        VMSTATE_UINT32(transfer_addr, CYW43455SDIOState),
        VMSTATE_UINT32(transfer_remaining, CYW43455SDIOState),
        VMSTATE_UINT8(transfer_func, CYW43455SDIOState),
        VMSTATE_BOOL(transfer_write, CYW43455SDIOState),
        VMSTATE_BOOL(transfer_increment, CYW43455SDIOState),
        VMSTATE_BOOL(selected, CYW43455SDIOState),
        VMSTATE_UINT32_ARRAY_V(wrapper_ioctl, CYW43455SDIOState,
                               ARRAY_SIZE(cyw43455_sdio_cores), 2),
        VMSTATE_UINT32_ARRAY_V(wrapper_reset, CYW43455SDIOState,
                               ARRAY_SIZE(cyw43455_sdio_cores), 2),
        VMSTATE_UINT32_V(armcr4_bankidx, CYW43455SDIOState, 2),
        VMSTATE_UINT32_V(sdio_intstatus, CYW43455SDIOState, 3),
        VMSTATE_UINT32_V(sdio_hostintmask, CYW43455SDIOState, 3),
        VMSTATE_UINT32_V(sdio_tosbmailbox, CYW43455SDIOState, 3),
        VMSTATE_UINT32_V(sdio_tohostmailbox, CYW43455SDIOState, 3),
        VMSTATE_UINT32_V(sdio_tosbmailboxdata, CYW43455SDIOState, 3),
        VMSTATE_UINT32_V(sdio_tohostmailboxdata, CYW43455SDIOState, 3),
        VMSTATE_UINT8_ARRAY_V(f2_tx_frame, CYW43455SDIOState,
                              CYW43455_F2_FRAME_MAX, 3),
        VMSTATE_UINT8_ARRAY_V(f2_rx_frame, CYW43455SDIOState,
                              CYW43455_F2_FRAME_MAX, 3),
        VMSTATE_UINT32_V(f2_tx_len, CYW43455SDIOState, 3),
        VMSTATE_UINT32_V(f2_rx_len, CYW43455SDIOState, 3),
        VMSTATE_UINT32_V(f2_rx_pos, CYW43455SDIOState, 3),
        VMSTATE_UINT8_V(f2_rx_seq, CYW43455SDIOState, 3),
        VMSTATE_BUFFER_V(conf.macaddr.a, CYW43455SDIOState, 4),
        VMSTATE_UINT8_V(tx_credit_seq, CYW43455SDIOState, 5),
        VMSTATE_BOOL_V(tx_credit_pending, CYW43455SDIOState, 5),
        VMSTATE_TIMER_PTR_V(tx_credit_timer, CYW43455SDIOState, 5),
        VMSTATE_BOOL_V(rx_data_enabled, CYW43455SDIOState, 6),
        VMSTATE_UINT8_ARRAY_V(f2_rx_pending_frame, CYW43455SDIOState,
                              CYW43455_F2_FRAME_MAX, 7),
        VMSTATE_UINT32_V(f2_rx_pending_len, CYW43455SDIOState, 7),
        VMSTATE_BOOL_V(link_event_pending, CYW43455SDIOState, 8),
        VMSTATE_BOOL_V(link_event_sent, CYW43455SDIOState, 8),
        VMSTATE_UINT8_2DARRAY_V(multicast, CYW43455SDIOState,
                                CYW43455_MULTICAST_MAX, ETH_ALEN, 9),
        VMSTATE_UINT32_V(multicast_count, CYW43455SDIOState, 9),
        VMSTATE_UINT32_V(pm2_sleep_ret, CYW43455SDIOState, 9),
        VMSTATE_BOOL_V(allmulti, CYW43455SDIOState, 9),
        VMSTATE_BOOL_V(promiscuous, CYW43455SDIOState, 9),
        VMSTATE_UINT8_ARRAY_V(f3_regs, CYW43455SDIOState,
                              CYW43455_SDIO_ADDR_SPACE_SIZE, 10),
        VMSTATE_BOOL_V(link_event_up, CYW43455SDIOState, 11),
        VMSTATE_BOOL_V(firmware_connected, CYW43455SDIOState, 11),
        VMSTATE_BOOL_V(backend_link_events_armed, CYW43455SDIOState, 11),
        VMSTATE_UINT8_ARRAY_V(current_mac, CYW43455SDIOState, ETH_ALEN, 12),
        VMSTATE_UINT8_2DARRAY_V(arp_hostip, CYW43455SDIOState,
                                CYW43455_ARP_HOSTIP_MAX, sizeof(uint32_t),
                                13),
        VMSTATE_UINT32_V(arp_hostip_count, CYW43455SDIOState, 13),
        VMSTATE_UINT32_V(arp_mode, CYW43455SDIOState, 13),
        VMSTATE_BOOL_V(arp_offload, CYW43455SDIOState, 13),
        VMSTATE_UINT8_2DARRAY_V(nd_hostip, CYW43455SDIOState,
                                CYW43455_ND_HOSTIP_MAX, 16, 14),
        VMSTATE_UINT32_V(nd_hostip_count, CYW43455SDIOState, 14),
        VMSTATE_BOOL_V(nd_offload, CYW43455SDIOState, 14),
        VMSTATE_UINT64_V(tx_bytes, CYW43455SDIOState, 15),
        VMSTATE_UINT64_V(rx_bytes, CYW43455SDIOState, 15),
        VMSTATE_UINT64_V(tx_multicast_bytes, CYW43455SDIOState, 15),
        VMSTATE_UINT64_V(rx_multicast_bytes, CYW43455SDIOState, 15),
        VMSTATE_UINT32_V(tx_packets, CYW43455SDIOState, 15),
        VMSTATE_UINT32_V(rx_packets, CYW43455SDIOState, 15),
        VMSTATE_UINT32_V(tx_multicast_packets, CYW43455SDIOState, 15),
        VMSTATE_UINT32_V(rx_multicast_packets, CYW43455SDIOState, 15),
        VMSTATE_INT64_V(association_time_ms, CYW43455SDIOState, 15),
        VMSTATE_UINT8_V(escan_event_phase, CYW43455SDIOState, 16),
        VMSTATE_UINT16_V(escan_sync_id, CYW43455SDIOState, 16),
        VMSTATE_UINT32_V(chanspec, CYW43455SDIOState, 17),
        VMSTATE_UINT32_V(survey_duration_ms, CYW43455SDIOState, 17),
        VMSTATE_BOOL_V(survey_active, CYW43455SDIOState, 17),
        VMSTATE_UINT32_V(ap_chanspec, CYW43455SDIOState, 18),
        VMSTATE_UINT32_V(power_mode, CYW43455SDIOState, 19),
        VMSTATE_INT32_V(obss_coex, CYW43455SDIOState, 20),
        VMSTATE_UINT32_V(qtxpower, CYW43455SDIOState, 21),
        VMSTATE_UINT32_V(mfp_mode, CYW43455SDIOState, 22),
        VMSTATE_UINT16_ARRAY_V(pfn_channels, CYW43455SDIOState,
                               CYW43455_PNO_MAX_CHANNELS, 23),
        VMSTATE_UINT8_ARRAY_V(pfn_ssid_len, CYW43455SDIOState,
                              CYW43455_PNO_MAX_NETWORKS, 23),
        VMSTATE_UINT8_2DARRAY_V(pfn_ssids, CYW43455SDIOState,
                                CYW43455_PNO_MAX_NETWORKS,
                                CYW43455_PNO_SSID_MAX_LEN, 23),
        VMSTATE_UINT32_ARRAY_V(pfn_network_flags, CYW43455SDIOState,
                               CYW43455_PNO_MAX_NETWORKS, 23),
        VMSTATE_UINT8_ARRAY_V(pfn_scan_mac, CYW43455SDIOState,
                              ETH_ALEN, 23),
        VMSTATE_UINT32_V(pfn_scan_freq, CYW43455SDIOState, 23),
        VMSTATE_UINT32_V(pfn_channel_count, CYW43455SDIOState, 23),
        VMSTATE_UINT32_V(pfn_network_count, CYW43455SDIOState, 23),
        VMSTATE_UINT8_V(pfn_scan_mac_flags, CYW43455SDIOState, 23),
        VMSTATE_UINT8_V(pfn_bucket_count, CYW43455SDIOState, 23),
        VMSTATE_BOOL_V(pfn_enabled, CYW43455SDIOState, 23),
        VMSTATE_BOOL_V(tdls_enabled, CYW43455SDIOState, 24),
        VMSTATE_BOOL_V(roam_off, CYW43455SDIOState, 25),
        VMSTATE_UINT32_V(bcn_timeout, CYW43455SDIOState, 26),
        VMSTATE_INT32_V(roam_trigger, CYW43455SDIOState, 27),
        VMSTATE_INT32_V(roam_delta, CYW43455SDIOState, 27),
        VMSTATE_UINT8_V(tx_credit_limit, CYW43455SDIOState, 28),
        VMSTATE_BOOL_V(tx_credit_update_pending, CYW43455SDIOState, 28),
        VMSTATE_BOOL_V(association_requested, CYW43455SDIOState, 28),
        VMSTATE_BOOL_V(link_ssid_pending, CYW43455SDIOState, 28),
        VMSTATE_UINT8_V(rx_glom_count, CYW43455SDIOState, 28),
        VMSTATE_UINT16_V(rx_glom_align, CYW43455SDIOState, 28),
        VMSTATE_UINT16_ARRAY_V(rx_glom_lengths, CYW43455SDIOState,
                              CYW43455_RX_GLOM_MAX, 28),
        VMSTATE_UINT8_ARRAY_V(rx_glom_frame, CYW43455SDIOState,
                             CYW43455_F2_FRAME_MAX, 28),
        VMSTATE_UINT32_V(rx_glom_len, CYW43455SDIOState, 28),
        VMSTATE_BOOL_V(rx_glom_ready, CYW43455SDIOState, 28),
        VMSTATE_BOOL_V(rx_glom_super_pending, CYW43455SDIOState, 28),
        VMSTATE_TIMER_PTR_V(rx_glom_timer, CYW43455SDIOState, 28),
        VMSTATE_UINT32_EQUAL_V(hardware_profile, CYW43455SDIOState, 29),
        VMSTATE_BOOL_V(explicit_role, CYW43455SDIOState, 30),
        VMSTATE_BOOL_V(ap_mode, CYW43455SDIOState, 30),
        VMSTATE_BOOL_V(ap_started, CYW43455SDIOState, 30),
        VMSTATE_BOOL_V(ap_peer_associated, CYW43455SDIOState, 30),
        VMSTATE_BOOL_V(ap_channel_configured, CYW43455SDIOState, 30),
        VMSTATE_BOOL_V(ap_hidden, CYW43455SDIOState, 30),
        VMSTATE_BOOL_V(infra, CYW43455SDIOState, 30),
        VMSTATE_BOOL_V(regulatory, CYW43455SDIOState, 30),
        VMSTATE_UINT16_V(ap_beacon_period, CYW43455SDIOState, 30),
        VMSTATE_UINT8_V(ap_dtim_period, CYW43455SDIOState, 30),
        VMSTATE_UINT8_V(ap_ssid_len, CYW43455SDIOState, 30),
        VMSTATE_UINT8_ARRAY_V(ap_ssid, CYW43455SDIOState,
                              CYW43455_AP_SSID_MAX, 30),
        VMSTATE_UINT8_V(ap_events, CYW43455SDIOState, 30),
        VMSTATE_END_OF_LIST()
    },
};

static void sd_realize(DeviceState *dev, Error **errp);

static const SDProto sd_proto_spi;
static const SDProto sd_proto_emmc;

static bool sd_is_spi(SDState *sd)
{
    return sd->proto == &sd_proto_spi;
}

static bool sd_is_emmc(SDState *sd)
{
    return sd->proto == &sd_proto_emmc;
}

static const char *sd_version_str(enum SDPhySpecificationVersion version)
{
    static const char *sdphy_version[] = {
        [SD_PHY_SPECv2_00_VERS]     = "v2.00",
        [SD_PHY_SPECv3_01_VERS]     = "v3.01",
    };
    if (version >= ARRAY_SIZE(sdphy_version)) {
        return "unsupported version";
    }
    return sdphy_version[version];
}

static const char *sd_mode_name(enum SDCardModes mode)
{
    static const char *mode_name[] = {
        [sd_inactive]                   = "inactive",
        [sd_card_identification_mode]   = "identification",
        [sd_data_transfer_mode]         = "transfer",
    };
    assert(mode < ARRAY_SIZE(mode_name));
    return mode_name[mode];
}

static const char *sd_state_name(enum SDCardStates state)
{
    static const char *state_name[] = {
        [sd_idle_state]             = "idle",
        [sd_ready_state]            = "ready",
        [sd_identification_state]   = "identification",
        [sd_standby_state]          = "standby",
        [sd_transfer_state]         = "transfer",
        [sd_sendingdata_state]      = "sendingdata",
        [sd_bus_test_state]         = "bus-test",
        [sd_receivingdata_state]    = "receivingdata",
        [sd_programming_state]      = "programming",
        [sd_disconnect_state]       = "disconnect",
        [sd_sleep_state]            = "sleep",
        [sd_io_state]               = "i/o"
    };
    if (state == sd_inactive_state) {
        return "inactive";
    }
    if (state == sd_waitirq_state) {
        return "wait-irq";
    }
    assert(state < ARRAY_SIZE(state_name));
    return state_name[state];
}

static const char *sd_response_name(sd_rsp_type_t rsp)
{
    static const char *response_name[] = {
        [sd_r0]     = "RESP#0 (no response)",
        [sd_r1]     = "RESP#1 (normal cmd)",
        [spi_r2]    = "RESP#2 (STATUS reg)",
        [sd_r2_i]   = "RESP#2 (CID reg)",
        [sd_r2_s]   = "RESP#2 (CSD reg)",
        [sd_r3]     = "RESP#3 (OCR reg)",
        [sd_r6]     = "RESP#6 (RCA)",
        [sd_r7]     = "RESP#7 (operating voltage)",
    };
    if (rsp == sd_illegal) {
        return "ILLEGAL RESP";
    }
    if (rsp == sd_r1b) {
        rsp = sd_r1;
    }
    assert(rsp < ARRAY_SIZE(response_name));
    return response_name[rsp];
}

static const char *sd_cmd_name(SDState *sd, uint8_t cmd)
{
    static const char *cmd_abbrev[SDMMC_CMD_MAX] = {
        [18]    = "READ_MULTIPLE_BLOCK",
                                            [25]    = "WRITE_MULTIPLE_BLOCK",
    };
    const SDProto *sdp = sd->proto;

    if (sdp->cmd[cmd].handler) {
        assert(!cmd_abbrev[cmd]);
        return sdp->cmd[cmd].name;
    }
    return cmd_abbrev[cmd] ? cmd_abbrev[cmd] : "UNKNOWN_CMD";
}

static const char *sd_acmd_name(SDState *sd, uint8_t cmd)
{
    const SDProto *sdp = sd->proto;

    if (sdp->acmd[cmd].handler) {
        return sdp->acmd[cmd].name;
    }

    return "UNKNOWN_ACMD";
}

static uint8_t sd_get_dat_lines(SDState *sd)
{
    return sd->dat_lines;
}

static bool sd_get_cmd_line(SDState *sd)
{
    return sd->cmd_line;
}

static void sd_set_voltage(SDState *sd, uint16_t millivolts)
{
    trace_sdcard_set_voltage(millivolts);

    switch (millivolts) {
    case 3001 ... 3600: /* SD_VOLTAGE_3_3V */
    case 2001 ... 3000: /* SD_VOLTAGE_3_0V */
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "SD card voltage not supported: %.3fV",
                      millivolts / 1000.f);
    }
}

static enum SDCardModes sd_mode(SDState *sd)
{
    switch (sd->state) {
    case sd_inactive_state:
        return sd_inactive;
    case sd_idle_state:
    case sd_ready_state:
    case sd_identification_state:
        return sd_card_identification_mode;
    case sd_standby_state:
    case sd_transfer_state:
    case sd_sendingdata_state:
    case sd_receivingdata_state:
    case sd_programming_state:
    case sd_disconnect_state:
        return sd_data_transfer_mode;
    default:
        g_assert_not_reached();
    }
}

static uint8_t sd_crc7(const void *message, size_t width)
{
    int i, bit;
    uint8_t shift_reg = 0x00;
    const uint8_t *msg = (const uint8_t *)message;

    for (i = 0; i < width; i ++, msg ++)
        for (bit = 7; bit >= 0; bit --) {
            shift_reg <<= 1;
            if ((shift_reg >> 7) ^ ((*msg >> bit) & 1))
                shift_reg ^= 0x89;
        }

    return shift_reg;
}

/* Operation Conditions register */

#define OCR_POWER_DELAY_NS      500000 /* 0.5ms */

FIELD(OCR, VDD_VOLTAGE_WINDOW,          0, 24)
FIELD(OCR, VDD_VOLTAGE_WIN_LO,          0,  8)
FIELD(OCR, DUAL_VOLTAGE_CARD,           7,  1)
FIELD(OCR, VDD_VOLTAGE_WIN_HI,          8, 16)
FIELD(OCR, ACCEPT_SWITCH_1V8,          24,  1) /* Only UHS-I */
FIELD(OCR, UHS_II_CARD,                29,  1) /* Only UHS-II */
FIELD(OCR, CARD_CAPACITY,              30,  1) /* 0:SDSC, 1:SDHC/SDXC */
FIELD(OCR, CARD_POWER_UP,              31,  1)

#define ACMD41_ENQUIRY_MASK     0x00ffffff
#define ACMD41_R3_MASK          (R_OCR_VDD_VOLTAGE_WIN_HI_MASK \
                               | R_OCR_ACCEPT_SWITCH_1V8_MASK \
                               | R_OCR_UHS_II_CARD_MASK \
                               | R_OCR_CARD_CAPACITY_MASK \
                               | R_OCR_CARD_POWER_UP_MASK)

static void sd_ocr_powerup(void *opaque)
{
    SDState *sd = opaque;

    trace_sdcard_powerup();
    assert(!FIELD_EX32(sd->ocr, OCR, CARD_POWER_UP));

    /* card power-up OK */
    sd->ocr = FIELD_DP32(sd->ocr, OCR, CARD_POWER_UP, 1);

    if (sd->size > SDSC_MAX_CAPACITY) {
        sd->ocr = FIELD_DP32(sd->ocr, OCR, CARD_CAPACITY, 1);
    }
}

static void sd_set_ocr(SDState *sd)
{
    /* All voltages OK */
    sd->ocr = R_OCR_VDD_VOLTAGE_WIN_HI_MASK;

    if (sd_is_spi(sd)) {
        /*
         * We don't need to emulate power up sequence in SPI-mode.
         * Thus, the card's power up status bit should be set to 1 when reset.
         * The card's capacity status bit should also be set if SD card size
         * is larger than 2GB for SDHC support.
         */
        sd_ocr_powerup(sd);
    }
}

/* SD Configuration register */

static void sd_set_scr(SDState *sd)
{
    sd->scr[0] = 0 << 4;        /* SCR structure version 1.0 */
    sd->scr[0] |= 2;            /* Spec Version 2.00 or Version 3.0X */
    sd->scr[1] = (2 << 4)       /* SDSC Card (Security Version 1.01) */
                 | 0b0101;      /* 1-bit or 4-bit width bus modes */
    sd->scr[2] = 0x00;          /* Extended Security is not supported. */
    if (sd->spec_version >= SD_PHY_SPECv3_01_VERS) {
        sd->scr[2] |= 1 << 7;   /* Spec Version 3.0X */
    }
    sd->scr[3] = 0x00;
    /* reserved for manufacturer usage */
    sd->scr[4] = 0x00;
    sd->scr[5] = 0x00;
    sd->scr[6] = 0x00;
    sd->scr[7] = 0x00;
}

/* Card IDentification register */

#define MID     0xaa
#define OID     "XY"
#define PNM     "QEMU!"
#define PRV     0x01
#define MDT_YR  2006
#define MDT_MON 2

static void sd_set_cid(SDState *sd)
{
    sd->cid[0] = MID;       /* Fake card manufacturer ID (MID) */
    sd->cid[1] = OID[0];    /* OEM/Application ID (OID) */
    sd->cid[2] = OID[1];
    sd->cid[3] = PNM[0];    /* Fake product name (PNM) */
    sd->cid[4] = PNM[1];
    sd->cid[5] = PNM[2];
    sd->cid[6] = PNM[3];
    sd->cid[7] = PNM[4];
    sd->cid[8] = PRV;       /* Fake product revision (PRV) */
    stl_be_p(&sd->cid[9], 0xdeadbeef); /* Fake serial number (PSN) */
    sd->cid[13] = 0x00 |    /* Manufacture date (MDT) */
        ((MDT_YR - 2000) / 10);
    sd->cid[14] = ((MDT_YR % 10) << 4) | MDT_MON;
    sd->cid[15] = (sd_crc7(sd->cid, 15) << 1) | 1;
}

static void emmc_set_cid(SDState *sd)
{
    sd->cid[0] = MID;       /* Fake card manufacturer ID (MID) */
    sd->cid[1] = 0b01;      /* CBX: soldered BGA */
    sd->cid[2] = OID[0];    /* OEM/Application ID (OID) */
    sd->cid[3] = PNM[0];    /* Fake product name (PNM) */
    sd->cid[4] = PNM[1];
    sd->cid[5] = PNM[2];
    sd->cid[6] = PNM[3];
    sd->cid[7] = PNM[4];
    sd->cid[8] = PNM[4];
    sd->cid[9] = PRV;       /* Fake product revision (PRV) */
    stl_be_p(&sd->cid[10], 0xdeadbeef); /* Fake serial number (PSN) */
    sd->cid[14] = (MDT_MON << 4) | (MDT_YR - 1997); /* Manufacture date (MDT) */
    sd->cid[15] = (sd_crc7(sd->cid, 15) << 1) | 1;
}

/* Card-Specific Data register */

#define HWBLOCK_SHIFT   9        /* 512 bytes */
#define SECTOR_SHIFT    5        /* 16 kilobytes */
#define WPGROUP_SHIFT   7        /* 2 megs */
#define CMULT_SHIFT     9        /* 512 times HWBLOCK_SIZE */
#define WPGROUP_SIZE    (1 << (HWBLOCK_SHIFT + SECTOR_SHIFT + WPGROUP_SHIFT))

static const uint8_t sd_csd_rw_mask[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfc, 0xfe,
};

static void emmc_set_ext_csd(SDState *sd, uint64_t size)
{
    uint32_t sectcount = size >> HWBLOCK_SHIFT;

    memset(sd->ext_csd, 0, sizeof(sd->ext_csd)); /* FIXME only RW at reset */

    /* Properties segment (RO) */
    sd->ext_csd[EXT_CSD_S_CMD_SET] = 0b1; /* supported command sets */
    sd->ext_csd[EXT_CSD_BOOT_INFO] = 0x0; /* Boot information */
                                     /* Boot partition size. 128KB unit */
    sd->ext_csd[EXT_CSD_BOOT_MULT] = sd->boot_part_size / (128 * KiB);
    sd->ext_csd[EXT_CSD_ACC_SIZE] = 0x1; /* Access size */
    sd->ext_csd[EXT_CSD_HC_ERASE_GRP_SIZE] = 0x01; /* HC Erase unit size */
    sd->ext_csd[EXT_CSD_ERASE_TIMEOUT_MULT] = 0x01; /* HC erase timeout */
    sd->ext_csd[EXT_CSD_REL_WR_SEC_C] = 0x1; /* Reliable write sector count */
    sd->ext_csd[EXT_CSD_HC_WP_GRP_SIZE] = 0x01; /* HC write protect group size */
    sd->ext_csd[EXT_CSD_S_C_VCC] = 0x01; /* Sleep current VCC  */
    sd->ext_csd[EXT_CSD_S_C_VCCQ] = 0x01; /* Sleep current VCCQ */
    sd->ext_csd[EXT_CSD_S_A_TIMEOUT] = 0x01; /* Sleep/Awake timeout */
    stl_le_p(&sd->ext_csd[EXT_CSD_SEC_CNT], sectcount); /* Sector count */
    sd->ext_csd[210] = 0x46; /* Min write perf for 8bit@52Mhz */
    sd->ext_csd[209] = 0x46; /* Min read perf for 8bit@52Mhz  */
    sd->ext_csd[208] = 0x46; /* Min write perf for 4bit@52Mhz */
    sd->ext_csd[207] = 0x46; /* Min read perf for 4bit@52Mhz */
    sd->ext_csd[206] = 0x46; /* Min write perf for 4bit@26Mhz */
    sd->ext_csd[205] = 0x46; /* Min read perf for 4bit@26Mhz */
    sd->ext_csd[EXT_CSD_CARD_TYPE] = 0b11;
    sd->ext_csd[EXT_CSD_STRUCTURE] = 2;
    sd->ext_csd[EXT_CSD_REV] = 5;
    sd->ext_csd[EXT_CSD_RPMB_MULT] = sd->rpmb_part_size / (128 * KiB);
    sd->ext_csd[EXT_CSD_PARTITION_SUPPORT] = 0b111;

    /* Mode segment (RW) */
    sd->ext_csd[EXT_CSD_PART_CONFIG] = sd->boot_config;
}

static void emmc_set_csd(SDState *sd, uint64_t size)
{
    int hwblock_shift = HWBLOCK_SHIFT;
    uint32_t sectsize = (1 << (SECTOR_SHIFT + 1)) - 1;
    uint32_t wpsize = (1 << (WPGROUP_SHIFT + 1)) - 1;

    sd->csd[0] = (3 << 6) | (4 << 2); /* Spec v4.3 with EXT_CSD */
    sd->csd[1] = (1 << 3) | 6; /* Asynchronous data access time: 1ms */
    sd->csd[2] = 0x00;
    sd->csd[3] = (1 << 3) | 3;; /* Maximum bus clock frequency: 100MHz */
    sd->csd[4] = 0x0f;
    if (size <= 2 * GiB) {
        /* use 1k blocks */
        uint32_t csize1k = (size >> (CMULT_SHIFT + 10)) - 1;
        sd->csd[5] = 0x5a;
        sd->csd[6] = 0x80 | ((csize1k >> 10) & 0xf);
        sd->csd[7] = (csize1k >> 2) & 0xff;
    } else { /* >= 2GB : size stored in ext CSD, block addressing */
        sd->csd[5] = 0x59;
        sd->csd[6] = 0x8f;
        sd->csd[7] = 0xff;
        sd->ocr = FIELD_DP32(sd->ocr, OCR, CARD_CAPACITY, 1);
    }
    sd->csd[8] = 0xff;
    sd->csd[9] = 0xfc |     /* Max. write current */
        ((CMULT_SHIFT - 2) >> 1);
    sd->csd[10] = 0x40 |    /* Erase sector size */
        (((CMULT_SHIFT - 2) << 7) & 0x80) | (sectsize >> 1);
    sd->csd[11] = 0x00 |    /* Write protect group size */
        ((sectsize << 7) & 0x80) | wpsize;
    sd->csd[12] = 0x90 |    /* Write speed factor */
        (hwblock_shift >> 2);
    sd->csd[13] = 0x20 |    /* Max. write data block length */
        ((hwblock_shift << 6) & 0xc0);
    sd->csd[14] = 0x00;
    sd->csd[15] = (sd_crc7(sd->csd, 15) << 1) | 1;
    emmc_set_ext_csd(sd, size);
}

static void sd_set_csd(SDState *sd, uint64_t size)
{
    int hwblock_shift = HWBLOCK_SHIFT;
    uint32_t csize;
    uint32_t sectsize = (1 << (SECTOR_SHIFT + 1)) - 1;
    uint32_t wpsize = (1 << (WPGROUP_SHIFT + 1)) - 1;

    /* To indicate 2 GiB card, BLOCK_LEN shall be 1024 bytes */
    if (size == SDSC_MAX_CAPACITY) {
        hwblock_shift += 1;
    }
    csize = (size >> (CMULT_SHIFT + hwblock_shift)) - 1;

    if (size <= SDSC_MAX_CAPACITY) { /* Standard Capacity SD */
        sd->csd[0] = 0x00;      /* CSD structure */
        sd->csd[1] = 0x26;      /* Data read access-time-1 */
        sd->csd[2] = 0x00;      /* Data read access-time-2 */
        sd->csd[3] = 0x32;      /* Max. data transfer rate: 25 MHz */
        sd->csd[4] = 0x5f;      /* Card Command Classes */
        sd->csd[5] = 0x50 |     /* Max. read data block length */
            hwblock_shift;
        sd->csd[6] = 0xe0 |     /* Partial block for read allowed */
            ((csize >> 10) & 0x03);
        sd->csd[7] = 0x00 |     /* Device size */
            ((csize >> 2) & 0xff);
        sd->csd[8] = 0x3f |     /* Max. read current */
            ((csize << 6) & 0xc0);
        sd->csd[9] = 0xfc |     /* Max. write current */
            ((CMULT_SHIFT - 2) >> 1);
        sd->csd[10] = 0x40 |    /* Erase sector size */
            (((CMULT_SHIFT - 2) << 7) & 0x80) | (sectsize >> 1);
        sd->csd[11] = 0x00 |    /* Write protect group size */
            ((sectsize << 7) & 0x80) | wpsize;
        sd->csd[12] = 0x90 |    /* Write speed factor */
            (hwblock_shift >> 2);
        sd->csd[13] = 0x20 |    /* Max. write data block length */
            ((hwblock_shift << 6) & 0xc0);
        sd->csd[14] = 0x00;     /* File format group */
    } else {                    /* SDHC */
        size /= 512 * KiB;
        size -= 1;
        sd->csd[0] = 0x40;
        sd->csd[1] = 0x0e;
        sd->csd[2] = 0x00;
        sd->csd[3] = 0x32;
        sd->csd[4] = 0x5b;
        sd->csd[5] = 0x59;
        sd->csd[6] = 0x00;
        st24_be_p(&sd->csd[7], size);
        sd->csd[10] = 0x7f;
        sd->csd[11] = 0x80;
        sd->csd[12] = 0x0a;
        sd->csd[13] = 0x40;
        sd->csd[14] = 0x00;
    }
    sd->csd[15] = (sd_crc7(sd->csd, 15) << 1) | 1;
}

/* Relative Card Address register */

static void sd_set_rca(SDState *sd, uint16_t value)
{
    trace_sdcard_set_rca(value);
    sd->rca = value;
}

static uint16_t sd_req_get_rca(SDState *s, SDRequest req)
{
    switch (s->proto->cmd[req.cmd].type) {
    case sd_ac:
    case sd_adtc:
        return req.arg >> 16;
    case sd_spi:
    default:
        g_assert_not_reached();
    }
}

static bool sd_req_rca_same(SDState *s, SDRequest req)
{
    return sd_req_get_rca(s, req) == s->rca;
}

/* Card Status register */

FIELD(CSR, AKE_SEQ_ERROR,               3,  1)
FIELD(CSR, APP_CMD,                     5,  1)
FIELD(CSR, FX_EVENT,                    6,  1)
FIELD(CSR, SWITCH_ERROR,                7,  1)
FIELD(CSR, READY_FOR_DATA,              8,  1)
FIELD(CSR, CURRENT_STATE,               9,  4)
FIELD(CSR, ERASE_RESET,                13,  1)
FIELD(CSR, CARD_ECC_DISABLED,          14,  1)
FIELD(CSR, WP_ERASE_SKIP,              15,  1)
FIELD(CSR, CSD_OVERWRITE,              16,  1)
FIELD(CSR, DEFERRED_RESPONSE,          17,  1)
FIELD(CSR, ERROR,                      19,  1)
FIELD(CSR, CC_ERROR,                   20,  1)
FIELD(CSR, CARD_ECC_FAILED,            21,  1)
FIELD(CSR, ILLEGAL_COMMAND,            22,  1)
FIELD(CSR, COM_CRC_ERROR,              23,  1)
FIELD(CSR, LOCK_UNLOCK_FAILED,         24,  1)
FIELD(CSR, CARD_IS_LOCKED,             25,  1)
FIELD(CSR, WP_VIOLATION,               26,  1)
FIELD(CSR, ERASE_PARAM,                27,  1)
FIELD(CSR, ERASE_SEQ_ERROR,            28,  1)
FIELD(CSR, BLOCK_LEN_ERROR,            29,  1)
FIELD(CSR, ADDRESS_ERROR,              30,  1)
FIELD(CSR, OUT_OF_RANGE,               31,  1)

/* Card status bits, split by clear condition:
 * A : According to the card current state
 * B : Always related to the previous command
 * C : Cleared by read
 */
#define CARD_STATUS_A           (R_CSR_READY_FOR_DATA_MASK \
                               | R_CSR_CARD_ECC_DISABLED_MASK \
                               | R_CSR_CARD_IS_LOCKED_MASK)
#define CARD_STATUS_B           (R_CSR_CURRENT_STATE_MASK \
                               | R_CSR_ILLEGAL_COMMAND_MASK \
                               | R_CSR_COM_CRC_ERROR_MASK)
#define CARD_STATUS_C           (R_CSR_AKE_SEQ_ERROR_MASK \
                               | R_CSR_APP_CMD_MASK \
                               | R_CSR_ERASE_RESET_MASK \
                               | R_CSR_WP_ERASE_SKIP_MASK \
                               | R_CSR_CSD_OVERWRITE_MASK \
                               | R_CSR_ERROR_MASK \
                               | R_CSR_CC_ERROR_MASK \
                               | R_CSR_CARD_ECC_FAILED_MASK \
                               | R_CSR_LOCK_UNLOCK_FAILED_MASK \
                               | R_CSR_WP_VIOLATION_MASK \
                               | R_CSR_ERASE_PARAM_MASK \
                               | R_CSR_ERASE_SEQ_ERROR_MASK \
                               | R_CSR_BLOCK_LEN_ERROR_MASK \
                               | R_CSR_ADDRESS_ERROR_MASK \
                               | R_CSR_OUT_OF_RANGE_MASK)

static void sd_set_cardstatus(SDState *sd)
{
    sd->card_status = READY_FOR_DATA;
}

static void sd_set_sdstatus(SDState *sd)
{
    memset(sd->sd_status, 0, 64);
}

static const uint8_t sd_tuning_block_pattern4[64] = {
    /*
     * See: Physical Layer Simplified Specification Version 3.01,
     * Table 4-2.
     */
    0xff, 0x0f, 0xff, 0x00,     0x0f, 0xfc, 0xc3, 0xcc,
    0xc3, 0x3c, 0xcc, 0xff,     0xfe, 0xff, 0xfe, 0xef,
    0xff, 0xdf, 0xff, 0xdd,     0xff, 0xfb, 0xff, 0xfb,
    0xbf, 0xff, 0x7f, 0xff,     0x77, 0xf7, 0xbd, 0xef,
    0xff, 0xf0, 0xff, 0xf0,     0x0f, 0xfc, 0xcc, 0x3c,
    0xcc, 0x33, 0xcc, 0xcf,     0xff, 0xef, 0xff, 0xee,
    0xff, 0xfd, 0xff, 0xfd,     0xdf, 0xff, 0xbf, 0xff,
    0xbb, 0xff, 0xf7, 0xff,     0xf7, 0x7f, 0x7b, 0xde
};

static int sd_req_crc_validate(SDRequest *req)
{
    uint8_t buffer[5];
    buffer[0] = 0x40 | req->cmd;
    stl_be_p(&buffer[1], req->arg);
    return 0;
    return sd_crc7(buffer, 5) != req->crc;  /* TODO */
}

static size_t sd_response_size(SDState *sd, sd_rsp_type_t rtype)
{
    switch (rtype) {
    case sd_r1:
    case sd_r1b:
        return sd_is_spi(sd) ? 1 : 4;

    case spi_r2:
        assert(sd_is_spi(sd));
        return 2;

    case sd_r2_i:
    case sd_r2_s:
        assert(!sd_is_spi(sd));
        return 16;

    case sd_r3:
    case sd_r7:
        return sd_is_spi(sd) ? 5 : 4;

    case sd_r6:
        assert(!sd_is_spi(sd));
        return 4;

    case sd_r0:
    case sd_illegal:
        return sd_is_spi(sd) ? 1 : 0;

    default:
        g_assert_not_reached();
    }
}

static void sd_response_r1_make(SDState *sd, uint8_t *response)
{
    if (sd_is_spi(sd)) {
        response[0] = sd->state == sd_idle_state;
        response[0] |= FIELD_EX32(sd->card_status, CSR, ERASE_RESET) << 1;
        response[0] |= FIELD_EX32(sd->card_status, CSR, ILLEGAL_COMMAND) << 2;
        response[0] |= FIELD_EX32(sd->card_status, CSR, COM_CRC_ERROR) << 3;
        response[0] |= FIELD_EX32(sd->card_status, CSR, ERASE_SEQ_ERROR) << 4;
        response[0] |= FIELD_EX32(sd->card_status, CSR, ADDRESS_ERROR) << 5;
        response[0] |= FIELD_EX32(sd->card_status, CSR, BLOCK_LEN_ERROR) << 6;
        response[0] |= 0 << 7;
    } else {
        stl_be_p(response, sd->card_status);
    }

    /* Clear the "clear on read" status bits */
    sd->card_status &= ~CARD_STATUS_C;
}

static void spi_response_r2_make(SDState *sd, uint8_t *resp)
{
    /* Prepend R1 */
    sd_response_r1_make(sd, resp);

    resp[1]  = FIELD_EX32(sd->card_status, CSR, CARD_IS_LOCKED) << 0;
    resp[1] |= (FIELD_EX32(sd->card_status, CSR, LOCK_UNLOCK_FAILED)
                || FIELD_EX32(sd->card_status, CSR, WP_ERASE_SKIP)) << 1;
    resp[1] |= FIELD_EX32(sd->card_status, CSR, ERROR) << 2;
    resp[1] |= FIELD_EX32(sd->card_status, CSR, CC_ERROR) << 3;
    resp[1] |= FIELD_EX32(sd->card_status, CSR, CARD_ECC_FAILED) << 4;
    resp[1] |= FIELD_EX32(sd->card_status, CSR, WP_VIOLATION) << 5;
    resp[1] |= FIELD_EX32(sd->card_status, CSR, ERASE_PARAM) << 6;
    resp[1] |= FIELD_EX32(sd->card_status, CSR, OUT_OF_RANGE) << 7;
}

static void sd_response_r3_make(SDState *sd, uint8_t *response)
{
    if (sd_is_spi(sd)) {
        /* Prepend R1 */
        sd_response_r1_make(sd, response);
        response++;
    }
    stl_be_p(response, sd->ocr & ACMD41_R3_MASK);
}

static void sd_response_r6_make(SDState *sd, uint8_t *response)
{
    uint16_t status;

    status = ((sd->card_status >> 8) & 0xc000) |
             ((sd->card_status >> 6) & 0x2000) |
              (sd->card_status & 0x1fff);
    sd->card_status &= ~(CARD_STATUS_C & 0xc81fff);
    stw_be_p(response + 0, sd->rca);
    stw_be_p(response + 2, status);
}

static void sd_response_r7_make(SDState *sd, uint8_t *response)
{
    if (sd_is_spi(sd)) {
        /* Prepend R1 */
        sd_response_r1_make(sd, response);
        response++;
    }
    stl_be_p(response, sd->vhs);
}

static uint32_t sd_blk_len(SDState *sd)
{
    if (FIELD_EX32(sd->ocr, OCR, CARD_CAPACITY)) {
        return 1 << HWBLOCK_SHIFT;
    }
    return sd->blk_len;
}

/*
 * This requires a disk image that has two boot partitions inserted at the
 * beginning of it, followed by an RPMB partition. The size of the boot
 * partitions is the "boot-partition-size" property, the one of the RPMB
 * partition is 'rpmb-partition-size'.
 */
static uint32_t sd_part_offset(SDState *sd)
{
    unsigned partition_access;

    if (!sd_is_emmc(sd)) {
        return 0;
    }

    partition_access = sd->ext_csd[EXT_CSD_PART_CONFIG]
                                 & EXT_CSD_PART_CONFIG_ACC_MASK;
    switch (partition_access) {
    case EXT_CSD_PART_CONFIG_ACC_DEFAULT:
        return sd->boot_part_size * 2 + sd->rpmb_part_size;
    case EXT_CSD_PART_CONFIG_ACC_BOOT1:
        return 0;
    case EXT_CSD_PART_CONFIG_ACC_BOOT2:
        return sd->boot_part_size * 1;
    case EXT_CSD_PART_CONFIG_ACC_RPMB:
        return sd->boot_part_size * 2;
    default:
         g_assert_not_reached();
    }
}

static uint64_t sd_req_get_address(SDState *sd, SDRequest req)
{
    uint64_t addr;

    if (FIELD_EX32(sd->ocr, OCR, CARD_CAPACITY)) {
        addr = (uint64_t) req.arg << HWBLOCK_SHIFT;
    } else {
        addr = req.arg;
    }
    trace_sdcard_req_addr(req.arg, addr);
    return addr;
}

static inline uint64_t sd_addr_to_wpnum(uint64_t addr)
{
    return addr >> (HWBLOCK_SHIFT + SECTOR_SHIFT + WPGROUP_SHIFT);
}

static void sd_reset(DeviceState *dev)
{
    SDState *sd = SDMMC_COMMON(dev);
    SDCardClass *sc = SDMMC_COMMON_GET_CLASS(sd);
    uint64_t size;
    uint64_t sect;

    trace_sdcard_reset();
    if (sd->blk) {
        blk_get_geometry(sd->blk, &sect);
    } else {
        sect = 0;
    }
    size = sect << HWBLOCK_SHIFT;
    if (sd_is_emmc(sd)) {
        size -= sd->boot_part_size * 2 + sd->rpmb_part_size;
    }

    sect = sd_addr_to_wpnum(size) + 1;

    sd->state = sd_idle_state;

    /* card registers */
    sd->rca = sd_is_emmc(sd) ? 0x0001 : 0x0000;
    sd->size = size;
    sd_set_ocr(sd);
    sd_set_scr(sd);
    sc->set_cid(sd);
    sc->set_csd(sd, size);
    sd_set_cardstatus(sd);
    sd_set_sdstatus(sd);

    g_free(sd->wp_group_bmap);
    sd->wp_switch = sd->blk ? !blk_is_writable(sd->blk) : false;
    sd->wp_group_bits = sect;
    sd->wp_group_bmap = bitmap_new(sd->wp_group_bits);
    memset(sd->function_group, 0, sizeof(sd->function_group));
    sd->erase_start = INVALID_ADDRESS;
    sd->erase_end = INVALID_ADDRESS;
    sd->blk_len = 0x200;
    sd->pwd_len = 0;
    sd->expecting_acmd = false;
    sd->dat_lines = 0xf;
    sd->cmd_line = true;
    sd->multi_blk_cnt = 0;
}

static bool sd_get_inserted(SDState *sd)
{
    return sd->blk && blk_is_inserted(sd->blk);
}

static bool sd_get_readonly(SDState *sd)
{
    return sd->wp_switch;
}

static void sd_cardchange(void *opaque, bool load, Error **errp)
{
    SDState *sd = opaque;
    DeviceState *dev = DEVICE(sd);
    SDBus *sdbus;
    bool inserted = sd_get_inserted(sd);
    bool readonly = sd_get_readonly(sd);

    if (inserted) {
        trace_sdcard_inserted(readonly);
        sd_reset(dev);
    } else {
        trace_sdcard_ejected();
    }

    sdbus = SD_BUS(qdev_get_parent_bus(dev));
    sdbus_set_inserted(sdbus, inserted);
    if (inserted) {
        sdbus_set_readonly(sdbus, readonly);
    }
}

static const BlockDevOps sd_block_ops = {
    .change_media_cb = sd_cardchange,
};

static bool sd_ocr_vmstate_needed(void *opaque)
{
    SDState *sd = opaque;

    /* Include the OCR state (and timer) if it is not yet powered up */
    return !FIELD_EX32(sd->ocr, OCR, CARD_POWER_UP);
}

static const VMStateDescription sd_ocr_vmstate = {
    .name = "sd-card/ocr-state",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sd_ocr_vmstate_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ocr, SDState),
        VMSTATE_TIMER_PTR(ocr_power_timer, SDState),
        VMSTATE_END_OF_LIST()
    },
};

static bool vmstate_needed_for_rpmb(void *opaque)
{
    SDState *sd = opaque;

    return sd->rpmb_part_size > 0;
}

static const VMStateDescription emmc_rpmb_vmstate = {
    .name = "sd-card/ext_csd_modes-state",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = vmstate_needed_for_rpmb,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(rpmb.result.key_mac, SDState, RPMB_KEY_MAC_LEN),
        VMSTATE_UINT8_ARRAY(rpmb.result.data, SDState, RPMB_DATA_LEN),
        VMSTATE_UINT8_ARRAY(rpmb.result.nonce, SDState, RPMB_NONCE_LEN),
        VMSTATE_UINT32(rpmb.result.write_counter, SDState),
        VMSTATE_UINT16(rpmb.result.address, SDState),
        VMSTATE_UINT16(rpmb.result.block_count, SDState),
        VMSTATE_UINT16(rpmb.result.result, SDState),
        VMSTATE_UINT16(rpmb.result.req_resp, SDState),
        VMSTATE_UINT32(rpmb.write_counter, SDState),
        VMSTATE_UINT8_ARRAY(rpmb.key, SDState, 32),
        VMSTATE_UINT8(rpmb.key_set, SDState),
        VMSTATE_END_OF_LIST()
    },
};

static bool vmstate_needed_for_emmc(void *opaque)
{
    SDState *sd = opaque;

    return sd_is_emmc(sd);
}

static const VMStateDescription emmc_extcsd_vmstate = {
    .name = "sd-card/ext_csd_modes-state",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = vmstate_needed_for_emmc,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(ext_csd_rw, SDState, 192),
        VMSTATE_END_OF_LIST()
    },
};

static int sd_vmstate_pre_load(void *opaque)
{
    SDState *sd = opaque;

    /* If the OCR state is not included (prior versions, or not
     * needed), then the OCR must be set as powered up. If the OCR state
     * is included, this will be replaced by the state restore.
     */
    sd_ocr_powerup(sd);

    return 0;
}

static const VMStateDescription sd_vmstate = {
    .name = "sd-card",
    .version_id = 2,
    .minimum_version_id = 2,
    .pre_load = sd_vmstate_pre_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UNUSED(4),
        VMSTATE_INT32(state, SDState),
        VMSTATE_UINT8_ARRAY(cid, SDState, 16),
        VMSTATE_UINT8_ARRAY(csd, SDState, 16),
        VMSTATE_UINT16(rca, SDState),
        VMSTATE_UINT32(card_status, SDState),
        VMSTATE_PARTIAL_BUFFER(sd_status, SDState, 1),
        VMSTATE_UINT32(vhs, SDState),
        VMSTATE_BITMAP(wp_group_bmap, SDState, 0, wp_group_bits),
        VMSTATE_UINT32(blk_len, SDState),
        VMSTATE_UINT32(multi_blk_cnt, SDState),
        VMSTATE_UINT32(erase_start, SDState),
        VMSTATE_UINT32(erase_end, SDState),
        VMSTATE_UINT8_ARRAY(pwd, SDState, 16),
        VMSTATE_UINT32(pwd_len, SDState),
        VMSTATE_UINT8_ARRAY(function_group, SDState, 6),
        VMSTATE_UINT8(current_cmd, SDState),
        VMSTATE_BOOL(expecting_acmd, SDState),
        VMSTATE_UINT32(blk_written, SDState),
        VMSTATE_UINT64(data_start, SDState),
        VMSTATE_UINT32(data_offset, SDState),
        VMSTATE_UINT8_ARRAY(data, SDState, 512),
        VMSTATE_UNUSED_V(1, 512),
        VMSTATE_UNUSED(1),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &sd_ocr_vmstate,
        &emmc_extcsd_vmstate,
        &emmc_rpmb_vmstate,
        NULL
    },
};

static void sd_blk_read(SDState *sd, uint64_t addr, uint32_t len)
{
    trace_sdcard_read_block(addr, len);
    addr += sd_part_offset(sd);
    if (!sd->blk || blk_pread(sd->blk, addr, len, sd->data, 0) < 0) {
        fprintf(stderr, "sd_blk_read: read error on host side\n");
    }
}

static void sd_blk_write(SDState *sd, uint64_t addr, uint32_t len)
{
    trace_sdcard_write_block(addr, len);
    addr += sd_part_offset(sd);
    if (!sd->blk || blk_pwrite(sd->blk, addr, len, sd->data, 0) < 0) {
        fprintf(stderr, "sd_blk_write: write error on host side\n");
    }
}

static bool rpmb_calc_hmac(SDState *sd, const RPMBDataFrame *frame,
                           unsigned int num_blocks, uint8_t *mac)
{
    g_autoptr(QCryptoHmac) hmac = NULL;
    size_t mac_len = RPMB_KEY_MAC_LEN;
    bool success = true;
    Error *err = NULL;
    uint64_t offset;

    hmac = qcrypto_hmac_new(QCRYPTO_HASH_ALGO_SHA256, sd->rpmb.key,
                            RPMB_KEY_MAC_LEN, &err);
    if (!hmac) {
        error_report_err(err);
        return false;
    }

    /*
     * This implies a read request because we only support single-block write
     * requests so far.
     */
    if (num_blocks > 1) {
        /*
         * Unfortunately, the underlying crypto libraries do not allow us to
         * migrate an active QCryptoHmac state. Therefore, we have to calculate
         * the HMAC in one run. To avoid buffering a complete read sequence in
         * SDState, reconstruct all frames except for the last one.
         */
        void *buf = sd->data;

        assert(RPMB_HASH_LEN <= sizeof(sd->data));

        /*
         * We will hash everything from data field to the end of RPMBDataFrame.
         */
        memcpy((uint8_t *)buf + RPMB_DATA_LEN,
               (uint8_t *)frame + offsetof(RPMBDataFrame, nonce),
               RPMB_HASH_LEN - RPMB_DATA_LEN);

        offset = lduw_be_p(&frame->address) * RPMB_DATA_LEN + sd_part_offset(sd);
        do {
            if (blk_pread(sd->blk, offset, RPMB_DATA_LEN, buf, 0) < 0) {
                error_report("sd_blk_read: read error on host side");
                success = false;
                break;
            }
            if (qcrypto_hmac_bytes(hmac, buf, RPMB_HASH_LEN, NULL, NULL,
                                   &err) < 0) {
                error_report_err(err);
                success = false;
                break;
            }
            offset += RPMB_DATA_LEN;
        } while (--num_blocks > 1);
    }

    if (success &&
        qcrypto_hmac_bytes(hmac, frame->data, RPMB_HASH_LEN, &mac,
                           &mac_len, &err) < 0) {
        error_report_err(err);
        success = false;
    }
    assert(!success || mac_len == RPMB_KEY_MAC_LEN);

    return success;
}

static void emmc_rpmb_blk_read(SDState *sd, uint64_t addr, uint32_t len)
{
    uint16_t resp = lduw_be_p(&sd->rpmb.result.req_resp);
    uint16_t result = lduw_be_p(&sd->rpmb.result.result);
    unsigned int curr_block = 0;

    if ((result & ~RPMB_RESULT_COUTER_EXPIRED) == RPMB_RESULT_OK &&
        resp == RPMB_RESP(RPMB_REQ_AUTH_DATA_READ)) {
        curr_block = lduw_be_p(&sd->rpmb.result.address);
        if (sd->rpmb.result.block_count == 0) {
            stw_be_p(&sd->rpmb.result.block_count, sd->multi_blk_cnt);
        } else {
            curr_block += lduw_be_p(&sd->rpmb.result.block_count);
            curr_block -= sd->multi_blk_cnt;
        }
        addr = curr_block * RPMB_DATA_LEN + sd_part_offset(sd);
        if (blk_pread(sd->blk, addr, RPMB_DATA_LEN,
                      sd->rpmb.result.data, 0) < 0) {
            error_report("sd_blk_read: read error on host side");
            memset(sd->rpmb.result.data, 0, sizeof(sd->rpmb.result.data));
            stw_be_p(&sd->rpmb.result.result,
                     RPMB_RESULT_READ_FAILURE
                     | (result & RPMB_RESULT_COUTER_EXPIRED));
        }
        if (sd->multi_blk_cnt == 1 &&
            !rpmb_calc_hmac(sd, &sd->rpmb.result,
                            lduw_be_p(&sd->rpmb.result.block_count),
                            sd->rpmb.result.key_mac)) {
            memset(sd->rpmb.result.data, 0, sizeof(sd->rpmb.result.data));
            stw_be_p(&sd->rpmb.result.result, RPMB_RESULT_AUTH_FAILURE);
        }
    } else if (!rpmb_calc_hmac(sd, &sd->rpmb.result, 1,
                               sd->rpmb.result.key_mac)) {
        memset(sd->rpmb.result.data, 0, sizeof(sd->rpmb.result.data));
        stw_be_p(&sd->rpmb.result.result, RPMB_RESULT_AUTH_FAILURE);
    }
    memcpy(sd->data, &sd->rpmb.result, sizeof(sd->rpmb.result));

    trace_sdcard_rpmb_read_block(resp, curr_block,
                                 lduw_be_p(&sd->rpmb.result.result));
}

static void emmc_rpmb_blk_write(SDState *sd, uint64_t addr, uint32_t len)
{
    RPMBDataFrame *frame = (RPMBDataFrame *)sd->data;
    uint16_t req = lduw_be_p(&frame->req_resp);
    uint8_t mac[RPMB_KEY_MAC_LEN];

    if (req == RPMB_REQ_READ_RESULT) {
        /* just return the current result register */
        goto exit;
    }
    memset(&sd->rpmb.result, 0, sizeof(sd->rpmb.result));
    memcpy(sd->rpmb.result.nonce, frame->nonce, sizeof(sd->rpmb.result.nonce));
    stw_be_p(&sd->rpmb.result.result, RPMB_RESULT_OK);
    stw_be_p(&sd->rpmb.result.req_resp, RPMB_RESP(req));

    if (!sd->rpmb.key_set && req != RPMB_REQ_PROGRAM_AUTH_KEY) {
        stw_be_p(&sd->rpmb.result.result, RPMB_RESULT_NO_AUTH_KEY);
        goto exit;
    }

    switch (req) {
    case RPMB_REQ_PROGRAM_AUTH_KEY:
        if (sd->rpmb.key_set) {
            stw_be_p(&sd->rpmb.result.result, RPMB_RESULT_WRITE_FAILURE);
            break;
        }
        memcpy(sd->rpmb.key, frame->key_mac, sizeof(sd->rpmb.key));
        sd->rpmb.key_set = 1;
        break;
    case RPMB_REQ_READ_WRITE_COUNTER:
        stl_be_p(&sd->rpmb.result.write_counter, sd->rpmb.write_counter);
        break;
    case RPMB_REQ_AUTH_DATA_WRITE:
        /* We only support single-block writes so far */
        if (sd->multi_blk_cnt != 1) {
            stw_be_p(&sd->rpmb.result.result, RPMB_RESULT_GENERAL_FAILURE);
            break;
        }
        if (sd->rpmb.write_counter == 0xffffffff) {
            stw_be_p(&sd->rpmb.result.result, RPMB_RESULT_WRITE_FAILURE);
            break;
        }
        if (!rpmb_calc_hmac(sd, frame, 1, mac) ||
            memcmp(frame->key_mac, mac, RPMB_KEY_MAC_LEN) != 0) {
            stw_be_p(&sd->rpmb.result.result, RPMB_RESULT_AUTH_FAILURE);
            break;
        }
        if (ldl_be_p(&frame->write_counter) != sd->rpmb.write_counter) {
            stw_be_p(&sd->rpmb.result.result, RPMB_RESULT_COUNTER_FAILURE);
            break;
        }
        sd->rpmb.result.address = frame->address;
        addr = lduw_be_p(&frame->address) * RPMB_DATA_LEN + sd_part_offset(sd);
        if (blk_pwrite(sd->blk, addr, RPMB_DATA_LEN, frame->data, 0) < 0) {
            error_report("sd_blk_write: write error on host side");
            stw_be_p(&sd->rpmb.result.result, RPMB_RESULT_WRITE_FAILURE);
        } else {
            sd->rpmb.write_counter++;
        }
        stl_be_p(&sd->rpmb.result.write_counter, sd->rpmb.write_counter);
        break;
    case RPMB_REQ_AUTH_DATA_READ:
        sd->rpmb.result.address = frame->address;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "RPMB request %d not implemented\n", req);
        stw_be_p(&sd->rpmb.result.result, RPMB_RESULT_GENERAL_FAILURE);
        break;
    }
exit:
    if (sd->rpmb.write_counter == 0xffffffff) {
        stw_be_p(&sd->rpmb.result.result,
                 lduw_be_p(&sd->rpmb.result.result) | RPMB_RESULT_COUTER_EXPIRED);
    }
    trace_sdcard_rpmb_write_block(req, lduw_be_p(&sd->rpmb.result.result));
}

static void sd_erase(SDState *sd)
{
    uint64_t erase_start = sd->erase_start;
    uint64_t erase_end = sd->erase_end;
    bool sdsc = true;
    uint64_t wpnum;
    uint64_t erase_addr;
    int erase_len = 1 << HWBLOCK_SHIFT;

    trace_sdcard_erase(sd->erase_start, sd->erase_end);
    if (sd->erase_start == INVALID_ADDRESS
            || sd->erase_end == INVALID_ADDRESS) {
        sd->card_status |= ERASE_SEQ_ERROR;
        sd->erase_start = INVALID_ADDRESS;
        sd->erase_end = INVALID_ADDRESS;
        return;
    }

    if (FIELD_EX32(sd->ocr, OCR, CARD_CAPACITY)) {
        /* High capacity memory card: erase units are 512 byte blocks */
        erase_start <<= HWBLOCK_SHIFT;
        erase_end <<= HWBLOCK_SHIFT;
        sdsc = false;
    }

    if (erase_start > sd->size || erase_end > sd->size) {
        sd->card_status |= OUT_OF_RANGE;
        sd->erase_start = INVALID_ADDRESS;
        sd->erase_end = INVALID_ADDRESS;
        return;
    }

    sd->erase_start = INVALID_ADDRESS;
    sd->erase_end = INVALID_ADDRESS;
    sd->csd[14] |= 0x40;

    memset(sd->data, 0xff, erase_len);
    for (erase_addr = erase_start; erase_addr <= erase_end;
         erase_addr += erase_len) {
        if (sdsc) {
            /* Only SDSC cards support write protect groups */
            wpnum = sd_addr_to_wpnum(erase_addr);
            assert(wpnum < sd->wp_group_bits);
            if (test_bit(wpnum, sd->wp_group_bmap)) {
                sd->card_status |= WP_ERASE_SKIP;
                continue;
            }
        }
        sd_blk_write(sd, erase_addr, erase_len);
    }
}

static uint32_t sd_wpbits(SDState *sd, uint64_t addr)
{
    uint32_t i, wpnum;
    uint32_t ret = 0;

    wpnum = sd_addr_to_wpnum(addr);

    for (i = 0; i < 32; i++, wpnum++, addr += WPGROUP_SIZE) {
        if (addr >= sd->size) {
            /*
             * If the addresses of the last groups are outside the valid range,
             * then the corresponding write protection bits shall be set to 0.
             */
            continue;
        }
        assert(wpnum < sd->wp_group_bits);
        if (test_bit(wpnum, sd->wp_group_bmap)) {
            ret |= (1 << i);
        }
    }

    return ret;
}

enum ExtCsdAccessMode {
    EXT_CSD_ACCESS_MODE_COMMAND_SET = 0,
    EXT_CSD_ACCESS_MODE_SET_BITS    = 1,
    EXT_CSD_ACCESS_MODE_CLEAR_BITS  = 2,
    EXT_CSD_ACCESS_MODE_WRITE_BYTE  = 3
};

static void emmc_function_switch(SDState *sd, uint32_t arg)
{
    uint8_t access = extract32(arg, 24, 2);
    uint8_t index = extract32(arg, 16, 8);
    uint8_t value = extract32(arg, 8, 8);
    uint8_t b = sd->ext_csd[index];

    trace_sdcard_switch(access, index, value, extract32(arg, 0, 2));

    if (index >= 192) {
        qemu_log_mask(LOG_GUEST_ERROR, "MMC switching illegal offset\n");
        sd->card_status |= R_CSR_SWITCH_ERROR_MASK;
        return;
    }

    switch (access) {
    case EXT_CSD_ACCESS_MODE_COMMAND_SET:
        qemu_log_mask(LOG_UNIMP, "MMC Command set switching not supported\n");
        return;
    case EXT_CSD_ACCESS_MODE_SET_BITS:
        b |= value;
        break;
    case EXT_CSD_ACCESS_MODE_CLEAR_BITS:
        b &= ~value;
        break;
    case EXT_CSD_ACCESS_MODE_WRITE_BYTE:
        b = value;
        break;
    }

    if (index == EXT_CSD_PART_CONFIG) {
        uint8_t part = b & EXT_CSD_PART_CONFIG_ACC_MASK;

        if (((part == EXT_CSD_PART_CONFIG_ACC_BOOT1 ||
              part == EXT_CSD_PART_CONFIG_ACC_BOOT2) && !sd->boot_part_size) ||
            (part == EXT_CSD_PART_CONFIG_ACC_RPMB && !sd->rpmb_part_size)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "MMC switching to illegal partition\n");
            sd->card_status |= R_CSR_SWITCH_ERROR_MASK;
            return;
        }
    }

    trace_sdcard_ext_csd_update(index, sd->ext_csd[index], b);
    sd->ext_csd[index] = b;
}

static void sd_function_switch(SDState *sd, uint32_t arg)
{
    int i, mode, new_func;
    mode = !!(arg & 0x80000000);

    sd->data[0] = 0x00;     /* Maximum current consumption */
    sd->data[1] = 0x01;
    sd->data[2] = 0x80;     /* Supported group 6 functions */
    sd->data[3] = 0x01;
    sd->data[4] = 0x80;     /* Supported group 5 functions */
    sd->data[5] = 0x01;
    sd->data[6] = 0x80;     /* Supported group 4 functions */
    sd->data[7] = 0x01;
    sd->data[8] = 0x80;     /* Supported group 3 functions */
    sd->data[9] = 0x01;
    sd->data[10] = 0x80;    /* Supported group 2 functions */
    sd->data[11] = 0x43;
    sd->data[12] = 0x80;    /* Supported group 1 functions */
    sd->data[13] = 0x03;

    memset(&sd->data[14], 0, 3);
    for (i = 0; i < 6; i ++) {
        new_func = (arg >> (i * 4)) & 0x0f;
        if (mode && new_func != 0x0f)
            sd->function_group[i] = new_func;
        sd->data[16 - (i >> 1)] |= new_func << ((i % 2) * 4);
    }
    memset(&sd->data[17], 0, 47);
}

static inline bool sd_wp_addr(SDState *sd, uint64_t addr)
{
    return test_bit(sd_addr_to_wpnum(addr), sd->wp_group_bmap);
}

static void sd_lock_command(SDState *sd)
{
    int erase, lock, clr_pwd, set_pwd, pwd_len;
    erase = !!(sd->data[0] & 0x08);
    lock = sd->data[0] & 0x04;
    clr_pwd = sd->data[0] & 0x02;
    set_pwd = sd->data[0] & 0x01;

    if (sd->blk_len > 1)
        pwd_len = sd->data[1];
    else
        pwd_len = 0;

    if (lock) {
        trace_sdcard_lock();
    } else {
        trace_sdcard_unlock();
    }
    if (erase) {
        if (!(sd->card_status & CARD_IS_LOCKED) || sd->blk_len > 1 ||
                        set_pwd || clr_pwd || lock || sd->wp_switch ||
                        (sd->csd[14] & 0x20)) {
            sd->card_status |= LOCK_UNLOCK_FAILED;
            return;
        }
        bitmap_zero(sd->wp_group_bmap, sd->wp_group_bits);
        sd->csd[14] &= ~0x10;
        sd->card_status &= ~CARD_IS_LOCKED;
        sd->pwd_len = 0;
        /* Erasing the entire card here! */
        fprintf(stderr, "SD: Card force-erased by CMD42\n");
        return;
    }

    if (sd->blk_len < 2 + pwd_len ||
                    pwd_len <= sd->pwd_len ||
                    pwd_len > sd->pwd_len + 16) {
        sd->card_status |= LOCK_UNLOCK_FAILED;
        return;
    }

    if (sd->pwd_len && memcmp(sd->pwd, sd->data + 2, sd->pwd_len)) {
        sd->card_status |= LOCK_UNLOCK_FAILED;
        return;
    }

    pwd_len -= sd->pwd_len;
    if ((pwd_len && !set_pwd) ||
                    (clr_pwd && (set_pwd || lock)) ||
                    (lock && !sd->pwd_len && !set_pwd) ||
                    (!set_pwd && !clr_pwd &&
                     (((sd->card_status & CARD_IS_LOCKED) && lock) ||
                      (!(sd->card_status & CARD_IS_LOCKED) && !lock)))) {
        sd->card_status |= LOCK_UNLOCK_FAILED;
        return;
    }

    if (set_pwd) {
        memcpy(sd->pwd, sd->data + 2 + sd->pwd_len, pwd_len);
        sd->pwd_len = pwd_len;
    }

    if (clr_pwd) {
        sd->pwd_len = 0;
    }

    if (lock)
        sd->card_status |= CARD_IS_LOCKED;
    else
        sd->card_status &= ~CARD_IS_LOCKED;
}

static bool address_in_range(SDState *sd, const char *desc,
                             uint64_t addr, uint32_t length)
{
    if (addr + length > sd->size) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s offset %"PRIu64" > card %"PRIu64" [%%%u]\n",
                      desc, addr, sd->size, length);
        sd->card_status |= ADDRESS_ERROR;
        return false;
    }
    return true;
}

static sd_rsp_type_t sd_invalid_state_for_cmd(SDState *sd, SDRequest req)
{
    qemu_log_mask(LOG_GUEST_ERROR, "%s: CMD%i in a wrong state: %s (spec %s)\n",
                  sd->proto->name, req.cmd, sd_state_name(sd->state),
                  sd_version_str(sd->spec_version));

    return sd_illegal;
}

static sd_rsp_type_t sd_invalid_mode_for_cmd(SDState *sd, SDRequest req)
{
    qemu_log_mask(LOG_GUEST_ERROR, "%s: CMD%i in a wrong mode: %s (spec %s)\n",
                  sd->proto->name, req.cmd, sd_mode_name(sd_mode(sd)),
                  sd_version_str(sd->spec_version));

    return sd_illegal;
}

static sd_rsp_type_t sd_cmd_illegal(SDState *sd, SDRequest req)
{
    qemu_log_mask(LOG_GUEST_ERROR, "%s: Unknown CMD%i for spec %s\n",
                  sd->proto->name, req.cmd,
                  sd_version_str(sd->spec_version));

    return sd_illegal;
}

/* Commands that are recognised but not yet implemented. */
static sd_rsp_type_t sd_cmd_unimplemented(SDState *sd, SDRequest req)
{
    qemu_log_mask(LOG_UNIMP, "%s: CMD%i not implemented\n",
                  sd->proto->name, req.cmd);

    return sd_illegal;
}

static sd_rsp_type_t sd_cmd_optional(SDState *sd, SDRequest req)
{
    qemu_log_mask(LOG_UNIMP, "%s: Optional CMD%i not implemented\n",
                  sd->proto->name, req.cmd);

    return sd_illegal;
}

/* Configure fields for following sd_generic_write_data() calls */
static sd_rsp_type_t sd_cmd_to_receivingdata(SDState *sd, SDRequest req,
                                             uint64_t start, size_t size)
{
    if (sd->state != sd_transfer_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }
    sd->state = sd_receivingdata_state;
    sd->data_start = start;
    sd->data_offset = 0;
    /* sd->data[] used as receive buffer */
    sd->data_size = size ?: sizeof(sd->data);
    return sd_r1;
}

/* Configure fields for following sd_generic_read_data() calls */
static sd_rsp_type_t sd_cmd_to_sendingdata(SDState *sd, SDRequest req,
                                           uint64_t start,
                                           const void *data, size_t size)
{
    if (sd->state != sd_transfer_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }

    sd->state = sd_sendingdata_state;
    sd->data_start = start;
    sd->data_offset = 0;
    if (data) {
        assert(size > 0 && size <= sizeof(sd->data));
        memcpy(sd->data, data, size);
    }
    if (size) {
        sd->data_size = size;
    }
    return sd_r1;
}

/* CMD0 */
static sd_rsp_type_t sd_cmd_GO_IDLE_STATE(SDState *sd, SDRequest req)
{
    if (sd->state == sd_sleep_state) {
        switch (req.arg) {
        case 0x00000000:
        case 0xf0f0f0f0:
            break;
        default:
            return sd_r0;
        }
    }
    if (sd->state != sd_inactive_state) {
        sd->state = sd_idle_state;
        sd_reset(DEVICE(sd));
    }

    return sd_is_spi(sd) ? sd_r1 : sd_r0;
}

/* CMD2 */
static sd_rsp_type_t sd_cmd_ALL_SEND_CID(SDState *sd, SDRequest req)
{
    switch (sd->state) {
    case sd_ready_state:
        sd->state = sd_identification_state;
        return sd_r2_i;
    default:
        return sd_invalid_state_for_cmd(sd, req);
    }
}

/* CMD3 */
static sd_rsp_type_t sd_cmd_SEND_RELATIVE_ADDR(SDState *sd, SDRequest req)
{
    uint16_t random_rca;

    switch (sd->state) {
    case sd_identification_state:
    case sd_standby_state:
        sd->state = sd_standby_state;
        qemu_guest_getrandom_nofail(&random_rca, sizeof(random_rca));
        sd_set_rca(sd, random_rca);
        return sd_r6;

    default:
        return sd_invalid_state_for_cmd(sd, req);
    }
}

static sd_rsp_type_t emmc_cmd_SET_RELATIVE_ADDR(SDState *sd, SDRequest req)
{
    switch (sd->state) {
    case sd_identification_state:
    case sd_standby_state:
        sd->state = sd_standby_state;
        sd_set_rca(sd, req.arg >> 16);
        return sd_r1;

    default:
        return sd_invalid_state_for_cmd(sd, req);
    }
}

/* CMD5 */
static sd_rsp_type_t emmc_cmd_sleep_awake(SDState *sd, SDRequest req)
{
    bool do_sleep = extract32(req.arg, 15, 1);

    switch (sd->state) {
    case sd_sleep_state:
        if (!do_sleep) {
            /* Awake */
            sd->state = sd_standby_state;
        }
        return sd_r1b;

    case sd_standby_state:
        if (do_sleep) {
            sd->state = sd_sleep_state;
        }
        return sd_r1b;

    default:
        return sd_invalid_state_for_cmd(sd, req);
    }
}

/* CMD6 */
static sd_rsp_type_t sd_cmd_SWITCH_FUNCTION(SDState *sd, SDRequest req)
{
    if (sd_mode(sd) != sd_data_transfer_mode) {
        return sd_invalid_mode_for_cmd(sd, req);
    }
    if (sd_is_spi(sd)) {
        if (sd->state == sd_idle_state) {
            return sd_invalid_state_for_cmd(sd, req);
        }
    } else {
        if (sd->state != sd_transfer_state) {
            return sd_invalid_state_for_cmd(sd, req);
        }
    }

    sd_function_switch(sd, req.arg);
    return sd_cmd_to_sendingdata(sd, req, 0, NULL, 64);
}

static sd_rsp_type_t emmc_cmd_SWITCH(SDState *sd, SDRequest req)
{
    switch (sd->state) {
    case sd_transfer_state:
        sd->state = sd_programming_state;
        emmc_function_switch(sd, req.arg);
        sd->state = sd_transfer_state;
        return sd_r1b;
    default:
        return sd_invalid_state_for_cmd(sd, req);
    }
}

/* CMD7 */
static sd_rsp_type_t sd_cmd_DE_SELECT_CARD(SDState *sd, SDRequest req)
{
    bool same_rca = sd_req_rca_same(sd, req);

    switch (sd->state) {
    case sd_standby_state:
        if (!same_rca) {
            return sd_r0;
        }
        sd->state = sd_transfer_state;
        return sd_r1b;

    case sd_transfer_state:
    case sd_sendingdata_state:
        if (same_rca) {
            break;
        }
        sd->state = sd_standby_state;
        return sd_r1b;

    case sd_disconnect_state:
        if (!same_rca) {
            return sd_r0;
        }
        sd->state = sd_programming_state;
        return sd_r1b;

    case sd_programming_state:
        if (same_rca) {
            break;
        }
        sd->state = sd_disconnect_state;
        return sd_r1b;

    default:
        break;
    }
    return sd_invalid_state_for_cmd(sd, req);
}

/* CMD8 */
static sd_rsp_type_t sd_cmd_SEND_IF_COND(SDState *sd, SDRequest req)
{
    if (sd->state != sd_idle_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }
    sd->vhs = 0;

    /* No response if not exactly one VHS bit is set.  */
    if (!(req.arg >> 8) || (req.arg >> (ctz32(req.arg & ~0xff) + 1))) {
        return sd_is_spi(sd) ? sd_r7 : sd_r0;
    }

    /* Accept.  */
    sd->vhs = req.arg;
    return sd_r7;
}

/* CMD8 */
static sd_rsp_type_t emmc_cmd_SEND_EXT_CSD(SDState *sd, SDRequest req)
{
    if (sd->state != sd_transfer_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }

    return sd_cmd_to_sendingdata(sd, req, sd_req_get_address(sd, req),
                                 sd->ext_csd, sizeof(sd->ext_csd));
}

static sd_rsp_type_t spi_cmd_SEND_CxD(SDState *sd, SDRequest req,
                                      const void *data, size_t size)
{
    /*
     * XXX as of v10.1.0-rc1 command is reached in sd_idle_state,
     * so disable this check.
    if (sd->state != sd_standby_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }
    */

    /*
     * Since SPI returns CSD and CID on the DAT lines,
     * switch to sd_transfer_state.
     */
    sd->state = sd_transfer_state;

    return sd_cmd_to_sendingdata(sd, req, 0, data, size);
}

/* CMD9 */
static sd_rsp_type_t spi_cmd_SEND_CSD(SDState *sd, SDRequest req)
{
    return spi_cmd_SEND_CxD(sd, req, sd->csd, sizeof(sd->csd));
}

static sd_rsp_type_t sd_cmd_SEND_CSD(SDState *sd, SDRequest req)
{
    if (sd->state != sd_standby_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }

    return sd_req_rca_same(sd, req) ? sd_r2_s : sd_r0;
}

/* CMD10 */
static sd_rsp_type_t spi_cmd_SEND_CID(SDState *sd, SDRequest req)
{
    return spi_cmd_SEND_CxD(sd, req, sd->cid, sizeof(sd->cid));
}

static sd_rsp_type_t sd_cmd_SEND_CID(SDState *sd, SDRequest req)
{
    if (sd->state != sd_standby_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }

    return sd_req_rca_same(sd, req) ? sd_r2_i : sd_r0;
}

/* CMD12 */
static sd_rsp_type_t sd_cmd_STOP_TRANSMISSION(SDState *sd, SDRequest req)
{
    switch (sd->state) {
    case sd_sendingdata_state:
        sd->state = sd_transfer_state;
        return sd_r1b;
    case sd_receivingdata_state:
        sd->state = sd_programming_state;
        /* Bzzzzzzztt .... Operation complete.  */
        sd->state = sd_transfer_state;
        return sd_r1;
    default:
        return sd_invalid_state_for_cmd(sd, req);
    }
}

/* CMD13 */
static sd_rsp_type_t sd_cmd_SEND_STATUS(SDState *sd, SDRequest req)
{
    if (sd_mode(sd) != sd_data_transfer_mode) {
        return sd_invalid_mode_for_cmd(sd, req);
    }

    switch (sd->state) {
    case sd_standby_state:
    case sd_transfer_state:
    case sd_sendingdata_state:
    case sd_receivingdata_state:
    case sd_programming_state:
    case sd_disconnect_state:
        break;
    default:
        return sd_invalid_state_for_cmd(sd, req);
    }

    if (sd_is_spi(sd)) {
        return spi_r2;
    }

    return sd_req_rca_same(sd, req) ? sd_r1 : sd_r0;
}

/* CMD15 */
static sd_rsp_type_t sd_cmd_GO_INACTIVE_STATE(SDState *sd, SDRequest req)
{
    if (sd_mode(sd) != sd_data_transfer_mode) {
        return sd_invalid_mode_for_cmd(sd, req);
    }
    switch (sd->state) {
    case sd_standby_state:
    case sd_transfer_state:
    case sd_sendingdata_state:
    case sd_receivingdata_state:
    case sd_programming_state:
    case sd_disconnect_state:
        break;
    default:
        return sd_invalid_state_for_cmd(sd, req);
    }
    if (sd_req_rca_same(sd, req)) {
        sd->state = sd_inactive_state;
    }

    return sd_r0;
}

/* CMD16 */
static sd_rsp_type_t sd_cmd_SET_BLOCKLEN(SDState *sd, SDRequest req)
{
    if (sd->state != sd_transfer_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }
    if (req.arg > (1 << HWBLOCK_SHIFT)) {
        sd->card_status |= BLOCK_LEN_ERROR;
    } else {
        trace_sdcard_set_blocklen(req.arg);
        sd->blk_len = req.arg;
    }

    return sd_r1;
}

/* CMD17 */
static sd_rsp_type_t sd_cmd_READ_SINGLE_BLOCK(SDState *sd, SDRequest req)
{
    uint64_t addr;

    if (sd->state != sd_transfer_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }

    addr = sd_req_get_address(sd, req);
    if (!address_in_range(sd, "READ_SINGLE_BLOCK", addr, sd->blk_len)) {
        return sd_r1;
    }

    sd_blk_read(sd, addr, sd->blk_len);
    return sd_cmd_to_sendingdata(sd, req, addr, NULL, sd->blk_len);
}

/* CMD19 */
static sd_rsp_type_t sd_cmd_SEND_TUNING_BLOCK(SDState *sd, SDRequest req)
{
    if (sd->spec_version < SD_PHY_SPECv3_01_VERS) {
        return sd_cmd_illegal(sd, req);
    }

    return sd_cmd_to_sendingdata(sd, req, 0,
                                 sd_tuning_block_pattern4,
                                 sizeof(sd_tuning_block_pattern4));
}

/* CMD23 */
static sd_rsp_type_t sd_cmd_SET_BLOCK_COUNT(SDState *sd, SDRequest req)
{
    if (sd->spec_version < SD_PHY_SPECv3_01_VERS) {
        return sd_cmd_illegal(sd, req);
    }

    if (sd->state != sd_transfer_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }

    sd->multi_blk_cnt = req.arg;
    if (sd_is_emmc(sd)) {
        sd->multi_blk_cnt &= 0xffff;
    }
    trace_sdcard_set_block_count(sd->multi_blk_cnt);

    return sd_r1;
}

/* CMD24 */
static sd_rsp_type_t sd_cmd_WRITE_SINGLE_BLOCK(SDState *sd, SDRequest req)
{
    uint64_t addr;

    if (sd->state != sd_transfer_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }

    addr = sd_req_get_address(sd, req);
    if (!address_in_range(sd, "WRITE_SINGLE_BLOCK", addr, sd->blk_len)) {
        return sd_r1;
    }

    if (sd->size <= SDSC_MAX_CAPACITY) {
        if (sd_wp_addr(sd, addr)) {
            sd->card_status |= WP_VIOLATION;
        }
    }
    if (sd->csd[14] & 0x30) {
        sd->card_status |= WP_VIOLATION;
    }

    sd->blk_written = 0;
    return sd_cmd_to_receivingdata(sd, req, addr, sd->blk_len);
}

/* CMD26 */
static sd_rsp_type_t emmc_cmd_PROGRAM_CID(SDState *sd, SDRequest req)
{
    return sd_cmd_to_receivingdata(sd, req, 0, sizeof(sd->cid));
}

/* CMD27 */
static sd_rsp_type_t sd_cmd_PROGRAM_CSD(SDState *sd, SDRequest req)
{
    return sd_cmd_to_receivingdata(sd, req, 0, sizeof(sd->csd));
}

static sd_rsp_type_t sd_cmd_SET_CLR_WRITE_PROT(SDState *sd, SDRequest req,
                                               bool is_write)
{
    uint64_t addr;

    if (sd->size > SDSC_MAX_CAPACITY) {
        return sd_illegal;
    }

    if (sd->state != sd_transfer_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }

    addr = sd_req_get_address(sd, req);
    if (!address_in_range(sd, is_write ? "SET_WRITE_PROT" : "CLR_WRITE_PROT",
                          addr, 1)) {
        return sd_r1b;
    }

    sd->state = sd_programming_state;
    if (is_write) {
        set_bit(sd_addr_to_wpnum(addr), sd->wp_group_bmap);
    } else {
        clear_bit(sd_addr_to_wpnum(addr), sd->wp_group_bmap);
    }
    /* Bzzzzzzztt .... Operation complete.  */
    sd->state = sd_transfer_state;
    return sd_r1;
}

/* CMD28 */
static sd_rsp_type_t sd_cmd_SET_WRITE_PROT(SDState *sd, SDRequest req)
{
    return sd_cmd_SET_CLR_WRITE_PROT(sd, req, true);
}

/* CMD29 */
static sd_rsp_type_t sd_cmd_CLR_WRITE_PROT(SDState *sd, SDRequest req)
{
    return sd_cmd_SET_CLR_WRITE_PROT(sd, req, false);
}

/* CMD30 */
static sd_rsp_type_t sd_cmd_SEND_WRITE_PROT(SDState *sd, SDRequest req)
{
    uint64_t addr;
    uint32_t data;

    if (sd->size > SDSC_MAX_CAPACITY) {
        return sd_illegal;
    }

    if (sd->state != sd_transfer_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }

    addr = sd_req_get_address(sd, req);
    if (!address_in_range(sd, "SEND_WRITE_PROT", addr, sd->blk_len)) {
        return sd_r1;
    }

    data = sd_wpbits(sd, req.arg);
    return sd_cmd_to_sendingdata(sd, req, addr, &data, sizeof(data));
}

/* CMD32 */
static sd_rsp_type_t sd_cmd_ERASE_WR_BLK_START(SDState *sd, SDRequest req)
{
    if (sd->state != sd_transfer_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }
    sd->erase_start = req.arg;
    return sd_r1;
}

/* CMD33 */
static sd_rsp_type_t sd_cmd_ERASE_WR_BLK_END(SDState *sd, SDRequest req)
{
    if (sd->state != sd_transfer_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }
    sd->erase_end = req.arg;
    return sd_r1;
}

/* CMD38 */
static sd_rsp_type_t sd_cmd_ERASE(SDState *sd, SDRequest req)
{
    if (sd->state != sd_transfer_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }
    if (sd->csd[14] & 0x30) {
        sd->card_status |= WP_VIOLATION;
        return sd_r1b;
    }

    sd->state = sd_programming_state;
    sd_erase(sd);
    /* Bzzzzzzztt .... Operation complete.  */
    sd->state = sd_transfer_state;
    return sd_r1b;
}

/* CMD42 */
static sd_rsp_type_t sd_cmd_LOCK_UNLOCK(SDState *sd, SDRequest req)
{
    return sd_cmd_to_receivingdata(sd, req, 0, 0);
}

/* CMD55 */
static sd_rsp_type_t sd_cmd_APP_CMD(SDState *sd, SDRequest req)
{
    switch (sd->state) {
    case sd_ready_state:
    case sd_identification_state:
    case sd_inactive_state:
    case sd_sleep_state:
        return sd_invalid_state_for_cmd(sd, req);
    case sd_idle_state:
        if (!sd_is_spi(sd) && sd_req_get_rca(sd, req) != 0x0000) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "SD: illegal RCA 0x%04x for APP_CMD\n", req.cmd);
        }
        /* fall-through */
    default:
        break;
    }
    if (!sd_is_spi(sd) && !sd_req_rca_same(sd, req)) {
        return sd_r0;
    }
    sd->expecting_acmd = true;
    sd->card_status |= APP_CMD;

    return sd_r1;
}

/* CMD56 */
static sd_rsp_type_t sd_cmd_GEN_CMD(SDState *sd, SDRequest req)
{
    if (sd->state != sd_transfer_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }

    /* Vendor specific command: our model is RAZ/WI */
    if (req.arg & 1) {
        memset(sd->data, 0, sizeof(sd->data));
        return sd_cmd_to_sendingdata(sd, req, 0, NULL, 0);
    } else {
        return sd_cmd_to_receivingdata(sd, req, 0, 0);
    }
}

/* CMD58 */
static sd_rsp_type_t spi_cmd_READ_OCR(SDState *sd, SDRequest req)
{
    return sd_r3;
}

/* CMD59 */
static sd_rsp_type_t spi_cmd_CRC_ON_OFF(SDState *sd, SDRequest req)
{
    return sd_r1;
}

/* ACMD6 */
static sd_rsp_type_t sd_acmd_SET_BUS_WIDTH(SDState *sd, SDRequest req)
{
    if (sd->state != sd_transfer_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }

    sd->sd_status[0] &= 0x3f;
    sd->sd_status[0] |= (req.arg & 0x03) << 6;
    return sd_r1;
}

/* ACMD13 */
static sd_rsp_type_t sd_acmd_SD_STATUS(SDState *sd, SDRequest req)
{
    sd_rsp_type_t rsp;

    rsp = sd_cmd_to_sendingdata(sd, req, 0,
                                sd->sd_status, sizeof(sd->sd_status));
    if (sd_is_spi(sd) && rsp != sd_illegal) {
        return spi_r2;
    }
    return rsp;
}

/* ACMD22 */
static sd_rsp_type_t sd_acmd_SEND_NUM_WR_BLOCKS(SDState *sd, SDRequest req)
{
    return sd_cmd_to_sendingdata(sd, req, 0,
                                 &sd->blk_written, sizeof(sd->blk_written));
}

/* ACMD23 */
static sd_rsp_type_t sd_acmd_SET_WR_BLK_ERASE_COUNT(SDState *sd, SDRequest req)
{
    if (sd->state != sd_transfer_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }
    return sd_r1;
}

/* ACMD41 */
static sd_rsp_type_t sd_cmd_SEND_OP_COND(SDState *sd, SDRequest req)
{
    if (sd->state != sd_idle_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }

    /*
     * If it's the first ACMD41 since reset, we need to decide
     * whether to power up. If this is not an enquiry ACMD41,
     * we immediately report power on and proceed below to the
     * ready state, but if it is, we set a timer to model a
     * delay for power up. This works around a bug in EDK2
     * UEFI, which sends an initial enquiry ACMD41, but
     * assumes that the card is in ready state as soon as it
     * sees the power up bit set.
     */
    if (!FIELD_EX32(sd->ocr, OCR, CARD_POWER_UP)) {
        if ((req.arg & ACMD41_ENQUIRY_MASK) != 0) {
            timer_del(sd->ocr_power_timer);
            sd_ocr_powerup(sd);
        } else {
            trace_sdcard_inquiry_cmd41();
            if (!timer_pending(sd->ocr_power_timer)) {
                timer_mod_ns(sd->ocr_power_timer,
                             (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                              + OCR_POWER_DELAY_NS));
            }
        }
    }

    if (sd_is_spi(sd)) {
        sd->state = sd_ready_state;
        return sd_r1;
    } else {
        if (FIELD_EX32(sd->ocr & req.arg, OCR, VDD_VOLTAGE_WINDOW)) {
            /*
             * We accept any voltage.  10000 V is nothing.
             *
             * Once we're powered up, we advance straight to ready state
             * unless it's an enquiry ACMD41 (bits 23:0 == 0).
             */
            sd->state = sd_ready_state;
        }
        return sd_r3;
    }
}

/* ACMD42 */
static sd_rsp_type_t sd_acmd_SET_CLR_CARD_DETECT(SDState *sd, SDRequest req)
{
    if (sd->state != sd_transfer_state) {
        return sd_invalid_state_for_cmd(sd, req);
    }

    /* Bringing in the 50KOhm pull-up resistor... Done.  */
    return sd_r1;
}

/* ACMD51 */
static sd_rsp_type_t sd_acmd_SEND_SCR(SDState *sd, SDRequest req)
{
    return sd_cmd_to_sendingdata(sd, req, 0, sd->scr, sizeof(sd->scr));
}

static sd_rsp_type_t sd_normal_command(SDState *sd, SDRequest req)
{
    uint64_t addr;

    sd->last_cmd_name = sd_cmd_name(sd, req.cmd);
    /* CMD55 precedes an ACMD, so we are not interested in tracing it.
     * However there is no ACMD55, so we want to trace this particular case.
     */
    if (req.cmd != 55 || sd->expecting_acmd) {
        trace_sdcard_normal_command(sd->proto->name,
                                    sd->last_cmd_name, req.cmd,
                                    req.arg,
                                    sd_mode_name(sd_mode(sd)),
                                    sd_state_name(sd->state));
    }

    /* Not interpreting this as an app command */
    sd->card_status &= ~APP_CMD;

    /* CMD23 (set block count) must be immediately followed by CMD18 or CMD25
     * if not, its effects are cancelled */
    if (sd->multi_blk_cnt != 0 && !(req.cmd == 18 || req.cmd == 25)) {
        sd->multi_blk_cnt = 0;
    }

    if (sd->proto->cmd[req.cmd].class == 6 && FIELD_EX32(sd->ocr, OCR,
                                                         CARD_CAPACITY)) {
        /* Only Standard Capacity cards support class 6 commands */
        return sd_illegal;
    }

    if (sd->proto->cmd[req.cmd].handler) {
        return sd->proto->cmd[req.cmd].handler(sd, req);
    }

    switch (req.cmd) {
    /* Block read commands (Class 2) */
    case 18:  /* CMD18:  READ_MULTIPLE_BLOCK */
        addr = sd_req_get_address(sd, req);
        switch (sd->state) {
        case sd_transfer_state:

            if (!address_in_range(sd, "READ_BLOCK", addr, sd->blk_len)) {
                return sd_r1;
            }

            sd->state = sd_sendingdata_state;
            sd->data_start = addr;
            sd->data_offset = 0;
            return sd_r1;

        default:
            break;
        }
        break;

    /* Block write commands (Class 4) */
    case 25:  /* CMD25:  WRITE_MULTIPLE_BLOCK */
        addr = sd_req_get_address(sd, req);
        switch (sd->state) {
        case sd_transfer_state:

            if (!address_in_range(sd, "WRITE_BLOCK", addr, sd->blk_len)) {
                return sd_r1;
            }

            sd->state = sd_receivingdata_state;
            sd->data_start = addr;
            sd->data_offset = 0;
            sd->blk_written = 0;

            if (sd->size <= SDSC_MAX_CAPACITY) {
                if (sd_wp_addr(sd, sd->data_start)) {
                    sd->card_status |= WP_VIOLATION;
                }
            }
            if (sd->csd[14] & 0x30) {
                sd->card_status |= WP_VIOLATION;
            }
            return sd_r1;

        default:
            break;
        }
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "SD: Unknown CMD%i\n", req.cmd);
        return sd_illegal;
    }

    return sd_invalid_state_for_cmd(sd, req);
}

static sd_rsp_type_t sd_app_command(SDState *sd,
                                    SDRequest req)
{
    sd->last_cmd_name = sd_acmd_name(sd, req.cmd);
    trace_sdcard_app_command(sd->proto->name, sd->last_cmd_name,
                             req.cmd, req.arg,
                             sd_mode_name(sd_mode(sd)),
                             sd_state_name(sd->state));
    sd->card_status |= APP_CMD;

    if (sd->proto->acmd[req.cmd].handler) {
        return sd->proto->acmd[req.cmd].handler(sd, req);
    }

    switch (req.cmd) {
    case 18:    /* Reserved for SD security applications */
    case 25:
    case 26:
    case 38:
    case 43 ... 49:
        /* Refer to the "SD Specifications Part3 Security Specification" for
         * information about the SD Security Features.
         */
        qemu_log_mask(LOG_UNIMP, "SD: CMD%i Security not implemented\n",
                      req.cmd);
        return sd_illegal;

    default:
        /* Fall back to standard commands.  */
        return sd_normal_command(sd, req);
    }

    qemu_log_mask(LOG_GUEST_ERROR, "SD: ACMD%i in a wrong state\n", req.cmd);
    return sd_illegal;
}

static bool cmd_valid_while_locked(SDState *sd, unsigned cmd)
{
    unsigned cmd_class;

    /* Valid commands in locked state:
     * basic class (0)
     * lock card class (7)
     * CMD16
     * implicitly, the ACMD prefix CMD55
     * ACMD41 and ACMD42
     * Anything else provokes an "illegal command" response.
     */
    if (sd->expecting_acmd) {
        return cmd == 41 || cmd == 42;
    }
    if (cmd == 16 || cmd == 55) {
        return true;
    }
    if (!sd->proto->cmd[cmd].handler) {
        return false;
    }
    cmd_class = sd->proto->cmd[cmd].class;

    return cmd_class == 0 || cmd_class == 7;
}

static size_t sd_do_command(SDState *sd, SDRequest *req,
                            uint8_t *response, size_t respsz)
{
    int last_state;
    sd_rsp_type_t rtype;
    int rsplen;

    if (!sd->blk || !blk_is_inserted(sd->blk)) {
        return 0;
    }

    if (sd->state == sd_inactive_state) {
        rtype = sd_illegal;
        goto send_response;
    }

    if (sd_req_crc_validate(req)) {
        sd->card_status |= COM_CRC_ERROR;
        rtype = sd_illegal;
        goto send_response;
    }

    if (req->cmd >= SDMMC_CMD_MAX) {
        qemu_log_mask(LOG_GUEST_ERROR, "SD: incorrect command 0x%02x\n",
                      req->cmd);
        req->cmd &= 0x3f;
    }

    if (sd->state == sd_sleep_state && req->cmd) {
        qemu_log_mask(LOG_GUEST_ERROR, "SD: Card is sleeping\n");
        rtype = sd_r0;
        goto send_response;
    }

    if (sd->card_status & CARD_IS_LOCKED) {
        if (!cmd_valid_while_locked(sd, req->cmd)) {
            sd->card_status |= ILLEGAL_COMMAND;
            sd->expecting_acmd = false;
            qemu_log_mask(LOG_GUEST_ERROR, "SD: Card is locked\n");
            rtype = sd_illegal;
            goto send_response;
        }
    }

    last_state = sd->state;

    if (sd->expecting_acmd) {
        sd->expecting_acmd = false;
        rtype = sd_app_command(sd, *req);
    } else {
        rtype = sd_normal_command(sd, *req);
    }

    if (rtype == sd_illegal) {
        sd->card_status |= ILLEGAL_COMMAND;
    } else {
        /* Valid command, we can update the 'state before command' bits.
         * (Do this now so they appear in r1 responses.)
         */
        sd->card_status = FIELD_DP32(sd->card_status, CSR,
                                     CURRENT_STATE, last_state);
    }

send_response:
    rsplen = sd_response_size(sd, rtype);
    assert(rsplen <= respsz);

    switch (rtype) {
    case sd_r1:
    case sd_r1b:
        sd_response_r1_make(sd, response);
        break;

    case spi_r2:
        spi_response_r2_make(sd, response);
        break;

    case sd_r2_i:
        memcpy(response, sd->cid, sizeof(sd->cid));
        break;

    case sd_r2_s:
        memcpy(response, sd->csd, sizeof(sd->csd));
        break;

    case sd_r3:
        sd_response_r3_make(sd, response);
        break;

    case sd_r6:
        sd_response_r6_make(sd, response);
        break;

    case sd_r7:
        sd_response_r7_make(sd, response);
        break;

    case sd_r0:
        /*
         * Invalid state transition, reset implementation
         * fields to avoid OOB abuse.
         */
        sd->data_start = 0;
        sd->data_offset = 0;
        /* fall-through */
    case sd_illegal:
        break;
    default:
        g_assert_not_reached();
    }
    trace_sdcard_response(sd_response_name(rtype), rsplen);

    if (rtype != sd_illegal) {
        /* Clear the "clear on valid command" status bits now we've
         * sent any response
         */
        sd->card_status &= ~CARD_STATUS_B;
    }

#ifdef DEBUG_SD
    qemu_hexdump(stderr, "Response", response, rsplen);
#endif

    sd->current_cmd = rtype == sd_illegal ? 0 : req->cmd;

    return rsplen;
}

/* Return true if buffer is consumed. Configured by sd_cmd_to_receivingdata() */
static bool sd_generic_write_data(SDState *sd, const void *buf, size_t *len)
{
    size_t to_write = MIN(sd->data_size - sd->data_offset, *len);

    memcpy(&sd->data[sd->data_offset], buf, to_write);
    sd->data_offset += to_write;
    *len = to_write;

    if (sd->data_offset >= sd->data_size) {
        sd->state = sd_transfer_state;
        return true;
    }
    return false;
}

/* Return true when buffer is consumed. Configured by sd_cmd_to_sendingdata() */
static bool sd_generic_read_data(SDState *sd, void *buf, size_t *len)
{
    size_t to_read = MIN(sd->data_size - sd->data_offset, *len);

    memcpy(buf, &sd->data[sd->data_offset], to_read);
    sd->data_offset += to_read;
    *len = to_read;

    if (sd->data_offset >= sd->data_size) {
        sd->state = sd_transfer_state;
        return true;
    }

    return false;
}

static void sdcard_write_data_dump(const char *proto, const char *cmd_desc,
                                   uint8_t cmd, uint32_t offset,
                                   const void *buf, size_t len)
{
    g_autoptr(GString) str = NULL;

    if (trace_event_get_state_backends(TRACE_SDCARD_WRITE_DATA)) {
        str = qemu_hexdump_line(NULL, buf, len, 8, 0);
        trace_sdcard_write_data(proto, cmd_desc, cmd, offset, str->str);
    }
}

static size_t sd_write_data(SDState *sd, const void *buf, size_t length)
{
    unsigned int partition_access;
    int i;
    const uint8_t *value = buf;

    if (!sd->blk || !blk_is_inserted(sd->blk)) {
        return length;
    }

    if (sd->state != sd_receivingdata_state) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: not in Receiving-Data state\n", __func__);
        return length;
    }

    if (sd->card_status & (ADDRESS_ERROR | WP_VIOLATION))
        return length;

    sdcard_write_data_dump(sd->proto->name,
                           sd->last_cmd_name,
                           sd->current_cmd, sd->data_offset, buf, length);
    switch (sd->current_cmd) {
    case 24:  /* CMD24:  WRITE_SINGLE_BLOCK */
        if (sd_generic_write_data(sd, buf, &length)) {
            /* TODO: Check CRC before committing */
            sd->state = sd_programming_state;
            sd_blk_write(sd, sd->data_start, sd->data_offset);
            sd->blk_written ++;
            sd->csd[14] |= 0x40;
            /* Bzzzzzzztt .... Operation complete.  */
            sd->state = sd_transfer_state;
        }
        break;

    case 25:  /* CMD25:  WRITE_MULTIPLE_BLOCK */
        /*
         * Only read one byte at a time. We will be called again with the
         * remaining.
         */
        length = 1;

        if (sd->data_offset == 0) {
            /* Start of the block - let's check the address is valid */
            if (!address_in_range(sd, "WRITE_MULTIPLE_BLOCK",
                                  sd->data_start, sd->blk_len)) {
                break;
            }
            if (sd->size <= SDSC_MAX_CAPACITY) {
                if (sd_wp_addr(sd, sd->data_start)) {
                    sd->card_status |= WP_VIOLATION;
                    break;
                }
            }
        }
        sd->data[sd->data_offset++] = value[0];
        if (sd->data_offset >= sd->blk_len) {
            /* TODO: Check CRC before committing */
            sd->state = sd_programming_state;
            partition_access = sd->ext_csd[EXT_CSD_PART_CONFIG]
                    & EXT_CSD_PART_CONFIG_ACC_MASK;
            if (partition_access == EXT_CSD_PART_CONFIG_ACC_RPMB) {
                emmc_rpmb_blk_write(sd, sd->data_start, sd->data_offset);
            } else {
                sd_blk_write(sd, sd->data_start, sd->data_offset);
            }
            sd->blk_written++;
            sd->data_start += sd->blk_len;
            sd->data_offset = 0;
            sd->csd[14] |= 0x40;

            /* Bzzzzzzztt .... Operation complete.  */
            if (sd->multi_blk_cnt != 0) {
                if (--sd->multi_blk_cnt == 0) {
                    /* Stop! */
                    sd->state = sd_transfer_state;
                    break;
                }
            }

            sd->state = sd_receivingdata_state;
        }
        break;

    case 26:  /* CMD26:  PROGRAM_CID */
        if (sd_generic_write_data(sd, buf, &length)) {
            /* TODO: Check CRC before committing */
            sd->state = sd_programming_state;
            for (i = 0; i < sizeof(sd->cid); i ++)
                if ((sd->cid[i] | 0x00) != sd->data[i])
                    sd->card_status |= CID_CSD_OVERWRITE;

            if (!(sd->card_status & CID_CSD_OVERWRITE))
                for (i = 0; i < sizeof(sd->cid); i ++) {
                    sd->cid[i] |= 0x00;
                    sd->cid[i] &= sd->data[i];
                }
            /* Bzzzzzzztt .... Operation complete.  */
            sd->state = sd_transfer_state;
        }
        break;

    case 27:  /* CMD27:  PROGRAM_CSD */
        if (sd_generic_write_data(sd, buf, &length)) {
            /* TODO: Check CRC before committing */
            sd->state = sd_programming_state;
            for (i = 0; i < sizeof(sd->csd); i ++)
                if ((sd->csd[i] | sd_csd_rw_mask[i]) !=
                    (sd->data[i] | sd_csd_rw_mask[i]))
                    sd->card_status |= CID_CSD_OVERWRITE;

            /* Copy flag (OTP) & Permanent write protect */
            if (sd->csd[14] & ~sd->data[14] & 0x60)
                sd->card_status |= CID_CSD_OVERWRITE;

            if (!(sd->card_status & CID_CSD_OVERWRITE))
                for (i = 0; i < sizeof(sd->csd); i ++) {
                    sd->csd[i] |= sd_csd_rw_mask[i];
                    sd->csd[i] &= sd->data[i];
                }
            /* Bzzzzzzztt .... Operation complete.  */
            sd->state = sd_transfer_state;
        }
        break;

    case 42:  /* CMD42:  LOCK_UNLOCK */
        if (sd_generic_write_data(sd, buf, &length)) {
            /* TODO: Check CRC before committing */
            sd->state = sd_programming_state;
            sd_lock_command(sd);
            /* Bzzzzzzztt .... Operation complete.  */
            sd->state = sd_transfer_state;
        }
        break;

    case 56:  /* CMD56:  GEN_CMD */
        sd_generic_write_data(sd, buf, &length);
        break;

    default:
        g_assert_not_reached();
    }

    return length;
}

static size_t sd_read_data(SDState *sd, void *buf, size_t length)
{
    /* TODO: Append CRCs */
    const uint8_t dummy_byte = 0x00;
    unsigned int partition_access;
    uint32_t io_len;
    uint8_t *value = buf;

    if (!sd->blk || !blk_is_inserted(sd->blk)) {
        memset(buf, dummy_byte, length);
        return length;
    }

    if (sd->state != sd_sendingdata_state) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: not in Sending-Data state\n", __func__);
        memset(buf, dummy_byte, length);
        return length;
    }

    if (sd->card_status & (ADDRESS_ERROR | WP_VIOLATION)) {
        memset(buf, dummy_byte, length);
        return length;
    }

    io_len = sd_blk_len(sd);

    trace_sdcard_read_data(sd->proto->name,
                           sd->last_cmd_name, sd->current_cmd,
                           sd->data_offset, sd->data_size, io_len);
    switch (sd->current_cmd) {
    case 6:  /* CMD6:   SWITCH_FUNCTION */
    case 8:  /* CMD8:   SEND_EXT_CSD */
    case 9:  /* CMD9:   SEND_CSD */
    case 10: /* CMD10:  SEND_CID */
    case 13: /* ACMD13: SD_STATUS */
    case 17: /* CMD17:  READ_SINGLE_BLOCK */
    case 19: /* CMD19:  SEND_TUNING_BLOCK (SD) */
    case 22: /* ACMD22: SEND_NUM_WR_BLOCKS */
    case 30: /* CMD30:  SEND_WRITE_PROT */
    case 51: /* ACMD51: SEND_SCR */
    case 56: /* CMD56:  GEN_CMD */
        sd_generic_read_data(sd, buf, &length);
        break;

    case 18:  /* CMD18:  READ_MULTIPLE_BLOCK */
        /*
         * We will only read one byte at a time. We will be called again with
         * the remaining buffer.
         */
        length = 1;

        if (sd->data_offset == 0) {
            if (!address_in_range(sd, "READ_MULTIPLE_BLOCK",
                                  sd->data_start, io_len)) {
                *value = dummy_byte;
                return length;
            }
            partition_access = sd->ext_csd[EXT_CSD_PART_CONFIG]
                    & EXT_CSD_PART_CONFIG_ACC_MASK;
            if (partition_access == EXT_CSD_PART_CONFIG_ACC_RPMB) {
                emmc_rpmb_blk_read(sd, sd->data_start, io_len);
            } else {
                sd_blk_read(sd, sd->data_start, io_len);
            }
        }
        *value = sd->data[sd->data_offset++];

        if (sd->data_offset >= io_len) {
            sd->data_start += io_len;
            sd->data_offset = 0;

            if (sd->multi_blk_cnt != 0) {
                if (--sd->multi_blk_cnt == 0) {
                    /* Stop! */
                    sd->state = sd_transfer_state;
                    break;
                }
            }
        }
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: DAT read illegal for command %s\n",
                                       __func__, sd->last_cmd_name);
        memset(buf, dummy_byte, length);
    }

    return length;
}

static bool sd_receive_ready(SDState *sd)
{
    return sd->state == sd_receivingdata_state;
}

static bool sd_data_ready(SDState *sd)
{
    return sd->state == sd_sendingdata_state;
}

static const SDProto sd_proto_spi = {
    .name = "SPI",
    .cmd = {
        [0]  = {0,  sd_spi, "GO_IDLE_STATE", sd_cmd_GO_IDLE_STATE},
        [1]  = {0,  sd_spi, "SEND_OP_COND", sd_cmd_SEND_OP_COND},
        [5]  = {9,  sd_spi, "IO_SEND_OP_COND", sd_cmd_optional},
        [6]  = {10, sd_spi, "SWITCH_FUNCTION", sd_cmd_SWITCH_FUNCTION},
        [8]  = {0,  sd_spi, "SEND_IF_COND", sd_cmd_SEND_IF_COND},
        [9]  = {0,  sd_spi, "SEND_CSD", spi_cmd_SEND_CSD},
        [10] = {0,  sd_spi, "SEND_CID", spi_cmd_SEND_CID},
        [12] = {0,  sd_spi, "STOP_TRANSMISSION", sd_cmd_STOP_TRANSMISSION},
        [13] = {0,  sd_spi, "SEND_STATUS", sd_cmd_SEND_STATUS},
        [16] = {2,  sd_spi, "SET_BLOCKLEN", sd_cmd_SET_BLOCKLEN},
        [17] = {2,  sd_spi, "READ_SINGLE_BLOCK", sd_cmd_READ_SINGLE_BLOCK},
        [24] = {4,  sd_spi, "WRITE_SINGLE_BLOCK", sd_cmd_WRITE_SINGLE_BLOCK},
        [27] = {4,  sd_spi, "PROGRAM_CSD", sd_cmd_PROGRAM_CSD},
        [28] = {6,  sd_spi, "SET_WRITE_PROT", sd_cmd_SET_WRITE_PROT},
        [29] = {6,  sd_spi, "CLR_WRITE_PROT", sd_cmd_CLR_WRITE_PROT},
        [30] = {6,  sd_spi, "SEND_WRITE_PROT", sd_cmd_SEND_WRITE_PROT},
        [32] = {5,  sd_spi, "ERASE_WR_BLK_START", sd_cmd_ERASE_WR_BLK_START},
        [33] = {5,  sd_spi, "ERASE_WR_BLK_END", sd_cmd_ERASE_WR_BLK_END},
        [34] = {10, sd_spi, "READ_SEC_CMD", sd_cmd_optional},
        [35] = {10, sd_spi, "WRITE_SEC_CMD", sd_cmd_optional},
        [36] = {10, sd_spi, "SEND_PSI", sd_cmd_optional},
        [37] = {10, sd_spi, "CONTROL_ASSD_SYSTEM", sd_cmd_optional},
        [38] = {5,  sd_spi, "ERASE", sd_cmd_ERASE},
        [42] = {7,  sd_spi, "LOCK_UNLOCK", sd_cmd_LOCK_UNLOCK},
        [50] = {10, sd_spi, "DIRECT_SECURE_READ", sd_cmd_optional},
        [52] = {9,  sd_spi, "IO_RW_DIRECT", sd_cmd_optional},
        [53] = {9,  sd_spi, "IO_RW_EXTENDED", sd_cmd_optional},
        [55] = {8,  sd_spi, "APP_CMD", sd_cmd_APP_CMD},
        [56] = {8,  sd_spi, "GEN_CMD", sd_cmd_GEN_CMD},
        [57] = {10, sd_spi, "DIRECT_SECURE_WRITE", sd_cmd_optional},
        [58] = {0,  sd_spi, "READ_OCR", spi_cmd_READ_OCR},
        [59] = {0,  sd_spi, "CRC_ON_OFF", spi_cmd_CRC_ON_OFF},
    },
    .acmd = {
        [13] = {8,  sd_spi, "SD_STATUS", sd_acmd_SD_STATUS},
        [22] = {8,  sd_spi, "SEND_NUM_WR_BLOCKS", sd_acmd_SEND_NUM_WR_BLOCKS},
        [23] = {8,  sd_spi, "SET_WR_BLK_ERASE_COUNT", sd_acmd_SET_WR_BLK_ERASE_COUNT},
        [41] = {8,  sd_spi, "SEND_OP_COND", sd_cmd_SEND_OP_COND},
        [42] = {8,  sd_spi, "SET_CLR_CARD_DETECT", sd_acmd_SET_CLR_CARD_DETECT},
        [51] = {8,  sd_spi, "SEND_SCR", sd_acmd_SEND_SCR},
    },
};

static const SDProto sd_proto_sd = {
    .name = "SD",
    .cmd = {
        [0]  = {0,  sd_bc,   "GO_IDLE_STATE", sd_cmd_GO_IDLE_STATE},
        [2]  = {0,  sd_bcr,  "ALL_SEND_CID", sd_cmd_ALL_SEND_CID},
        [3]  = {0,  sd_bcr,  "SEND_RELATIVE_ADDR", sd_cmd_SEND_RELATIVE_ADDR},
        [4]  = {0,  sd_bc,   "SEND_DSR", sd_cmd_unimplemented},
        [5]  = {9,  sd_bc,   "IO_SEND_OP_COND", sd_cmd_optional},
        [6]  = {10, sd_adtc, "SWITCH_FUNCTION", sd_cmd_SWITCH_FUNCTION},
        [7]  = {0,  sd_ac,   "(DE)SELECT_CARD", sd_cmd_DE_SELECT_CARD},
        [8]  = {0,  sd_bcr,  "SEND_IF_COND", sd_cmd_SEND_IF_COND},
        [9]  = {0,  sd_ac,   "SEND_CSD", sd_cmd_SEND_CSD},
        [10] = {0,  sd_ac,   "SEND_CID", sd_cmd_SEND_CID},
        [11] = {0,  sd_ac,   "VOLTAGE_SWITCH", sd_cmd_optional},
        [12] = {0,  sd_ac,   "STOP_TRANSMISSION", sd_cmd_STOP_TRANSMISSION},
        [13] = {0,  sd_ac,   "SEND_STATUS", sd_cmd_SEND_STATUS},
        [15] = {0,  sd_ac,   "GO_INACTIVE_STATE", sd_cmd_GO_INACTIVE_STATE},
        [16] = {2,  sd_ac,   "SET_BLOCKLEN", sd_cmd_SET_BLOCKLEN},
        [17] = {2,  sd_adtc, "READ_SINGLE_BLOCK", sd_cmd_READ_SINGLE_BLOCK},
        [19] = {2,  sd_adtc, "SEND_TUNING_BLOCK", sd_cmd_SEND_TUNING_BLOCK},
        [20] = {2,  sd_ac,   "SPEED_CLASS_CONTROL", sd_cmd_optional},
        [23] = {2,  sd_ac,   "SET_BLOCK_COUNT", sd_cmd_SET_BLOCK_COUNT},
        [24] = {4,  sd_adtc, "WRITE_SINGLE_BLOCK", sd_cmd_WRITE_SINGLE_BLOCK},
        [27] = {4,  sd_adtc, "PROGRAM_CSD", sd_cmd_PROGRAM_CSD},
        [28] = {6,  sd_ac,   "SET_WRITE_PROT", sd_cmd_SET_WRITE_PROT},
        [29] = {6,  sd_ac,   "CLR_WRITE_PROT", sd_cmd_CLR_WRITE_PROT},
        [30] = {6,  sd_adtc, "SEND_WRITE_PROT", sd_cmd_SEND_WRITE_PROT},
        [32] = {5,  sd_ac,   "ERASE_WR_BLK_START", sd_cmd_ERASE_WR_BLK_START},
        [33] = {5,  sd_ac,   "ERASE_WR_BLK_END", sd_cmd_ERASE_WR_BLK_END},
        [34] = {10, sd_adtc, "READ_SEC_CMD", sd_cmd_optional},
        [35] = {10, sd_adtc, "WRITE_SEC_CMD", sd_cmd_optional},
        [36] = {10, sd_adtc, "SEND_PSI", sd_cmd_optional},
        [37] = {10, sd_ac,   "CONTROL_ASSD_SYSTEM", sd_cmd_optional},
        [38] = {5,  sd_ac,   "ERASE", sd_cmd_ERASE},
        [42] = {7,  sd_adtc, "LOCK_UNLOCK", sd_cmd_LOCK_UNLOCK},
        [43] = {1,  sd_ac,   "Q_MANAGEMENT", sd_cmd_optional},
        [44] = {1,  sd_ac,   "Q_TASK_INFO_A", sd_cmd_optional},
        [45] = {1,  sd_ac,   "Q_TASK_INFO_B", sd_cmd_optional},
        [46] = {1,  sd_adtc, "Q_RD_TASK", sd_cmd_optional},
        [47] = {1,  sd_adtc, "Q_WR_TASK", sd_cmd_optional},
        [48] = {1,  sd_adtc, "READ_EXTR_SINGLE", sd_cmd_optional},
        [49] = {1,  sd_adtc, "WRITE_EXTR_SINGLE", sd_cmd_optional},
        [50] = {10, sd_adtc, "DIRECT_SECURE_READ", sd_cmd_optional},
        [52] = {9,  sd_bc,   "IO_RW_DIRECT", sd_cmd_optional},
        [53] = {9,  sd_bc,   "IO_RW_EXTENDED", sd_cmd_optional},
        [55] = {8,  sd_ac,   "APP_CMD", sd_cmd_APP_CMD},
        [56] = {8,  sd_adtc, "GEN_CMD", sd_cmd_GEN_CMD},
        [57] = {10, sd_adtc, "DIRECT_SECURE_WRITE", sd_cmd_optional},
        [58] = {11, sd_adtc, "READ_EXTR_MULTI", sd_cmd_optional},
        [59] = {11, sd_adtc, "WRITE_EXTR_MULTI", sd_cmd_optional},
    },
    .acmd = {
        [6]  = {8,  sd_ac,   "SET_BUS_WIDTH", sd_acmd_SET_BUS_WIDTH},
        [13] = {8,  sd_adtc, "SD_STATUS", sd_acmd_SD_STATUS},
        [22] = {8,  sd_adtc, "SEND_NUM_WR_BLOCKS", sd_acmd_SEND_NUM_WR_BLOCKS},
        [23] = {8,  sd_ac,   "SET_WR_BLK_ERASE_COUNT", sd_acmd_SET_WR_BLK_ERASE_COUNT},
        [41] = {8,  sd_bcr,  "SEND_OP_COND", sd_cmd_SEND_OP_COND},
        [42] = {8,  sd_ac,   "SET_CLR_CARD_DETECT", sd_acmd_SET_CLR_CARD_DETECT},
        [51] = {8,  sd_adtc, "SEND_SCR", sd_acmd_SEND_SCR},
    },
};

static const SDProto sd_proto_emmc = {
    /* Only v4.3 is supported */
    .name = "eMMC",
    .cmd = {
        [0]  = {0,  sd_bc,   "GO_IDLE_STATE", sd_cmd_GO_IDLE_STATE},
        [1]  = {0,  sd_bcr,  "SEND_OP_COND", sd_cmd_SEND_OP_COND},
        [2]  = {0,  sd_bcr,  "ALL_SEND_CID", sd_cmd_ALL_SEND_CID},
        [3]  = {0,  sd_ac,   "SET_RELATIVE_ADDR", emmc_cmd_SET_RELATIVE_ADDR},
        [4]  = {0,  sd_bc,   "SEND_DSR", sd_cmd_unimplemented},
        [5]  = {0,  sd_ac,   "SLEEP/AWAKE", emmc_cmd_sleep_awake},
        [6]  = {10, sd_adtc, "SWITCH", emmc_cmd_SWITCH},
        [7]  = {0,  sd_ac,   "(DE)SELECT_CARD", sd_cmd_DE_SELECT_CARD},
        [8]  = {0,  sd_adtc, "SEND_EXT_CSD", emmc_cmd_SEND_EXT_CSD},
        [9]  = {0,  sd_ac,   "SEND_CSD", sd_cmd_SEND_CSD},
        [10] = {0,  sd_ac,   "SEND_CID", sd_cmd_SEND_CID},
        [11] = {1,  sd_adtc, "READ_DAT_UNTIL_STOP", sd_cmd_unimplemented},
        [12] = {0,  sd_ac,   "STOP_TRANSMISSION", sd_cmd_STOP_TRANSMISSION},
        [13] = {0,  sd_ac,   "SEND_STATUS", sd_cmd_SEND_STATUS},
        [14] = {0,  sd_adtc, "BUSTEST_R", sd_cmd_unimplemented},
        [15] = {0,  sd_ac,   "GO_INACTIVE_STATE", sd_cmd_GO_INACTIVE_STATE},
        [16] = {2,  sd_ac,   "SET_BLOCKLEN", sd_cmd_SET_BLOCKLEN},
        [17] = {2,  sd_adtc, "READ_SINGLE_BLOCK", sd_cmd_READ_SINGLE_BLOCK},
        [19] = {0,  sd_adtc, "BUSTEST_W", sd_cmd_unimplemented},
        [20] = {3,  sd_adtc, "WRITE_DAT_UNTIL_STOP", sd_cmd_unimplemented},
        [23] = {2,  sd_ac,   "SET_BLOCK_COUNT", sd_cmd_SET_BLOCK_COUNT},
        [24] = {4,  sd_adtc, "WRITE_SINGLE_BLOCK", sd_cmd_WRITE_SINGLE_BLOCK},
        [26] = {4,  sd_adtc, "PROGRAM_CID", emmc_cmd_PROGRAM_CID},
        [27] = {4,  sd_adtc, "PROGRAM_CSD", sd_cmd_PROGRAM_CSD},
        [28] = {6,  sd_ac,   "SET_WRITE_PROT", sd_cmd_SET_WRITE_PROT},
        [29] = {6,  sd_ac,   "CLR_WRITE_PROT", sd_cmd_CLR_WRITE_PROT},
        [30] = {6,  sd_adtc, "SEND_WRITE_PROT", sd_cmd_SEND_WRITE_PROT},
        [31] = {6,  sd_adtc, "SEND_WRITE_PROT_TYPE", sd_cmd_unimplemented},
        [35] = {5,  sd_ac,   "ERASE_WR_BLK_START", sd_cmd_ERASE_WR_BLK_START},
        [36] = {5,  sd_ac,   "ERASE_WR_BLK_END", sd_cmd_ERASE_WR_BLK_END},
        [38] = {5,  sd_ac,   "ERASE", sd_cmd_ERASE},
        [39] = {9,  sd_ac,   "FAST_IO", sd_cmd_unimplemented},
        [40] = {9,  sd_bcr,  "GO_IRQ_STATE", sd_cmd_unimplemented},
        [42] = {7,  sd_adtc, "LOCK_UNLOCK", sd_cmd_LOCK_UNLOCK},
        [49] = {0,  sd_adtc, "SET_TIME", sd_cmd_unimplemented},
        [55] = {8,  sd_ac,   "APP_CMD", sd_cmd_APP_CMD},
        [56] = {8,  sd_adtc, "GEN_CMD", sd_cmd_GEN_CMD},
    },
};

static void sd_instance_init(Object *obj)
{
    SDState *sd = SDMMC_COMMON(obj);
    SDCardClass *sc = SDMMC_COMMON_GET_CLASS(sd);

    sd->proto = sc->proto;
    sd->last_cmd_name = "UNSET";
    sd->ocr_power_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sd_ocr_powerup, sd);
}

static void sd_instance_finalize(Object *obj)
{
    SDState *sd = SDMMC_COMMON(obj);

    timer_free(sd->ocr_power_timer);
}

static void sd_blk_size_error(SDState *sd, int64_t blk_size,
                              int64_t blk_size_aligned, const char *rule,
                              Error **errp)
{
    const char *dev_type = sd_is_emmc(sd) ? "eMMC" : "SD card";
    char *blk_size_str;

    blk_size_str = size_to_str(blk_size);
    error_setg(errp, "Invalid %s size: %s", dev_type, blk_size_str);
    g_free(blk_size_str);

    blk_size_str = size_to_str(blk_size_aligned);
    error_append_hint(errp,
                      "%s size has to be %s, e.g. %s.\n"
                      "You can resize disk images with"
                      " 'qemu-img resize <imagefile> <new-size>'\n"
                      "(note that this will lose data if you make the"
                      " image smaller than it currently is).\n",
                      dev_type, rule, blk_size_str);
    g_free(blk_size_str);
}

static void sd_realize(DeviceState *dev, Error **errp)
{
    SDState *sd = SDMMC_COMMON(dev);
    int64_t blk_size = -ENOMEDIUM;
    int ret;

    switch (sd->spec_version) {
    case SD_PHY_SPECv2_00_VERS
     ... SD_PHY_SPECv3_01_VERS:
        break;
    default:
        error_setg(errp, "Invalid SD card Spec version: %u", sd->spec_version);
        return;
    }

    if (sd->blk) {
        if (!blk_supports_write_perm(sd->blk)) {
            error_setg(errp, "Cannot use read-only drive as SD card");
            return;
        }

        blk_size = blk_getlength(sd->blk);
    }
    if (blk_size >= 0) {
        blk_size -= sd->boot_part_size * 2 + sd->rpmb_part_size;
        if (blk_size > SDSC_MAX_CAPACITY) {
            if (sd_is_emmc(sd) &&
                !QEMU_IS_ALIGNED(blk_size, 1 << HWBLOCK_SHIFT)) {
                int64_t blk_size_aligned =
                    ((blk_size >> HWBLOCK_SHIFT) + 1) << HWBLOCK_SHIFT;
                sd_blk_size_error(sd, blk_size, blk_size_aligned,
                                  "multiples of 512", errp);
                return;
            } else if (!sd_is_emmc(sd) &&
                !QEMU_IS_ALIGNED(blk_size, 512 * KiB)) {
                int64_t blk_size_aligned = ((blk_size >> 19) + 1) << 19;
                sd_blk_size_error(sd, blk_size, blk_size_aligned,
                                  "multiples of 512K", errp);
                return;
            }
        } else if (blk_size > 0 && !is_power_of_2(blk_size)) {
            sd_blk_size_error(sd, blk_size, pow2ceil(blk_size), "a power of 2",
                              errp);
            return;
        } else if (blk_size < 0) {
            error_setg(errp, "eMMC image smaller than boot partitions");
            return;
        }

        ret = blk_set_perm(sd->blk, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                           BLK_PERM_ALL, errp);
        if (ret < 0) {
            return;
        }
        blk_set_dev_ops(sd->blk, &sd_block_ops, sd);
    }
    if (!QEMU_IS_ALIGNED(sd->boot_part_size, 128 * KiB) ||
        sd->boot_part_size > 255 * 128 * KiB) {
        g_autofree char *size_str = size_to_str(sd->boot_part_size);

        error_setg(errp, "Invalid boot partition size: %s", size_str);
        error_append_hint(errp,
                          "The boot partition size must be multiples of 128K"
                          "and not larger than 32640K.\n");
    }
    if (!QEMU_IS_ALIGNED(sd->rpmb_part_size, 128 * KiB) ||
        sd->rpmb_part_size > 128 * 128 * KiB) {
        char *size_str = size_to_str(sd->boot_part_size);

        error_setg(errp, "Invalid RPMB partition size: %s", size_str);
        g_free(size_str);
        error_append_hint(errp,
                          "The RPMB partition size must be multiples of 128K"
                          "and not larger than 16384K.\n");
    }
    if (sd_is_emmc(sd) && sd->preset_auth_key) {
        if (strlen(sd->preset_auth_key) != 64) {
            error_setg(errp,
                       "Authentication key must be 32 bytes long, "
                       "encoded hexadecimally");
            return;
        }

        char *pos = sd->preset_auth_key;
        unsigned int n;
        for (n = 0; n < RPMB_KEY_MAC_LEN; n++, pos += 2) {
            int chrs;
            if (sscanf(pos, "%02hhx%n", &sd->rpmb.key[n], &chrs) != 1 ||
                chrs != 2) {
                error_setg(errp,
                           "Authentication key contains invalid characters");
                return;
            }
        }
        sd->rpmb.key_set = 1;
    }
}

static void emmc_realize(DeviceState *dev, Error **errp)
{
    SDState *sd = SDMMC_COMMON(dev);

    sd->spec_version = SD_PHY_SPECv3_01_VERS; /* Actually v4.5 */

    sd_realize(dev, errp);
}

static const Property sdmmc_common_properties[] = {
    DEFINE_PROP_DRIVE("drive", SDState, blk),
};

static const Property sd_properties[] = {
    DEFINE_PROP_UINT8("spec_version", SDState,
                      spec_version, SD_PHY_SPECv3_01_VERS),
};

static const Property emmc_properties[] = {
    DEFINE_PROP_UINT64("boot-partition-size", SDState, boot_part_size, 0),
    DEFINE_PROP_UINT8("boot-config", SDState, boot_config, 0x0),
    DEFINE_PROP_UINT64("rpmb-partition-size", SDState, rpmb_part_size, 0),
    DEFINE_PROP_STRING("auth-key", SDState, preset_auth_key),
};

static void sdmmc_common_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SDCardClass *sc = SDMMC_COMMON_CLASS(klass);

    device_class_set_props(dc, sdmmc_common_properties);
    dc->vmsd = &sd_vmstate;
    device_class_set_legacy_reset(dc, sd_reset);
    dc->bus_type = TYPE_SD_BUS;
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);

    sc->set_voltage = sd_set_voltage;
    sc->get_dat_lines = sd_get_dat_lines;
    sc->get_cmd_line = sd_get_cmd_line;
    sc->do_command = sd_do_command;
    sc->write_data = sd_write_data;
    sc->read_data = sd_read_data;
    sc->receive_ready = sd_receive_ready;
    sc->data_ready = sd_data_ready;
    sc->get_inserted = sd_get_inserted;
    sc->get_readonly = sd_get_readonly;
}

static void sd_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SDCardClass *sc = SDMMC_COMMON_CLASS(klass);

    dc->realize = sd_realize;
    device_class_set_props(dc, sd_properties);

    sc->set_cid = sd_set_cid;
    sc->set_csd = sd_set_csd;
    sc->proto = &sd_proto_sd;
}

static const Property cyw43455_sdio_properties[] = {
    DEFINE_PROP_UINT32("hardware-profile", CYW43455SDIOState,
                       hardware_profile, CYW43455_SDIO_PROFILE_PI400),
    DEFINE_NIC_PROPERTIES(CYW43455SDIOState, conf),
    DEFINE_PROP_UINT32("tx-credit-delay-ms", CYW43455SDIOState,
                       tx_credit_delay_ms, 0),
    DEFINE_PROP_UINT8("tx-credit-window", CYW43455SDIOState,
                      tx_credit_window,
                      CYW43455_TX_CREDIT_WINDOW_DEFAULT),
    DEFINE_PROP_BOOL("fws-enabled", CYW43455SDIOState, fws_enabled, false),
    DEFINE_PROP_UINT8("fws-credit-budget", CYW43455SDIOState,
                      fws_credit_budget, 2),
    DEFINE_PROP_UINT8("fws-status-batch-max", CYW43455SDIOState,
                      fws_status_batch_max, 1),
    DEFINE_PROP_UINT32("fws-startup-delay-ms", CYW43455SDIOState,
                       fws_startup_delay_ms, 1),
    DEFINE_PROP_UINT32("fws-status-delay-ms", CYW43455SDIOState,
                       fws_status_delay_ms, 1),
    DEFINE_PROP_UINT32("fws-credit-delay-ms", CYW43455SDIOState,
                       fws_credit_delay_ms, 1),
    DEFINE_PROP_BOOL("auto-associate", CYW43455SDIOState,
                     auto_associate, true),
    DEFINE_PROP_BOOL("explicit-association", CYW43455SDIOState,
                     explicit_association, false),
    DEFINE_PROP_UINT8("rx-glom-max-frames", CYW43455SDIOState,
                      rx_glom_max_frames, 1),
    DEFINE_PROP_UINT32("rx-glom-delay-ms", CYW43455SDIOState,
                       rx_glom_delay_ms, 1),
    DEFINE_PROP_UINT32("ap-chanspec", CYW43455SDIOState,
                       ap_chanspec, CYW43455_CHANSPEC_2G_CH1_20MHZ),
};

static void cyw43455_sdio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SDCardClass *sc = SDMMC_COMMON_CLASS(klass);

    dc->desc = "Experimental CYW43455 SDIO endpoint";
    dc->realize = cyw43455_sdio_realize;
    dc->unrealize = cyw43455_sdio_unrealize;
    dc->vmsd = &cyw43455_sdio_vmstate;
    device_class_set_legacy_reset(dc, cyw43455_sdio_reset);
    device_class_set_props(dc, cyw43455_sdio_properties);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);

    sc->set_voltage = cyw43455_sdio_set_voltage;
    sc->get_dat_lines = cyw43455_sdio_get_dat_lines;
    sc->get_cmd_line = cyw43455_sdio_get_cmd_line;
    sc->do_command = cyw43455_sdio_do_command;
    sc->write_data = cyw43455_sdio_write_data;
    sc->read_data = cyw43455_sdio_read_data;
    sc->receive_ready = cyw43455_sdio_receive_ready;
    sc->data_ready = cyw43455_sdio_data_ready;
    sc->get_inserted = cyw43455_sdio_get_inserted;
    sc->get_readonly = cyw43455_sdio_get_readonly;
}

/*
 * We do not model the chip select pin, so allow the board to select
 * whether card should be in SSI or MMC/SD mode.  It is also up to the
 * board to ensure that ssi transfers only occur when the chip select
 * is asserted.
 */
static void sd_spi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SDCardClass *sc = SDMMC_COMMON_CLASS(klass);

    dc->desc = "SD SPI";
    sc->proto = &sd_proto_spi;
}

static void emmc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SDCardClass *sc = SDMMC_COMMON_CLASS(klass);

    assert(qcrypto_hmac_supports(QCRYPTO_HASH_ALGO_SHA256));

    dc->desc = "eMMC";
    dc->realize = emmc_realize;
    device_class_set_props(dc, emmc_properties);

    sc->proto = &sd_proto_emmc;

    sc->set_cid = emmc_set_cid;
    sc->set_csd = emmc_set_csd;
}

static const TypeInfo sd_types[] = {
    {
        .name           = TYPE_SDMMC_COMMON,
        .parent         = TYPE_DEVICE,
        .abstract       = true,
        .instance_size  = sizeof(SDState),
        .class_size     = sizeof(SDCardClass),
        .class_init     = sdmmc_common_class_init,
        .instance_init  = sd_instance_init,
        .instance_finalize = sd_instance_finalize,
    },
    {
        .name           = TYPE_SD_CARD,
        .parent         = TYPE_SDMMC_COMMON,
        .class_init     = sd_class_init,
    },
    {
        .name           = TYPE_SD_CARD_SPI,
        .parent         = TYPE_SD_CARD,
        .class_init     = sd_spi_class_init,
    },
    {
        .name           = TYPE_CYW43455_SDIO,
        .parent         = TYPE_SDMMC_COMMON,
        .instance_size  = sizeof(CYW43455SDIOState),
        .class_init     = cyw43455_sdio_class_init,
    },
    {
        .name           = TYPE_EMMC,
        .parent         = TYPE_SDMMC_COMMON,
        .class_init     = emmc_class_init,
    },
};

DEFINE_TYPES(sd_types)
