// Reference-counted native frame behind the public rcs_gpu_frame handle.
#pragma once

#include "media/video.hpp"
#include "util/api.hpp"

#include <atomic>

struct rcs_gpu_frame_impl {
    std::atomic<uint32_t> references{1};
    rcsdk::VideoFrame frame;
    rcs_gpu_frame_info info{};
};
