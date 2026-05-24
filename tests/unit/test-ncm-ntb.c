/*
 * Unit tests for USB CDC NCM 1.0 NTB16 helpers (hw/usb/ncm-ntb.c).
 *
 * Byte-exact assertions, cross-checked against the host driver's
 * expectations in wasabi/src/ncm.rs (where build_ntb16 and
 * iter_ntb16_datagrams have matching test cases).
 *
 * Copyright (c) 2026 The QEMU contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "../../hw/usb/ncm-ntb.h"

/* ---------------- ncm_build_ntb16 ---------------- */

static void test_build_layout_42_byte_frame(void)
{
    uint8_t frame[42];
    uint8_t ntb[NCM_DGRAM_OFF + sizeof(frame)];
    uint32_t ntb_len;

    memset(frame, 0xab, sizeof(frame));
    ntb_len = ncm_build_ntb16(ntb, frame, sizeof(frame), 0x0042);

    /* 12 (NTH) + 16 (NDP) + 42 (frame) = 70 */
    g_assert_cmpuint(ntb_len, ==, 70);

    /* NTH16 */
    g_assert_cmpmem(ntb, 4, "NCMH", 4);
    g_assert_cmpuint(ntb[4] | (ntb[5] << 8), ==, 0x000c); /* wHeaderLength */
    g_assert_cmpuint(ntb[6] | (ntb[7] << 8), ==, 0x0042); /* wSequence */
    g_assert_cmpuint(ntb[8] | (ntb[9] << 8), ==, 70);     /* wBlockLength */
    g_assert_cmpuint(ntb[10] | (ntb[11] << 8), ==, 12);   /* wNdpIndex */

    /* NDP16 */
    g_assert_cmpmem(ntb + 12, 4, "NCM0", 4);
    g_assert_cmpuint(ntb[16] | (ntb[17] << 8), ==, 16);   /* wLength */
    g_assert_cmpuint(ntb[18] | (ntb[19] << 8), ==, 0);    /* wNextNdpIndex */
    g_assert_cmpuint(ntb[20] | (ntb[21] << 8), ==, 28);   /* wDatagramIndex */
    g_assert_cmpuint(ntb[22] | (ntb[23] << 8), ==, 42);   /* wDatagramLength */
    g_assert_cmpuint(ntb[24] | (ntb[25] << 8), ==, 0);    /* terminator idx */
    g_assert_cmpuint(ntb[26] | (ntb[27] << 8), ==, 0);    /* terminator len */

    /* Datagram bytes copied verbatim. */
    g_assert_cmpmem(ntb + 28, sizeof(frame), frame, sizeof(frame));
}

static void test_build_oversize_returns_zero(void)
{
    /* Block length = 28 + 0xffe8 = 0x10004, > uint16_t. Should refuse. */
    static uint8_t scratch[0x10100];
    g_assert_cmpuint(ncm_build_ntb16(scratch, scratch, 0xffe8, 0), ==, 0);
}

/* ---------------- ncm_walk_ntb16 ---------------- */

typedef struct {
    const uint8_t *frames[8];
    uint32_t       lens[8];
    unsigned       n;
} WalkCollector;

static void collect_cb(void *opaque, const uint8_t *frame, uint32_t len)
{
    WalkCollector *c = opaque;
    g_assert(c->n < ARRAY_SIZE(c->frames));
    c->frames[c->n] = frame;
    c->lens[c->n]   = len;
    c->n++;
}

static void test_walk_single_via_build(void)
{
    uint8_t frame[50];
    uint8_t ntb[NCM_DGRAM_OFF + sizeof(frame)];
    WalkCollector c = { 0 };
    unsigned n;

    memset(frame, 0xcd, sizeof(frame));
    ncm_build_ntb16(ntb, frame, sizeof(frame), 0);
    n = ncm_walk_ntb16(ntb, sizeof(ntb), collect_cb, &c);

    g_assert_cmpuint(n, ==, 1);
    g_assert_cmpuint(c.n, ==, 1);
    g_assert_cmpuint(c.lens[0], ==, sizeof(frame));
    g_assert_cmpmem(c.frames[0], c.lens[0], frame, sizeof(frame));
}

static void test_walk_multi_datagram(void)
{
    /*
     * Hand-built NTB16 with two 6-byte datagrams in a single NDP16,
     * matching the wasabi-side iter_datagrams_multi test case.
     *
     *   0..12  : NTH16
     *  12..32  : NDP16 (NCM0 + len + next + 2 entries + (0,0) terminator)
     *  32..38  : datagram 0  -> 1..6
     *  38..44  : datagram 1  -> 7..12
     */
    uint8_t ntb[44] = { 0 };
    WalkCollector c = { 0 };
    unsigned n;

    /* NTH16 */
    ntb[0] = 'N'; ntb[1] = 'C'; ntb[2] = 'M'; ntb[3] = 'H';
    ntb[4] = 12; ntb[5] = 0;                       /* wHeaderLength */
    ntb[6] = 0;  ntb[7] = 0;                       /* wSequence */
    ntb[8] = 44; ntb[9] = 0;                       /* wBlockLength */
    ntb[10] = 12; ntb[11] = 0;                     /* wNdpIndex */
    /* NDP16 */
    ntb[12] = 'N'; ntb[13] = 'C'; ntb[14] = 'M'; ntb[15] = '0';
    ntb[16] = 20; ntb[17] = 0;                     /* wLength = 20 */
    ntb[18] = 0;  ntb[19] = 0;                     /* wNextNdpIndex */
    ntb[20] = 32; ntb[21] = 0;                     /* dgram0 idx */
    ntb[22] = 6;  ntb[23] = 0;                     /* dgram0 len */
    ntb[24] = 38; ntb[25] = 0;                     /* dgram1 idx */
    ntb[26] = 6;  ntb[27] = 0;                     /* dgram1 len */
    /* ntb[28..32] = (0,0) terminator (already zero). */
    ntb[32] = 1; ntb[33] = 2; ntb[34] = 3;
    ntb[35] = 4; ntb[36] = 5; ntb[37] = 6;
    ntb[38] = 7; ntb[39] = 8; ntb[40] = 9;
    ntb[41] = 10; ntb[42] = 11; ntb[43] = 12;

    n = ncm_walk_ntb16(ntb, sizeof(ntb), collect_cb, &c);

    g_assert_cmpuint(n, ==, 2);
    g_assert_cmpuint(c.n, ==, 2);
    g_assert_cmpuint(c.lens[0], ==, 6);
    g_assert_cmpmem(c.frames[0], 6, "\x01\x02\x03\x04\x05\x06", 6);
    g_assert_cmpuint(c.lens[1], ==, 6);
    g_assert_cmpmem(c.frames[1], 6, "\x07\x08\x09\x0a\x0b\x0c", 6);
}

static void test_walk_rejects_bad_signature(void)
{
    uint8_t bad[44] = { 0 };
    WalkCollector c = { 0 };

    /* All zeros: missing "NCMH" signature. */
    g_assert_cmpuint(ncm_walk_ntb16(bad, sizeof(bad), collect_cb, &c), ==, 0);
    g_assert_cmpuint(c.n, ==, 0);
}

static void test_walk_rejects_short_buffer(void)
{
    uint8_t too_short[NCM_NTH16_LEN - 1] = { 0 };
    WalkCollector c = { 0 };
    g_assert_cmpuint(ncm_walk_ntb16(too_short, sizeof(too_short),
                                    collect_cb, &c), ==, 0);
    g_assert_cmpuint(c.n, ==, 0);
}

static void test_walk_rejects_block_len_overrun(void)
{
    uint8_t frame[8] = "abcdefgh";
    uint8_t ntb[NCM_DGRAM_OFF + sizeof(frame)];
    WalkCollector c = { 0 };

    ncm_build_ntb16(ntb, frame, sizeof(frame), 0);
    /* Lie: wBlockLength = NTB length + 1. Should refuse the walk. */
    ntb[8]++;
    g_assert_cmpuint(ncm_walk_ntb16(ntb, sizeof(ntb), collect_cb, &c), ==, 0);
    g_assert_cmpuint(c.n, ==, 0);
}

static void test_walk_rejects_datagram_out_of_bounds(void)
{
    uint8_t frame[8] = "abcdefgh";
    uint8_t ntb[NCM_DGRAM_OFF + sizeof(frame)];
    WalkCollector c = { 0 };

    ncm_build_ntb16(ntb, frame, sizeof(frame), 0);
    /* Inflate the datagram length so didx + dlen > block_len. */
    ntb[22] = 0xff; ntb[23] = 0xff;
    g_assert_cmpuint(ncm_walk_ntb16(ntb, sizeof(ntb), collect_cb, &c), ==, 0);
    g_assert_cmpuint(c.n, ==, 0);
}

static void test_walk_rejects_crc_variant(void)
{
    uint8_t frame[4] = "test";
    uint8_t ntb[NCM_DGRAM_OFF + sizeof(frame)];
    WalkCollector c = { 0 };

    ncm_build_ntb16(ntb, frame, sizeof(frame), 0);
    /* Swap the NDP signature from "NCM0" to "NCM1" (CRC variant). */
    ntb[15] = '1';
    g_assert_cmpuint(ncm_walk_ntb16(ntb, sizeof(ntb), collect_cb, &c), ==, 0);
    g_assert_cmpuint(c.n, ==, 0);
}

/* ---------------- ncm_fill_ntb_parameters ---------------- */

static void test_fill_ntb_parameters_matches_wasabi_expectation(void)
{
    /*
     * Reproduces the exact 28-byte response the WasabiOS NCM driver
     * logged during a live boot:
     *
     *   [28, 0, 1, 0, 0, 64, 0, 0,
     *    4, 0, 0, 0, 4, 0, 0, 0,
     *    0, 64, 0, 0,
     *    4, 0, 0, 0, 4, 0, 0, 0]
     *
     * with dwNtbIn/OutMaxSize = 16384.
     */
    uint8_t buf[NCM_NTB_PARAMS_LEN];
    static const uint8_t expected[NCM_NTB_PARAMS_LEN] = {
        28, 0,                  /* wLength */
        0x01, 0x00,             /* bmNtbFormatsSupported (NTB16 only) */
        0x00, 0x40, 0x00, 0x00, /* dwNtbInMaxSize = 16384 */
        4, 0,                   /* wNdpInDivisor */
        0, 0,                   /* wNdpInPayloadRemainder */
        4, 0,                   /* wNdpInAlignment */
        0, 0,                   /* wReserved */
        0x00, 0x40, 0x00, 0x00, /* dwNtbOutMaxSize = 16384 */
        4, 0,                   /* wNdpOutDivisor */
        0, 0,                   /* wNdpOutPayloadRemainder */
        4, 0,                   /* wNdpOutAlignment */
        0, 0,                   /* wNtbOutMaxDatagrams */
    };

    ncm_fill_ntb_parameters(buf, 16384, 16384);
    g_assert_cmpmem(buf, NCM_NTB_PARAMS_LEN, expected, sizeof(expected));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/ncm-ntb/build/layout-42",
                    test_build_layout_42_byte_frame);
    g_test_add_func("/ncm-ntb/build/oversize-rejected",
                    test_build_oversize_returns_zero);

    g_test_add_func("/ncm-ntb/walk/single-via-build",
                    test_walk_single_via_build);
    g_test_add_func("/ncm-ntb/walk/multi-datagram",
                    test_walk_multi_datagram);
    g_test_add_func("/ncm-ntb/walk/rejects-bad-signature",
                    test_walk_rejects_bad_signature);
    g_test_add_func("/ncm-ntb/walk/rejects-short-buffer",
                    test_walk_rejects_short_buffer);
    g_test_add_func("/ncm-ntb/walk/rejects-block-len-overrun",
                    test_walk_rejects_block_len_overrun);
    g_test_add_func("/ncm-ntb/walk/rejects-datagram-oob",
                    test_walk_rejects_datagram_out_of_bounds);
    g_test_add_func("/ncm-ntb/walk/rejects-crc-variant",
                    test_walk_rejects_crc_variant);

    g_test_add_func("/ncm-ntb/fill/matches-wasabi",
                    test_fill_ntb_parameters_matches_wasabi_expectation);

    return g_test_run();
}
