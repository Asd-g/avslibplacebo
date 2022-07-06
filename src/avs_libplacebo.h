#pragma once

#include <memory>
#include <string>
#include <vector>

#include "avisynth_c.h"

extern "C" {
#include <libplacebo/renderer.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/shaders.h>
#include <libplacebo/utils/upload.h>
#include <libplacebo/vulkan.h>
}

std::unique_ptr<struct priv> avs_libplacebo_init(VkPhysicalDevice device);
void avs_libplacebo_uninit(std::unique_ptr<struct priv> p);

struct format
{
    int num_comps;
    int bitdepth;
};

struct plane
{
    int subx, suby; // subsampling shift
    struct format fmt;
    size_t stride;
    void* data;
};

#define MAX_PLANES 4

struct image
{
    int width, height;
    int num_planes;
    struct plane planes[MAX_PLANES];
};

struct priv
{
    pl_log log;
    pl_vulkan vk;
    pl_gpu gpu;
    pl_dispatch dp;
    pl_shader_obj dither_state;

    pl_renderer rr;
    pl_tex tex_in[MAX_PLANES];
    pl_tex tex_out[MAX_PLANES];
};

AVS_Value AVSC_CC create_deband(AVS_ScriptEnvironment* env, AVS_Value args, void* param);
AVS_Value AVSC_CC create_resample(AVS_ScriptEnvironment* env, AVS_Value args, void* param);
