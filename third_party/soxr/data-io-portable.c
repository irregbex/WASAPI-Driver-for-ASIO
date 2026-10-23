/* WASAPI Driver for ASIO - soxr's data-io.c with portable rounding (no inline assembly, no SSE intrinsics)
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Compiled INSTEAD of src/data-io.c by every build of the driver and by the offline tests (README-vendored.txt).
 * src/data-io.c is the only user of src/rint.h, which picks the rounding code by compiler and target:
 *   - GCC on x86 / x64:        x87 inline assembly (fistp / fnstsw / fldenv);
 *   - MSVC with _M_X64:        SSE2 intrinsics (_mm_cvtsd_si32 from <emmintrin.h>) and _statusfp / _clearfp.
 *     ARM64EC code (the x64-compatible half of the Arm64X DLL, cl /arm64EC) ALSO defines _M_X64, so it would get
 *     the SSE2 branch, whose intrinsics exist on ARM64EC only through emulation headers;
 *   - MSVC with _M_IX86:       x87 __asm blocks (not available on x64 / ARM64 compilers anyway).
 * Those functions only round floating-point samples to 16- or 32-bit integers, i.e. they are used only when soxr's
 * OUTPUT type is an integer. The driver always uses SOXR_FLOAT32_I in and out (frsoxr.h), so the code is never
 * executed; it only has to compile everywhere. Defining rint.h's include guard and the portable definitions that
 * rint.h itself falls back to (its "#if !defined FPU_RINT32" branch) gives the same plain-C code on every target.
 * src/data-io.c is included unchanged.
 */

#define soxr_rint_included                 /* skip src/rint.h */
#include "src/std-types.h"

/* rint.h's portable fallbacks, verbatim (round half away from zero; the clipping code checks the range) */
#define rint32D(y,x) ((y)=(int32_t)((x) < 0? x - .5 : x + .5))
#define rint32F(y,x) rint32D(y,(double)(x))
#define rint16D(y,x) ((y)=(int16_t)((x) < 0? x - .5 : x + .5))
#define rint16F(y,x) rint16D(y,(double)(x))

#include "src/data-io.c"
