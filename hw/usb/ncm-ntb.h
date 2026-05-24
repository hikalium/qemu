/*
 * USB CDC NCM 1.0 NTB16 helpers
 *
 * Pure encoding / decoding of NTB16 (no-CRC) "NCMH" / "NCM0" frames
 * exchanged on the bulk endpoints of a CDC NCM device. Extracted from
 * dev-ncm.c so a unit test in tests/unit/ can exercise the bit layout
 * without spinning up the whole USB stack.
 *
 * Copyright (c) 2026 The QEMU contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QEMU_HW_USB_NCM_NTB_H
#define QEMU_HW_USB_NCM_NTB_H

/*
 * Wire-format constants for NTB16 (no-CRC) carrying one Ethernet frame:
 *
 *   off  0..12  : NTH16  (signature "NCMH", wHeaderLength = 12)
 *   off 12..28  : NDP16  (signature "NCM0", one (idx,len) + (0,0) term)
 *   off 28..N+28: datagram bytes
 *
 * Total block length = 28 + N, which is 4-aligned for any N (the
 * conservative wNdpInAlignment / wNdpOutAlignment we advertise).
 */
#define NCM_NTH16_LEN  12
#define NCM_NDP16_LEN  16
#define NCM_DGRAM_OFF  (NCM_NTH16_LEN + NCM_NDP16_LEN)

/* Length of the GET_NTB_PARAMETERS response (NTB16-only). */
#define NCM_NTB_PARAMS_LEN 28

/*
 * Encode `frame_len` bytes from `frame` into a single-datagram NTB16
 * in `dst`. Caller guarantees dst has at least NCM_DGRAM_OFF + frame_len
 * bytes available. Returns the total NTB length, or 0 if the requested
 * block length doesn't fit in NTH16.wBlockLength (uint16_t).
 */
uint32_t ncm_build_ntb16(uint8_t *dst, const uint8_t *frame,
                         uint32_t frame_len, uint16_t seq);

/*
 * Callback invoked once per parsed datagram by ncm_walk_ntb16().
 * The `frame` pointer is borrowed from the input NTB; do not retain.
 */
typedef void (*ncm_ntb16_datagram_cb)(void *opaque,
                                      const uint8_t *frame, uint32_t len);

/*
 * Walk one NTB16 no-CRC and invoke `cb` for each datagram, in order.
 * Returns the count of datagrams emitted. Returns 0 on any framing
 * error (bad signature, bad header_length, NDP out of bounds,
 * datagram out of bounds). Multi-NDP chaining via wNextNdpIndex is
 * NOT followed; the wasabi driver and the Linux gadget both put all
 * datagrams in the first NDP.
 */
unsigned ncm_walk_ntb16(const uint8_t *ntb, uint32_t len,
                        ncm_ntb16_datagram_cb cb, void *opaque);

/*
 * Fill the 28-byte NTB Parameter Structure response to
 * GET_NTB_PARAMETERS. Advertises NTB16 only, wNdp{In,Out}Alignment = 4,
 * wNdp{In,Out}Divisor = 4, wNtbOutMaxDatagrams = 0 (no limit). `in_max`
 * and `out_max` set dwNtbInMaxSize / dwNtbOutMaxSize respectively.
 */
void ncm_fill_ntb_parameters(uint8_t *out, uint32_t in_max, uint32_t out_max);

#endif /* QEMU_HW_USB_NCM_NTB_H */
