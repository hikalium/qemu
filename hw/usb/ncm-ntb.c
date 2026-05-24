/*
 * USB CDC NCM 1.0 NTB16 helpers - implementation
 *
 * Pure C, no QEMU-runtime deps: safe to link from tests/unit/.
 *
 * Copyright (c) 2026 The QEMU contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "ncm-ntb.h"

uint32_t ncm_build_ntb16(uint8_t *dst, const uint8_t *frame,
                         uint32_t frame_len, uint16_t seq)
{
    uint32_t block_len = NCM_DGRAM_OFF + frame_len;

    if (block_len > 0xffff) {
        return 0;
    }

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

unsigned ncm_walk_ntb16(const uint8_t *ntb, uint32_t len,
                        ncm_ntb16_datagram_cb cb, void *opaque)
{
    uint16_t hdr_len, block_len, ndp_index;
    uint32_t i;
    unsigned count = 0;

    if (len < NCM_NTH16_LEN) {
        return 0;
    }
    if (ntb[0] != 'N' || ntb[1] != 'C' ||
        ntb[2] != 'M' || ntb[3] != 'H') {
        return 0;
    }
    hdr_len   = ntb[4] | ((uint16_t)ntb[5] << 8);
    block_len = ntb[8] | ((uint16_t)ntb[9] << 8);
    ndp_index = ntb[10] | ((uint16_t)ntb[11] << 8);
    if (hdr_len != NCM_NTH16_LEN) {
        return 0;
    }
    if (block_len > len) {
        return 0;
    }
    /* NDP16 minimum size: 4 (sig) + 2 (wLength) + 2 (wNextNdpIndex)
     * + 4 (one (idx,len) pair) + 4 (terminator) = 16 bytes. We need
     * at least the signature + wLength + wNextNdpIndex (= 8 bytes)
     * before we can start reading datagram entries. */
    if ((uint32_t)ndp_index + 8 > block_len) {
        return 0;
    }
    if (ntb[ndp_index]     != 'N' || ntb[ndp_index + 1] != 'C' ||
        ntb[ndp_index + 2] != 'M' || ntb[ndp_index + 3] != '0') {
        /* Only the no-CRC variant ("NCM0") is supported. */
        return 0;
    }

    /* Skip wLength (2) + wNextNdpIndex (2) and walk (idx,len) pairs. */
    for (i = ndp_index + 8; i + 4 <= block_len; i += 4) {
        uint16_t didx = ntb[i]     | ((uint16_t)ntb[i + 1] << 8);
        uint16_t dlen = ntb[i + 2] | ((uint16_t)ntb[i + 3] << 8);
        if (didx == 0 && dlen == 0) {
            break;                           /* (0,0) end-of-list */
        }
        if ((uint32_t)didx + dlen > block_len) {
            break;
        }
        if (cb) {
            cb(opaque, ntb + didx, dlen);
        }
        count++;
    }
    return count;
}

void ncm_fill_ntb_parameters(uint8_t *out, uint32_t in_max, uint32_t out_max)
{
    /* [NCM10 Table 6-3] NTB Parameter Structure. */
    out[0]  = NCM_NTB_PARAMS_LEN;
    out[1]  = 0;
    /* bmNtbFormatsSupported = 0x0001 (NTB16 only). */
    out[2]  = 0x01;
    out[3]  = 0x00;
    /* dwNtbInMaxSize */
    out[4]  = in_max         & 0xff;
    out[5]  = (in_max >> 8)  & 0xff;
    out[6]  = (in_max >> 16) & 0xff;
    out[7]  = (in_max >> 24) & 0xff;
    /* wNdpInDivisor = 4 */
    out[8]  = 4; out[9]  = 0;
    /* wNdpInPayloadRemainder = 0 */
    out[10] = 0; out[11] = 0;
    /* wNdpInAlignment = 4 */
    out[12] = 4; out[13] = 0;
    /* wReserved */
    out[14] = 0; out[15] = 0;
    /* dwNtbOutMaxSize */
    out[16] = out_max         & 0xff;
    out[17] = (out_max >> 8)  & 0xff;
    out[18] = (out_max >> 16) & 0xff;
    out[19] = (out_max >> 24) & 0xff;
    /* wNdpOutDivisor = 4 */
    out[20] = 4; out[21] = 0;
    /* wNdpOutPayloadRemainder = 0 */
    out[22] = 0; out[23] = 0;
    /* wNdpOutAlignment = 4 */
    out[24] = 4; out[25] = 0;
    /* wNtbOutMaxDatagrams = 0 (no limit) */
    out[26] = 0; out[27] = 0;
}
