/* WASAPI Driver for ASIO - hand-written configuration of the vendored SoX Resampler library (soxr 0.1.3)
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Replaces the header that soxr's CMake build generates from src/soxr-config.h.in (the driver's builds do not run
 * CMake). Same macros, same meaning; every one is 0 or 1. See README-vendored.txt for the reasons.
 *
 *   No libavcodec / libavutil / PFFFT: the real FFT is soxr's own fft4g (fft4g32.c, fft4g64.c), plain C.
 *   Engines: only "cr32" (constant rate, single precision, plain C). No SIMD engines (cr32s / cr64s need SSE or
 *   AVX on x86 and are not portable to ARM64 / ARM64EC), no double-precision engine (cr64: only used for
 *   precision > 20 bits; the driver's recipe is 20-bit HQ), no variable-rate engine (vr32: the driver's ratio is
 *   a fixed exact fraction). soxr_create therefore always selects _soxr_rate32_cb.
 *   No developer trace (it would read the SOXR_TRACE environment variable and print to stderr).
 *   High-precision clock: as in soxr's default build (only used for irrational ratios; ours are rational).
 *   OpenMP is not used (the driver's builds never pass -fopenmp / /openmp), so ccrw2.h's locks are no-ops; see
 *   ARCHITECTURE.txt (section "soxr engine") for why this is safe in the driver.
 *   C99 headers and lrint exist in every toolchain used (mingw-w64 gcc, zig cc, MSVC 2015 or later).
 *   All Windows targets are little-endian.
 */

#if !defined soxr_config_included
#define soxr_config_included

#define AVCODEC_FOUND 0
#define AVUTIL_FOUND 0
#define WITH_PFFFT 0

#define HAVE_FENV_H 1
#define HAVE_STDBOOL_H 1
#define HAVE_STDINT_H 1
#define HAVE_LRINT 1
#define HAVE_BIGENDIAN 0

#define WITH_CR32 1
#define WITH_CR32S 0
#define WITH_CR64 0
#define WITH_CR64S 0
#define WITH_VR32 0

#define WITH_HI_PREC_CLOCK 1
#define WITH_FLOAT_STD_PREC_CLOCK 0
#define WITH_DEV_TRACE 0

/* Not a CMake setting: soxr's per-stage FIFOs (src/fifo.h) take their allocator from these macros when they are
 * defined (otherwise malloc / realloc / free). fifo-headroom.c gives each FIFO twice the space it asks for when
 * it grows and then satisfies later growth requests in place while they fit, so a FIFO that has grown during
 * the converter's warm-up (host thread) does not call realloc again on the audio thread when a later block
 * pattern needs slightly more room than the warm-up did. See fifo-headroom.c and src/frsoxr.h. Every source file
 * that includes fifo.h includes this header first (through internal.h / std-types.h). */
#include <stdlib.h>   /* size_t. Not <stddef.h>: fft4g32.c includes this header after "#define double float", and a
                       * first full <stddef.h> there would declare max_align_t with "long float"; <stdlib.h> is
                       * always included before (include guard), so nothing is declared twice. */
void * wb_soxr_fifo_malloc(size_t n);
void * wb_soxr_fifo_realloc(void * p, size_t n);
void wb_soxr_fifo_free(void * p);
/* FIFO_MIN (fifo.h default 16 KB) is both the initial size of a FIFO and the read offset beyond which fifo_reserve
 * compacts instead of growing. With 64 bytes a FIFO compacts (memmove of the few unread samples) whenever that
 * makes room, so it only grows when its unread data plus the write really exceed its size, and its size settles at
 * the largest such need, which the warm-up reaches (src/frsoxr.h feeds soxr in pieces of at most 128 frames). */
#define FIFO_MIN 64
#define FIFO_MALLOC(n) wb_soxr_fifo_malloc(n)
#define FIFO_REALLOC(p, n, old) wb_soxr_fifo_realloc(p, n)
#define FIFO_FREE(p) wb_soxr_fifo_free(p)

#endif
