SoX Resampler library (soxr) - vendored copy used by WASAPI Driver for ASIO (0.9.10)
SPDX-License-Identifier: LGPL-2.1-or-later (the library; see LICENCE and COPYING.LGPL)
===================================================================================

What
  The SoX Resampler library, "libsoxr", version 0.1.3 (released 2018-02-24), by
  robs@users.sourceforge.net (Rob Sykes) and contributors (AUTHORS). Official home:
  https://sourceforge.net/projects/soxr/ . License: GNU Lesser General Public License
  version 2.1 or (at your option) any later version (LICENCE, COPYING.LGPL).
  The driver uses it as the "soxr HQ" engine of its fixed-ratio sample-rate converter
  (src/frsoxr.h, src/fixedratio.h; ARCHITECTURE.txt section 60).

Where it was taken from
  git repository https://github.com/chirlu/soxr (a mirror of the upstream git repository),
  tag 0.1.3 = commit 945b592b70470e29f917f4de89b4281fbbd540c0 ("update NEWS, versions",
  2018-02-24), which is also the head of its master branch (no later commit exists). The
  SourceForge tarball (soxr-0.1.3-Source.tar.xz) could not be downloaded from the build
  environment, so the copy was not compared with it byte by byte.

Which files (unchanged, byte for byte, LF line ends as upstream)
  src/  the files needed for the constant-rate single-precision engine "cr32" in plain C:
        soxr.c data-io.c dbesi0.c filter.c fft4g32.c fft4g64.c cr.c cr32.c, the files they
        include (cr-core.c, fft4g.c and the headers), and soxr.h. Nothing else of upstream
        is needed: no SIMD engines (cr32s, cr64s, pffft, util*s, dev*s, avfft), no
        double-precision engine (cr64), no variable-rate engine (vr32, vr-coefs), no
        libsamplerate wrapper (soxr-lsr), no CMake files, tests or examples.
        data-io.c is NOT compiled directly (see data-io-portable.c below).
  LICENCE, COPYING.LGPL, AUTHORS, NEWS   from the upstream root.
  SHA-256 of every vendored upstream file:
  5ad419882124571d5e7c031f6c2eeaec3b7f7a2daba3dc2a906b2be65fff8fd8  src/aliases.h
  620542698a4af0cd503b35e94965f76f0696681dc283c4963080c808ea919074  src/ccrw2.h
  ec8755248757ad1116b0e783cd8546e69b3a9809e29e5481fb9ec703a36e8777  src/cr-core.c
  224322c832160c03c0ab27277e4c6282c880606c63624e624e6890081361be5c  src/cr.c
  c712cda53c3ac23bdfda9bc8658be00420b007afab9ef019a5ebd391fead445d  src/cr.h
  6eaf2b8b33f75c4892c83c7aeb458f8040d416d494e39e442c699a57719d2876  src/cr32.c
  e88ee7cfddac5dcbb6a9fc6cd246d2de8526fa35973c5a84a55abb7c0a4bd8c7  src/data-io.c
  6001cee72944963da2b58bc4c47c1186bba4dda480b0dc31c6b8b0c56da80b6e  src/data-io.h
  06ea6d16a2d781c77a3f528357c42a7d4559a39b515dbf8109290d679edc3394  src/dbesi0.c
  604e71c0dcebe1922f718f43077ccebf588c770aa76e37d28b70cbefbaa2eca1  src/fft4g.c
  81c587dfca362c501ab11f7a245135175217b1c58da1b85632372b769978e6aa  src/fft4g.h
  a0dd216835fdf11a55a815ece61013e91c65fb3f356f23fba4137fd813b6f1ee  src/fft4g32.c
  8f94a82d565245e1afd28e0f4ce572ed0abf380a9bbf6718fc5e54d59d286d87  src/fft4g64.c
  1eea9b4a924e6004b438f64352b74399fbb3bcea57ee936c0ad44bdc6b2e325b  src/fft4g_cache.h
  400502cea5c47022b0350f14b90209470f6cbc2e15c96e5adb9297bb5d44d28f  src/fifo.h
  86bff80d41d720f2758ef7cf77a4bd2c3c4b2d8ab9a310cb1c9d1daab2fc537a  src/filter.c
  419065db3578a061fe822d091b641409df7761fb791e5570c71e31aa64dc9801  src/filter.h
  95d3b41777f070544ef394ef0a031f0be51035590285f66698b1cadd6422abf8  src/half-coefs.h
  f2f683646d1a25ac2c7b3e275d9fd9bac82a81d1f8cae50e1fb535b94a371863  src/half-fir.h
  25208e096c6f67029d9cbc29acda3580031a02454f81d28eb84fac35efaf96a0  src/internal.h
  444ecdd5403f8de33682f032f4c2d002207c5bfe0fcde2e11e43e71dd3b87600  src/math-wrap.h
  178fdb1ccac2ff24f2d5d856b63a3d6b756830a6254326381e06ce46b9c4a4a4  src/poly-fir.h
  c06b652aaa9e034729782ff52c8a8f68df6eb956dc000cd521590f349df92559  src/poly-fir0.h
  2a0c68bce7395ad380b7ec19f1c73ee57a46e9cd50e339e48aaa4e335e0fa531  src/rdft.h
  0641f2c59f4634ebc67001d474405fb6c1210985b812ed914054e5c343c36f6f  src/rdft_t.h
  63265a84f83aec1b66ab790181312c487d1bd525debe7ee0d15716f617610b11  src/rint-clip.h
  1f306bce28e40cd48b5cd8ea68eb5ddf1c4123fc98768f94d9dda05dde2721eb  src/rint.h
  d54e1b84bcaf73c38024c66bd594bb55fb02247afec8d726e27e30acd4d44421  src/soxr.c
  dfd18fae32b77b484bda918dbaac122ddd402512041a96977fc01377f69684a4  src/soxr.h
  b10b0d00a8b1f8a04207dd5efa331fe69a1a315f4da4ddf0f7a9586c83f22d01  src/std-types.h
  f2f118b9029ec1871b953639ecc46651b2fc7b62e295e6cf3ef2ac4c9a058b33  COPYING.LGPL
  dc98676341fdcd29d9f279c9679d6a75288785b174ded8d1b2e316c366166135  LICENCE
  e5d53cd6c716dfeabe47a9f3613a8c6815aa4a67f0f77900787c59445396aeff  AUTHORS
  490966b220ad4b335057c5c92ecc47380c346709b08f942354df085d05163211  NEWS

Files of this project (not from upstream, CRLF line ends like the rest of the driver)
  soxr-config.h        Hand-written replacement for the soxr-config.h that CMake generates
                       from src/soxr-config.h.in (the driver's builds do not run CMake):
                       only WITH_CR32 = 1 (no SIMD, no cr64, no vr32), no AVCODEC / AVUTIL /
                       PFFFT, WITH_DEV_TRACE = 0, WITH_HI_PREC_CLOCK = 1 (CMake default),
                       C99 headers and lrint available, little-endian. It also sets the
                       allocator macros of src/fifo.h (FIFO_MALLOC / FIFO_REALLOC /
                       FIFO_FREE -> fifo-headroom.c) and FIFO_MIN = 64 (see there and
                       src/frsoxr.h, "Real-time safety").
  fifo-headroom.c      The FIFO allocator: a FIFO that grows gets twice the requested size
                       and later growth is satisfied in place while it fits, so the audio
                       thread does not allocate after the host-thread warm-up.
  data-io-portable.c   Compiles src/data-io.c with rint.h's portable rounding macros instead
                       of src/rint.h (x87 inline assembly for GCC on x86, SSE2 intrinsics for
                       MSVC _M_X64, which ARM64EC also defines). Those functions only convert
                       to integer output types, which the driver never uses (float in/out).
  README-vendored.txt  This file.

How it is built (no CMake; see BUILDING.txt)
  Compiled as C, one object per file: src/soxr.c data-io-portable.c fifo-headroom.c
  src/dbesi0.c src/filter.c src/fft4g32.c src/fft4g64.c src/cr.c src/cr32.c with
  -DSOXR_LIB -DNDEBUG -I third_party/soxr (soxr-config.h) -O2, and warnings switched off
  for these files only (-w with gcc / zig cc, /W0 with MSVC; third-party code; the driver's
  own sources keep -Wall -Wextra and 0 warnings). MSVC (Arm64X DLL): cl /TC for the native
  ARM64 and the /arm64EC half (scripts/build-arm64x.ps1). No OpenMP (never -fopenmp / /openmp),
  so soxr runs single-threaded and its FFT-table lock (ccrw2.h) compiles to nothing. Static
  linking: no soxr DLL exists or is needed.

License notes (see THIRD-PARTY-NOTICES.txt section 6)
  soxr is LGPL-2.1-or-later and keeps that license and its notices in this folder; the
  driver, a GPL-3.0-or-later program, links it statically. That combination is permitted
  (LGPL 2.1 section 6, "work that uses the Library"; section 3 even allows a copy to be
  conveyed under the GPL v2 or later). Section 6's conditions are met: the Setup ships the
  LGPL 2.1 text (THIRD-PARTY-NOTICES.txt, appendix) with a notice that soxr is used, and
  the complete corresponding source of the library (this folder) and of the whole driver is
  published under the GPL, so anyone can rebuild the driver with a modified soxr.
