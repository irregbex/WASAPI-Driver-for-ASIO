/* WASAPI Driver for ASIO - allocator with headroom for soxr's FIFOs (used through soxr-config.h)
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Why
 *   soxr keeps one FIFO per filter stage and channel (src/fifo.h). fifo_reserve() grows a FIFO with realloc()
 *   when a write does not fit; it grows by exactly the missing amount, so a FIFO can grow again whenever a block
 *   pattern needs a little more room than any pattern before. The driver warms every soxr instance up on the host
 *   thread (src/frsoxr.h), but the warm-up cannot reproduce every combination of block sizes the audio thread will
 *   later pass (the offline test found one: 48 -> 44.1 kHz with 1024-frame ASIO blocks grew a 36 KB FIFO to 44 KB
 *   during playback).
 * How
 *   Each block carries a small header with its real capacity. Every FIFO starts with a real capacity of
 *   WB_FIFO_FLOOR (64 KB, 16384 floats) whatever fifo_create asks for (FIFO_MIN, 64 bytes, see soxr-config.h); a
 *   growth request is satisfied in place while it fits the capacity; otherwise the block is reallocated to TWICE
 *   the requested size (host thread in practice: the warm-up runs first).
 *   Why 64 KB is enough: with FIFO_MIN = 64 a FIFO only needs (unread samples + the size of one write). src/frsoxr.h
 *   feeds soxr at most 128 frames per call and drains all output after each call, so a FIFO holds at most the
 *   output of one call plus about one FFT block; with the largest ratio the driver accepts (48, 8 -> 384 kHz) and
 *   FFT blocks of up to 4096 points (log2_large_dft_size = 10 allows more only for filters longer than 2^11 taps,
 *   which the HQ recipe does not design for these ratios) that is about 128 x 48 + 2 x 4096 = 14336 samples
 *   (56 KB). A larger need still works: the FIFO is reallocated (twice the size), which is what the stress test
 *   of CODE-REVIEW.txt (0.9.10) checks never happens on the audio thread: with soxr's default FIFO policy and
 *   512-frame calls some FIFOs grew there; with this policy none did.
 *   Memory: one 64 KB block per FIFO, i.e. per soxr stage and channel (2 or 3 stages + the output FIFO): about
 *   200 KB per converted channel, 6 MB for 32 channels; allocated on the host thread at stream open.
 *   fifo.h only ever calls these three functions through FIFO_MALLOC / FIFO_REALLOC / FIFO_FREE, and realloc()
 *   semantics (contents preserved) are kept. tests/fixedratio_test.cpp counts every allocation soxr makes on the
 *   audio-thread path (zero over 52 million frames of random driver-like traffic).
 * Alignment
 *   The header is a union of the widest scalar types, so the data keeps malloc's alignment for those types (soxr's
 *   plain C engine "cr32" stores floats and doubles in these FIFOs; no SIMD alignment is needed).
 */
#include <stdlib.h>
#include "soxr-config.h"

typedef union { size_t cap; double d; long double ld; void * p; long long ll; } wb_fifo_hdr_t;

#define WB_FIFO_FLOOR ((size_t)65536)   /* initial real capacity of every FIFO, bytes */

void * wb_soxr_fifo_malloc(size_t n)
{
  const size_t cap = n > WB_FIFO_FLOOR ? n : WB_FIFO_FLOOR;
  wb_fifo_hdr_t * h = (wb_fifo_hdr_t *)malloc(sizeof(wb_fifo_hdr_t) + cap);
  if (!h) return 0;
  h->cap = cap;
  return h + 1;
}

void * wb_soxr_fifo_realloc(void * p, size_t n)
{
  wb_fifo_hdr_t * h = p ? (wb_fifo_hdr_t *)p - 1 : 0;
  size_t want;
  if (h && h->cap >= n) return p;                       /* fits the headroom: no allocation */
  want = n <= ((size_t)-1 - sizeof(wb_fifo_hdr_t)) / 2 ? 2 * n : n;
  h = (wb_fifo_hdr_t *)realloc(h, sizeof(wb_fifo_hdr_t) + want);
  if (!h) return 0;                       /* out of memory: same as a failing realloc in fifo.h */
  h->cap = want;
  return h + 1;
}

void wb_soxr_fifo_free(void * p)
{
  if (p) free((wb_fifo_hdr_t *)p - 1);
}
