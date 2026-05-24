/*
 * QEMU USB CDC NCM (Network Control Model) device
 *
 * Copyright (c) 2026 The QEMU contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * This is a minimal CDC NCM 1.0 device: one ethernet frame per NTB, NTB16
 * format only (no CRC). It is wired to a QEMU netdev like usb-net (RNDIS/CDC).
 *
 * References:
 *   [NCM10] USB Class Definitions for Communications Devices, NCM Subclass,
 *           Revision 1.0
 *   [CDC12] USB Class Definitions for Communications Devices, Revision 1.2
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/usb/usb.h"
#include "migration/vmstate.h"
#include "desc.h"
#include "net/net.h"
#include "qemu/queue.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "trace.h"

#define NCM_VENDOR_NUM          0x0525  /* NetChip (Linux gadget range) */
#define NCM_PRODUCT_NUM         0xa4a3  /* a4a1=CDC, a4a2=RNDIS, a4a3=NCM */

enum usbstring_idx {
    STRING_MANUFACTURER = 1,
    STRING_PRODUCT,
    STRING_SERIALNUMBER,
    STRING_CONFIG,
    STRING_CONTROL,
    STRING_DATA,
    STRING_ETHADDR,
};

/* The wasabi driver requires bConfigurationValue == 2 (it hardcodes
 * request_set_config(slot, 2) before request_set_interface). Keep this
 * value stable. */
#define NCM_CONFIG_VALUE 2

/* CDC functional descriptor subtypes [CDC12 Table 13] */
#define USB_CDC_SUBCLASS_NCM            0x0d
#define USB_CDC_NCM_PROTO_NONE          0x00
#define USB_CDC_DATA_PROTO_NCM          0x01

#define USB_CDC_HEADER_TYPE             0x00
#define USB_CDC_UNION_TYPE              0x06
#define USB_CDC_ETHERNET_TYPE           0x0f
#define USB_CDC_NCM_TYPE                0x1a

/* CDC NCM class requests [NCM10 Table 6-2] */
#define USB_CDC_NCM_GET_NTB_PARAMETERS          0x80
#define USB_CDC_NCM_GET_NET_ADDRESS             0x81
#define USB_CDC_NCM_SET_NET_ADDRESS             0x82
#define USB_CDC_NCM_GET_NTB_FORMAT              0x83
#define USB_CDC_NCM_SET_NTB_FORMAT              0x84
#define USB_CDC_NCM_GET_NTB_INPUT_SIZE          0x85
#define USB_CDC_NCM_SET_NTB_INPUT_SIZE          0x86
#define USB_CDC_NCM_GET_MAX_DATAGRAM_SIZE       0x87
#define USB_CDC_NCM_SET_MAX_DATAGRAM_SIZE       0x88
#define USB_CDC_NCM_GET_CRC_MODE                0x89
#define USB_CDC_NCM_SET_CRC_MODE                0x8a

/* CDC class requests reused under NCM */
#define USB_CDC_REQ_SET_ETHERNET_PACKET_FILTER  0x43

/* CDC notification codes [CDC12 Table 20] */
#define USB_CDC_NOTIFY_NETWORK_CONNECTION       0x00
#define USB_CDC_NOTIFY_CONNECTION_SPEED_CHANGE  0x2a

/* Sizing */
#define ETH_FRAME_LEN                   1514
#define NCM_NTB_MAX_LEN                 16384
#define NCM_STATUS_BYTECOUNT            16    /* fits SPEED_CHANGE payload */

/* Generous IN slot: an NTB carrying one full ethernet frame
 * (12 NTH + 16 NDP + 1514 frame = 1542; round up). */
#define NCM_IN_BUF_LEN                  (NCM_NTB_MAX_LEN)
/* Generous OUT accumulation buffer for one NTB. */
#define NCM_OUT_BUF_LEN                 (NCM_NTB_MAX_LEN)

#define TYPE_USB_NCM "usb-ncm"
OBJECT_DECLARE_SIMPLE_TYPE(USBNCMState, USB_NCM)

/* A small queue of pending notifications. The wasabi driver polls the
 * interrupt-IN endpoint with a 16-byte buffer, but a NETWORK_CONNECTION
 * notification is 8 bytes. USB short-packet rules let the host see the
 * smaller payload as a complete transfer. */
typedef struct NCMNotif {
    uint8_t  buf[NCM_STATUS_BYTECOUNT];
    uint8_t  len;
    QTAILQ_ENTRY(NCMNotif) link;
} NCMNotif;

struct USBNCMState {
    USBDevice  dev;
    NICState   *nic;
    NICConf    conf;
    USBEndpoint *bulk_in;
    USBEndpoint *intr_in;

    /* iMACAddress points at a string descriptor whose body is the 12-char
     * hex MAC. The wasabi driver reads this through GET_DESCRIPTOR(STRING). */
    char        usbstring_mac[13];

    /* TX (host->guest, i.e. bulk-IN as seen from the host): one NTB at a
     * time. Filled by usbnet_receive(), drained by handle_datain(). */
    uint8_t     in_buf[NCM_IN_BUF_LEN];
    uint32_t    in_len;
    uint32_t    in_ptr;
    uint16_t    in_seq;
    /* True when we just sent a frame whose total length is a multiple of
     * the bulk wMaxPacketSize and need to terminate it with a zero-length
     * packet so the host sees end-of-transfer. */
    bool        in_need_zlp;

    /* RX (guest->host, bulk-OUT): accumulate fragments of one NTB until
     * we have the full block_length, then walk its NDP16. */
    uint8_t     out_buf[NCM_OUT_BUF_LEN];
    uint32_t    out_ptr;

    /* Pending interrupt-IN notifications. */
    QTAILQ_HEAD(, NCMNotif) notifs;
};

static const USBDescStrings usb_ncm_stringtable = {
    [STRING_MANUFACTURER] = "QEMU",
    [STRING_PRODUCT]      = "QEMU USB NCM Network Device",
    [STRING_SERIALNUMBER] = "1",
    [STRING_CONFIG]       = "QEMU NCM",
    [STRING_CONTROL]      = "QEMU NCM Comms",
    [STRING_DATA]         = "QEMU NCM Data",
    [STRING_ETHADDR]      = "400102030405",
};

/*
 * Class-specific descriptors under interface 0 (CDC Comms / NCM):
 *   Header functional descriptor  [CDC12 Table 15]
 *   Union  functional descriptor  [CDC12 Table 16]
 *   Ethernet Networking functional descriptor [CDC12 Table 13]
 *   NCM functional descriptor    [NCM10 Table 5-2]
 *
 * iMACAddress in the Ethernet descriptor points at STRING_ETHADDR; the
 * actual hex string is filled at realize() from the NICConf MAC.
 */
static const USBDescIface desc_iface_ncm[] = {
    /* Interface 0 alt 0: NCM Communications Interface */
    {
        .bInterfaceNumber              = 0,
        .bAlternateSetting             = 0,
        .bNumEndpoints                 = 1,
        .bInterfaceClass               = USB_CLASS_COMM,
        .bInterfaceSubClass            = USB_CDC_SUBCLASS_NCM,
        .bInterfaceProtocol            = USB_CDC_NCM_PROTO_NONE,
        .iInterface                    = STRING_CONTROL,
        .ndesc                         = 4,
        .descs = (USBDescOther[]) {
            { /* Header */
                .data = (uint8_t[]) {
                    0x05,
                    USB_DT_CS_INTERFACE,
                    USB_CDC_HEADER_TYPE,
                    0x10, 0x01,             /* bcdCDC = 1.10 */
                },
            },
            { /* Union: master=0, slave=1 */
                .data = (uint8_t[]) {
                    0x05,
                    USB_DT_CS_INTERFACE,
                    USB_CDC_UNION_TYPE,
                    0x00,
                    0x01,
                },
            },
            { /* Ethernet Networking [CDC12 Table 13] */
                .data = (uint8_t[]) {
                    0x0d,
                    USB_DT_CS_INTERFACE,
                    USB_CDC_ETHERNET_TYPE,
                    STRING_ETHADDR,         /* iMACAddress */
                    0x00, 0x00, 0x00, 0x00, /* bmEthernetStatistics */
                    ETH_FRAME_LEN & 0xff,
                    ETH_FRAME_LEN >> 8,     /* wMaxSegmentSize */
                    0x00, 0x00,             /* wNumberMCFilters */
                    0x00,                   /* bNumberPowerFilters */
                },
            },
            { /* NCM functional [NCM10 Table 5-2] */
                .data = (uint8_t[]) {
                    0x06,
                    USB_DT_CS_INTERFACE,
                    USB_CDC_NCM_TYPE,
                    0x00, 0x01,             /* bcdNcmVersion = 1.00 */
                    0x00,                   /* bmNetworkCapabilities */
                },
            },
        },
        .eps = (USBDescEndpoint[]) {
            {
                .bEndpointAddress = USB_DIR_IN | 0x01,
                .bmAttributes     = USB_ENDPOINT_XFER_INT,
                .wMaxPacketSize   = NCM_STATUS_BYTECOUNT,
                .bInterval        = 9, /* 2^(9-1) * 125us = 32ms on HS/SS */
                .bMaxBurst        = 0,
                .wBytesPerInterval = NCM_STATUS_BYTECOUNT,
            },
        },
    },
    /* Interface 1 alt 0: NCM Data Interface (zero-bandwidth) */
    {
        .bInterfaceNumber              = 1,
        .bAlternateSetting             = 0,
        .bNumEndpoints                 = 0,
        .bInterfaceClass               = USB_CLASS_CDC_DATA,
        .bInterfaceSubClass            = 0x00,
        .bInterfaceProtocol            = USB_CDC_DATA_PROTO_NCM,
        .iInterface                    = STRING_DATA,
    },
    /* Interface 1 alt 1: NCM Data Interface (bulk-IN + bulk-OUT) */
    {
        .bInterfaceNumber              = 1,
        .bAlternateSetting             = 1,
        .bNumEndpoints                 = 2,
        .bInterfaceClass               = USB_CLASS_CDC_DATA,
        .bInterfaceSubClass            = 0x00,
        .bInterfaceProtocol            = USB_CDC_DATA_PROTO_NCM,
        .iInterface                    = STRING_DATA,
        .eps = (USBDescEndpoint[]) {
            {
                .bEndpointAddress = USB_DIR_IN | 0x02,
                .bmAttributes     = USB_ENDPOINT_XFER_BULK,
                .wMaxPacketSize   = 512, /* overridden to 1024 in super */
                .bMaxBurst        = 0,
            },
            {
                .bEndpointAddress = USB_DIR_OUT | 0x02,
                .bmAttributes     = USB_ENDPOINT_XFER_BULK,
                .wMaxPacketSize   = 512,
                .bMaxBurst        = 0,
            },
        },
    },
};

/* SuperSpeed: same layout but bulk mps = 1024 [USB3 9.6.6]. Re-declared
 * rather than mutated so the high/super entries can be const and live
 * in distinct USBDescDevices. */
static const USBDescIface desc_iface_ncm_super[] = {
    {
        .bInterfaceNumber              = 0,
        .bAlternateSetting             = 0,
        .bNumEndpoints                 = 1,
        .bInterfaceClass               = USB_CLASS_COMM,
        .bInterfaceSubClass            = USB_CDC_SUBCLASS_NCM,
        .bInterfaceProtocol            = USB_CDC_NCM_PROTO_NONE,
        .iInterface                    = STRING_CONTROL,
        .ndesc                         = 4,
        .descs = (USBDescOther[]) {
            { .data = (uint8_t[]) {
                0x05, USB_DT_CS_INTERFACE, USB_CDC_HEADER_TYPE,
                0x10, 0x01,
            }, },
            { .data = (uint8_t[]) {
                0x05, USB_DT_CS_INTERFACE, USB_CDC_UNION_TYPE,
                0x00, 0x01,
            }, },
            { .data = (uint8_t[]) {
                0x0d, USB_DT_CS_INTERFACE, USB_CDC_ETHERNET_TYPE,
                STRING_ETHADDR,
                0x00, 0x00, 0x00, 0x00,
                ETH_FRAME_LEN & 0xff, ETH_FRAME_LEN >> 8,
                0x00, 0x00,
                0x00,
            }, },
            { .data = (uint8_t[]) {
                0x06, USB_DT_CS_INTERFACE, USB_CDC_NCM_TYPE,
                0x00, 0x01,
                0x00,
            }, },
        },
        .eps = (USBDescEndpoint[]) {
            {
                .bEndpointAddress  = USB_DIR_IN | 0x01,
                .bmAttributes      = USB_ENDPOINT_XFER_INT,
                .wMaxPacketSize    = NCM_STATUS_BYTECOUNT,
                .bInterval         = 9,
                .bMaxBurst         = 0,
                .wBytesPerInterval = NCM_STATUS_BYTECOUNT,
            },
        },
    },
    {
        .bInterfaceNumber              = 1,
        .bAlternateSetting             = 0,
        .bNumEndpoints                 = 0,
        .bInterfaceClass               = USB_CLASS_CDC_DATA,
        .bInterfaceSubClass            = 0x00,
        .bInterfaceProtocol            = USB_CDC_DATA_PROTO_NCM,
        .iInterface                    = STRING_DATA,
    },
    {
        .bInterfaceNumber              = 1,
        .bAlternateSetting             = 1,
        .bNumEndpoints                 = 2,
        .bInterfaceClass               = USB_CLASS_CDC_DATA,
        .bInterfaceSubClass            = 0x00,
        .bInterfaceProtocol            = USB_CDC_DATA_PROTO_NCM,
        .iInterface                    = STRING_DATA,
        .eps = (USBDescEndpoint[]) {
            {
                .bEndpointAddress = USB_DIR_IN | 0x02,
                .bmAttributes     = USB_ENDPOINT_XFER_BULK,
                .wMaxPacketSize   = 1024,
                .bMaxBurst        = 0,
            },
            {
                .bEndpointAddress = USB_DIR_OUT | 0x02,
                .bmAttributes     = USB_ENDPOINT_XFER_BULK,
                .wMaxPacketSize   = 1024,
                .bMaxBurst        = 0,
            },
        },
    },
};

static const USBDescDevice desc_device_ncm_high = {
    .bcdUSB                = 0x0200,
    .bDeviceClass          = USB_CLASS_COMM,
    .bMaxPacketSize0       = 64,
    .bNumConfigurations    = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces      = 2,
            .bConfigurationValue = NCM_CONFIG_VALUE,
            .iConfiguration      = STRING_CONFIG,
            .bmAttributes        = USB_CFG_ATT_ONE | USB_CFG_ATT_SELFPOWER,
            .bMaxPower           = 0x32,
            .nif = ARRAY_SIZE(desc_iface_ncm),
            .ifs = desc_iface_ncm,
        },
    },
};

static const USBDescDevice desc_device_ncm_super = {
    .bcdUSB                = 0x0300,
    .bDeviceClass          = USB_CLASS_COMM,
    .bMaxPacketSize0       = 9,    /* 512-byte EP0 (2^9) at SuperSpeed */
    .bNumConfigurations    = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces      = 2,
            .bConfigurationValue = NCM_CONFIG_VALUE,
            .iConfiguration      = STRING_CONFIG,
            .bmAttributes        = USB_CFG_ATT_ONE | USB_CFG_ATT_SELFPOWER,
            .bMaxPower           = 0x32,
            .nif = ARRAY_SIZE(desc_iface_ncm_super),
            .ifs = desc_iface_ncm_super,
        },
    },
};

static const USBDesc desc_ncm = {
    .id = {
        .idVendor          = NCM_VENDOR_NUM,
        .idProduct         = NCM_PRODUCT_NUM,
        .bcdDevice         = 0,
        .iManufacturer     = STRING_MANUFACTURER,
        .iProduct          = STRING_PRODUCT,
        .iSerialNumber     = STRING_SERIALNUMBER,
    },
    .high = &desc_device_ncm_high,
    .super = &desc_device_ncm_super,
    .str   = usb_ncm_stringtable,
};

/* -----------------------------------------------------------------------
 *  NTB16 encode / decode
 * ----------------------------------------------------------------------- */

/* [NCM10] 3.2.1 NTH16, 3.3.1 NDP16 (NCM0: no-CRC variant).
 *
 * Layout for a single datagram:
 *   off  0..12  : NTH16  (signature "NCMH", wHeaderLength = 12)
 *   off 12..28  : NDP16  (signature "NCM0", one (idx,len) + (0,0) term)
 *   off 28..28+N: datagram bytes
 *
 * Block length is 12 + 16 + N = 28 + N, already 4-aligned for any N. */
#define NCM_NTH16_LEN  12
#define NCM_NDP16_LEN  16
#define NCM_DGRAM_OFF  (NCM_NTH16_LEN + NCM_NDP16_LEN)

/* Build an NTB16 in `dst` (caller-provided, at least NCM_DGRAM_OFF + len
 * bytes). Returns total NTB length. */
static uint32_t ncm_build_ntb16(uint8_t *dst, const uint8_t *frame,
                                uint32_t frame_len, uint16_t seq)
{
    uint32_t block_len = NCM_DGRAM_OFF + frame_len;

    /* NTH16 */
    dst[0] = 'N'; dst[1] = 'C'; dst[2] = 'M'; dst[3] = 'H';
    dst[4] = NCM_NTH16_LEN & 0xff;
    dst[5] = NCM_NTH16_LEN >> 8;
    dst[6] = seq & 0xff;
    dst[7] = seq >> 8;
    dst[8] = block_len & 0xff;
    dst[9] = block_len >> 8;
    dst[10] = NCM_NTH16_LEN & 0xff;
    dst[11] = NCM_NTH16_LEN >> 8;

    /* NDP16 (no-CRC: "NCM0") */
    dst[12] = 'N'; dst[13] = 'C'; dst[14] = 'M'; dst[15] = '0';
    dst[16] = NCM_NDP16_LEN & 0xff;
    dst[17] = NCM_NDP16_LEN >> 8;
    dst[18] = 0; dst[19] = 0;                       /* wNextNdpIndex */
    dst[20] = NCM_DGRAM_OFF & 0xff;
    dst[21] = NCM_DGRAM_OFF >> 8;
    dst[22] = frame_len & 0xff;
    dst[23] = frame_len >> 8;
    dst[24] = 0; dst[25] = 0;                       /* terminator idx */
    dst[26] = 0; dst[27] = 0;                       /* terminator len */

    memcpy(dst + NCM_DGRAM_OFF, frame, frame_len);
    return block_len;
}

/* Walk one NTB16, send each datagram to the netdev. Tolerates malformed
 * NTBs by silently discarding. */
static void ncm_parse_and_send_ntb16(USBNCMState *s,
                                     const uint8_t *ntb, uint32_t len)
{
    uint16_t hdr_len, block_len, ndp_index;
    uint32_t i;

    if (len < NCM_NTH16_LEN) {
        return;
    }
    if (ntb[0] != 'N' || ntb[1] != 'C' || ntb[2] != 'M' || ntb[3] != 'H') {
        trace_usb_ncm_rx_bad_ntb(len);
        return;
    }
    hdr_len   = ntb[4] | ((uint16_t)ntb[5] << 8);
    block_len = ntb[8] | ((uint16_t)ntb[9] << 8);
    ndp_index = ntb[10] | ((uint16_t)ntb[11] << 8);
    if (hdr_len != NCM_NTH16_LEN) {
        return;
    }
    if (block_len > len) {
        return;
    }
    if (ndp_index + 8 > block_len) {
        return;
    }
    if (ntb[ndp_index] != 'N' || ntb[ndp_index + 1] != 'C' ||
        ntb[ndp_index + 2] != 'M' || ntb[ndp_index + 3] != '0') {
        /* Only no-CRC variant is supported. */
        return;
    }
    /* Skip wLength (2) + wNextNdpIndex (2) and walk (idx,len) pairs. */
    for (i = ndp_index + 8; i + 4 <= block_len; i += 4) {
        uint16_t didx = ntb[i] | ((uint16_t)ntb[i + 1] << 8);
        uint16_t dlen = ntb[i + 2] | ((uint16_t)ntb[i + 3] << 8);
        if (didx == 0 && dlen == 0) {
            break;
        }
        if ((uint32_t)didx + dlen > block_len) {
            break;
        }
        trace_usb_ncm_rx_frame(dlen);
        qemu_send_packet(qemu_get_queue(s->nic), ntb + didx, dlen);
    }
}

/* -----------------------------------------------------------------------
 *  Notification queue (interrupt-IN endpoint)
 * ----------------------------------------------------------------------- */

static void ncm_queue_notif(USBNCMState *s, uint8_t code, uint16_t value,
                            const void *payload, uint16_t payload_len)
{
    NCMNotif *n = g_new0(NCMNotif, 1);
    uint8_t *b = n->buf;
    uint8_t total = 8 + payload_len;

    assert(total <= NCM_STATUS_BYTECOUNT);
    b[0] = ClassInterfaceRequest >> 8;          /* 0xA1 */
    b[1] = code;
    b[2] = value & 0xff;
    b[3] = value >> 8;
    b[4] = 0;                                   /* wIndex = interface 0 */
    b[5] = 0;
    b[6] = payload_len & 0xff;
    b[7] = payload_len >> 8;
    if (payload_len) {
        memcpy(b + 8, payload, payload_len);
    }
    n->len = total;
    QTAILQ_INSERT_TAIL(&s->notifs, n, link);
    trace_usb_ncm_notif_queue(code, value);
    if (s->intr_in) {
        usb_wakeup(s->intr_in, 0);
    }
}

static void ncm_queue_link_notifs(USBNCMState *s)
{
    uint8_t speed[8];
    uint32_t bps = 1000000000; /* 1 Gbps */

    ncm_queue_notif(s, USB_CDC_NOTIFY_NETWORK_CONNECTION, 1, NULL, 0);

    /* CONNECTION_SPEED_CHANGE payload: dwUSBitRate (down), dwDSBitRate (up).
     * [CDC12 Table 22] */
    speed[0] = bps & 0xff;
    speed[1] = (bps >> 8)  & 0xff;
    speed[2] = (bps >> 16) & 0xff;
    speed[3] = (bps >> 24) & 0xff;
    speed[4] = speed[0]; speed[5] = speed[1];
    speed[6] = speed[2]; speed[7] = speed[3];
    ncm_queue_notif(s, USB_CDC_NOTIFY_CONNECTION_SPEED_CHANGE, 0, speed, 8);
}

static void ncm_clear_notifs(USBNCMState *s)
{
    NCMNotif *n;
    while ((n = QTAILQ_FIRST(&s->notifs))) {
        QTAILQ_REMOVE(&s->notifs, n, link);
        g_free(n);
    }
}

/* -----------------------------------------------------------------------
 *  GET_NTB_PARAMETERS response  [NCM10 Table 6-3]
 * ----------------------------------------------------------------------- */

/* 28-byte structure, NTB16-only (NTB32 advertised as unsupported by
 * leaving its supported-formats bit clear and setting its sizes to 0). */
static void ncm_fill_ntb_parameters(uint8_t *out)
{
    /* wLength = 28 */
    out[0]  = 28;
    out[1]  = 0;
    /* bmNtbFormatsSupported = 0x0001 (NTB16 only) */
    out[2]  = 0x01;
    out[3]  = 0x00;
    /* dwNtbInMaxSize = NCM_NTB_MAX_LEN */
    out[4]  = NCM_NTB_MAX_LEN        & 0xff;
    out[5]  = (NCM_NTB_MAX_LEN >> 8) & 0xff;
    out[6]  = 0;
    out[7]  = 0;
    /* wNdpInDivisor = 4 */
    out[8]  = 4;  out[9]  = 0;
    /* wNdpInPayloadRemainder = 0 */
    out[10] = 0;  out[11] = 0;
    /* wNdpInAlignment = 4 */
    out[12] = 4;  out[13] = 0;
    /* wReserved */
    out[14] = 0;  out[15] = 0;
    /* dwNtbOutMaxSize */
    out[16] = NCM_NTB_MAX_LEN        & 0xff;
    out[17] = (NCM_NTB_MAX_LEN >> 8) & 0xff;
    out[18] = 0;
    out[19] = 0;
    /* wNdpOutDivisor = 4 */
    out[20] = 4;  out[21] = 0;
    /* wNdpOutPayloadRemainder = 0 */
    out[22] = 0;  out[23] = 0;
    /* wNdpOutAlignment = 4 */
    out[24] = 4;  out[25] = 0;
    /* wNtbOutMaxDatagrams = 0 (no limit) */
    out[26] = 0;  out[27] = 0;
}

/* -----------------------------------------------------------------------
 *  USB control and data callbacks
 * ----------------------------------------------------------------------- */

static void usb_ncm_handle_reset(USBDevice *dev)
{
    USBNCMState *s = USB_NCM(dev);

    s->in_len = 0;
    s->in_ptr = 0;
    s->in_seq = 0;
    s->in_need_zlp = false;
    s->out_ptr = 0;
    ncm_clear_notifs(s);
}

static void usb_ncm_handle_control(USBDevice *dev, USBPacket *p,
                                   int request, int value, int index,
                                   int length, uint8_t *data)
{
    USBNCMState *s = USB_NCM(dev);
    int ret;

    trace_usb_ncm_control(request, value, index, length);
    ret = usb_desc_handle_control(dev, p, request, value, index,
                                  length, data);
    if (ret >= 0) {
        if (request == (DeviceOutRequest | USB_REQ_SET_CONFIGURATION) &&
            value == NCM_CONFIG_VALUE) {
            /* Host has just picked the NCM configuration. Queue
             * NETWORK_CONNECTION + CONNECTION_SPEED_CHANGE so the
             * driver's interrupt-IN poll sees the link as up. */
            trace_usb_ncm_set_config(value);
            ncm_clear_notifs(s);
            ncm_queue_link_notifs(s);
        }
        return;
    }

    switch (request) {
    case ClassInterfaceRequest | USB_CDC_NCM_GET_NTB_PARAMETERS:
        if (length < 28) {
            p->status = USB_RET_STALL;
            return;
        }
        ncm_fill_ntb_parameters(data);
        p->actual_length = 28;
        return;

    case ClassInterfaceRequest | USB_CDC_NCM_GET_NTB_INPUT_SIZE:
        if (length < 4) {
            p->status = USB_RET_STALL;
            return;
        }
        data[0] = NCM_NTB_MAX_LEN        & 0xff;
        data[1] = (NCM_NTB_MAX_LEN >> 8) & 0xff;
        data[2] = 0;
        data[3] = 0;
        p->actual_length = 4;
        return;

    case ClassInterfaceOutRequest | USB_CDC_NCM_SET_NTB_INPUT_SIZE:
        /* Acknowledge but do not change behaviour; we always emit one
         * datagram per NTB, well within NCM_NTB_MAX_LEN. */
        return;

    case ClassInterfaceRequest | USB_CDC_NCM_GET_NTB_FORMAT:
        if (length < 2) {
            p->status = USB_RET_STALL;
            return;
        }
        data[0] = 0; data[1] = 0;        /* NTB16 */
        p->actual_length = 2;
        return;

    case ClassInterfaceOutRequest | USB_CDC_NCM_SET_NTB_FORMAT:
        if (value != 0) {
            p->status = USB_RET_STALL;
        }
        return;

    case ClassInterfaceOutRequest | USB_CDC_REQ_SET_ETHERNET_PACKET_FILTER:
        /* No-op: we accept all unicast/multicast/broadcast. */
        return;

    default:
        p->status = USB_RET_STALL;
        return;
    }
}

static void usb_ncm_handle_statusin(USBNCMState *s, USBPacket *p)
{
    NCMNotif *n = QTAILQ_FIRST(&s->notifs);

    if (!n) {
        p->status = USB_RET_NAK;
        return;
    }
    if (p->iov.size < n->len) {
        /* Driver under-allocated; better to stall than truncate. */
        p->status = USB_RET_STALL;
        return;
    }
    usb_packet_copy(p, n->buf, n->len);
    trace_usb_ncm_notif_pop(n->buf[1]);
    QTAILQ_REMOVE(&s->notifs, n, link);
    g_free(n);
}

static void usb_ncm_handle_datain(USBNCMState *s, USBPacket *p)
{
    uint32_t len, remaining;

    /* If a previous transfer ended exactly on an mps boundary, push a
     * zero-length packet so the host sees end-of-transfer. */
    if (s->in_need_zlp) {
        s->in_need_zlp = false;
        p->actual_length = 0;
        return;
    }

    if (s->in_len == 0) {
        p->status = USB_RET_NAK;
        return;
    }
    remaining = s->in_len - s->in_ptr;
    len = remaining;
    if (len > p->iov.size) {
        len = p->iov.size;
    }
    usb_packet_copy(p, &s->in_buf[s->in_ptr], len);
    s->in_ptr += len;

    if (s->in_ptr >= s->in_len) {
        uint32_t mps = p->ep->max_packet_size;
        if (mps && (s->in_len % mps) == 0) {
            s->in_need_zlp = true;
        } else {
            s->in_need_zlp = false;
        }
        s->in_len = 0;
        s->in_ptr = 0;
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
    }
}

static void usb_ncm_handle_dataout(USBNCMState *s, USBPacket *p)
{
    uint32_t sz = sizeof(s->out_buf) - s->out_ptr;
    uint32_t mps = p->ep->max_packet_size;

    if (sz > p->iov.size) {
        sz = p->iov.size;
    }
    usb_packet_copy(p, &s->out_buf[s->out_ptr], sz);
    s->out_ptr += sz;

    /* The host marks end-of-NTB with a short or zero-length packet. */
    if (mps == 0 || p->iov.size < mps || p->iov.size == 0) {
        ncm_parse_and_send_ntb16(s, s->out_buf, s->out_ptr);
        s->out_ptr = 0;
    } else if (s->out_ptr >= sizeof(s->out_buf)) {
        /* Defensive: if the host overruns NCM_NTB_MAX_LEN without ever
         * sending a short packet, drop and reset. */
        s->out_ptr = 0;
    }
}

static void usb_ncm_handle_data(USBDevice *dev, USBPacket *p)
{
    USBNCMState *s = USB_NCM(dev);

    switch (p->pid) {
    case USB_TOKEN_IN:
        switch (p->ep->nr) {
        case 1: usb_ncm_handle_statusin(s, p); return;
        case 2: usb_ncm_handle_datain(s, p);  return;
        }
        break;
    case USB_TOKEN_OUT:
        switch (p->ep->nr) {
        case 2: usb_ncm_handle_dataout(s, p); return;
        }
        break;
    }
    p->status = USB_RET_STALL;
}

/* -----------------------------------------------------------------------
 *  Netdev integration
 * ----------------------------------------------------------------------- */

static ssize_t usb_ncm_receive(NetClientState *nc, const uint8_t *buf,
                               size_t size)
{
    USBNCMState *s = qemu_get_nic_opaque(nc);
    uint32_t ntb_len;

    if (!s->dev.config) {
        return -1;
    }
    if (size == 0 || size > ETH_FRAME_LEN) {
        return size;        /* drop silently */
    }
    if (s->in_len > 0) {
        /* Backpressure: tell the netdev to try again later. */
        return 0;
    }
    if (NCM_DGRAM_OFF + size > sizeof(s->in_buf)) {
        return -1;
    }
    ntb_len = ncm_build_ntb16(s->in_buf, buf, size, s->in_seq);
    trace_usb_ncm_tx_frame(size, s->in_seq);
    s->in_seq++;
    s->in_len = ntb_len;
    s->in_ptr = 0;
    usb_wakeup(s->bulk_in, 0);
    return size;
}

static void usb_ncm_cleanup(NetClientState *nc)
{
    USBNCMState *s = qemu_get_nic_opaque(nc);
    s->nic = NULL;
}

static NetClientInfo net_usb_ncm_info = {
    .type    = NET_CLIENT_DRIVER_NIC,
    .size    = sizeof(NICState),
    .receive = usb_ncm_receive,
    .cleanup = usb_ncm_cleanup,
};

/* -----------------------------------------------------------------------
 *  realize / unrealize / class init
 * ----------------------------------------------------------------------- */

static void usb_ncm_realize(USBDevice *dev, Error **errp)
{
    USBNCMState *s = USB_NCM(dev);

    usb_desc_create_serial(dev);
    usb_desc_init(dev);
    trace_usb_ncm_realize(dev->speed);

    QTAILQ_INIT(&s->notifs);
    s->intr_in = usb_ep_get(dev, USB_TOKEN_IN, 1);
    s->bulk_in = usb_ep_get(dev, USB_TOKEN_IN, 2);

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&net_usb_ncm_info, &s->conf,
                          object_get_typename(OBJECT(s)),
                          s->dev.qdev.id,
                          &s->dev.qdev.mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);

    /* The wasabi driver expects iMACAddress to point at a string
     * descriptor whose body is a 12-character hex MAC (no separators). */
    snprintf(s->usbstring_mac, sizeof(s->usbstring_mac),
             "%02x%02x%02x%02x%02x%02x",
             s->conf.macaddr.a[0], s->conf.macaddr.a[1],
             s->conf.macaddr.a[2], s->conf.macaddr.a[3],
             s->conf.macaddr.a[4], s->conf.macaddr.a[5]);
    usb_desc_set_string(dev, STRING_ETHADDR, s->usbstring_mac);
}

static void usb_ncm_unrealize(USBDevice *dev)
{
    USBNCMState *s = USB_NCM(dev);

    ncm_clear_notifs(s);
    qemu_del_nic(s->nic);
}

static void usb_ncm_instance_init(Object *obj)
{
    USBDevice *dev = USB_DEVICE(obj);
    USBNCMState *s = USB_NCM(dev);

    device_add_bootindex_property(obj, &s->conf.bootindex,
                                  "bootindex", "/ethernet-phy@0",
                                  &dev->qdev);
}

static const VMStateDescription vmstate_usb_ncm = {
    .name = "usb-ncm",
    .unmigratable = 1,
};

static const Property ncm_properties[] = {
    DEFINE_NIC_PROPERTIES(USBNCMState, conf),
};

static void usb_ncm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->realize        = usb_ncm_realize;
    uc->product_desc   = "QEMU USB CDC NCM Network Device";
    uc->usb_desc       = &desc_ncm;
    /* No `.full` USBDescDevice is provided, so dev->device stays NULL
     * after usb_desc_init (which defaults dev->speed to FULL). Wire
     * handle_attach to usb_desc_attach so dev->device picks the right
     * speed-config once the xhci port settles on HS or SS. */
    uc->handle_attach  = usb_desc_attach;
    uc->handle_reset   = usb_ncm_handle_reset;
    uc->handle_control = usb_ncm_handle_control;
    uc->handle_data    = usb_ncm_handle_data;
    uc->unrealize      = usb_ncm_unrealize;
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
    dc->fw_name = "network";
    dc->vmsd    = &vmstate_usb_ncm;
    device_class_set_props(dc, ncm_properties);
}

static const TypeInfo usb_ncm_info = {
    .name          = TYPE_USB_NCM,
    .parent        = TYPE_USB_DEVICE,
    .instance_size = sizeof(USBNCMState),
    .class_init    = usb_ncm_class_init,
    .instance_init = usb_ncm_instance_init,
};

static void usb_ncm_register_types(void)
{
    type_register_static(&usb_ncm_info);
}

type_init(usb_ncm_register_types)
