/* Copyright (c) 2011 The WebM project authors. All Rights Reserved. */
/*  */
/* Use of this source code is governed by a BSD-style license */
/* that can be found in the LICENSE file in the root of the source */
/* tree. An additional intellectual property rights grant can be found */
/* in the file PATENTS.  All contributing project authors may */
/* be found in the AUTHORS file in the root of the source tree. */
#include "vpx/vpx_codec.h"
static const char* const cfg = "--target=x86_64-win64-vs17 --enable-external-build --enable-realtime-only --enable-vp9-highbitdepth --enable-multithread --enable-pic --disable-examples --disable-tools --disable-docs --disable-unit-tests --disable-install-docs --disable-install-bins --disable-install-libs --disable-install-srcs --disable-webm-io --disable-libyuv --disable-debug --disable-gprof --disable-gcov --enable-runtime-cpu-detect --as=nasm";
const char *vpx_codec_build_config(void) {return cfg;}
