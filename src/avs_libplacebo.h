#pragma once

#include <memory>
#include <string>
#include <vector>

#include "avisynth_c.h"

extern "C"
{
#include "libplacebo/dispatch.h"
#include "libplacebo/renderer.h"
#include "libplacebo/shaders.h"
#include "libplacebo/utils/upload.h"
#include "libplacebo/vulkan.h"
}

std::unique_ptr<struct priv> avs_libplacebo_init(VkPhysicalDevice device);
void avs_libplacebo_uninit(std::unique_ptr<struct priv> p);

AVS_Value devices_info(AVS_Clip* clip, AVS_ScriptEnvironment* env, std::vector<VkPhysicalDevice>& devices, VkInstance& inst, std::string& msg, const std::string& name, const int device, const int list_device);
AVS_Value avs_version(const std::string& name, AVS_ScriptEnvironment* env);

struct priv
{
    pl_log log;
    pl_vulkan vk;
    pl_gpu gpu;
    pl_dispatch dp;
    pl_shader_obj dither_state;

    pl_renderer rr;
    pl_tex tex_in[4];
    pl_tex tex_out[4];
};

AVS_Value AVSC_CC create_deband(AVS_ScriptEnvironment* env, AVS_Value args, void* param);
AVS_Value AVSC_CC create_resample(AVS_ScriptEnvironment* env, AVS_Value args, void* param);
AVS_Value AVSC_CC create_shader(AVS_ScriptEnvironment* env, AVS_Value args, void* param);
AVS_Value AVSC_CC create_tonemap(AVS_ScriptEnvironment* env, AVS_Value args, void* param);

[[maybe_unused]]
static AVS_FORCEINLINE AVS_Value set_error(AVS_Clip* clip, const char* error_message)
{
    avs_release_clip(clip);

    return avs_new_value_error(error_message);
}
