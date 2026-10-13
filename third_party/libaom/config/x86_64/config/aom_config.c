/*
 * Copyright (c) 2016, Alliance for Open Media. All rights reserved.
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */
#include "aom/aom_codec.h"
static const char* const cfg = "-DAOM_TARGET_CPU=x86_64 -DCONFIG_AV1_DECODER=0 -DCONFIG_LIBYUV=0 -DCONFIG_AV1_HIGHBITDEPTH=1 -DCONFIG_MULTITHREAD=1 -DCONFIG_PIC=1 -DCONFIG_REALTIME_ONLY=1 -DCONFIG_RUNTIME_CPU_DETECT=1 -DCONFIG_WEBM_IO=0 -DCONFIG_DENOISE=0 -DENABLE_APPS=0 -DENABLE_DOCS=0 -DENABLE_EXAMPLES=0 -DENABLE_NASM=1 -DENABLE_TESTS=0 -DENABLE_TOOLS=0 -DENABLE_SVE=0 -DENABLE_SVE2=0";
const char *aom_codec_build_config(void) {return cfg;}
