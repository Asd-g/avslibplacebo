#pragma once

#include <memory>
#include <sstream>
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

std::unique_ptr<struct priv> avs_libplacebo_init(const VkPhysicalDevice& device, std::string& err_msg);
void avs_libplacebo_uninit(const std::unique_ptr<struct priv>& p);

[[maybe_unused]]
AVS_Value devices_info(AVS_Clip* clip, AVS_ScriptEnvironment* env, std::vector<VkPhysicalDevice>& devices, VkInstance& inst,
    std::string& msg, const std::string& name, const int device, const int list_device);
AVS_Value avs_version(std::string& msg, const std::string& name, AVS_ScriptEnvironment* env);
[[maybe_unused]]
AVS_Value set_error(AVS_Clip* clip, const char* error_message, const std::unique_ptr<struct priv>& p);

struct priv
{
    pl_log log;
    pl_vulkan vk;
    pl_gpu gpu;
    pl_dispatch dp;
    pl_shader_obj dither_state;

    pl_renderer rr;
    pl_tex tex_in[3];
    pl_tex tex_out[3];

    std::ostringstream log_buffer;
};

AVS_Value AVSC_CC create_deband(AVS_ScriptEnvironment* env, AVS_Value args, void* param);
AVS_Value AVSC_CC create_resample(AVS_ScriptEnvironment* env, AVS_Value args, void* param);
AVS_Value AVSC_CC create_shader(AVS_ScriptEnvironment* env, AVS_Value args, void* param);
AVS_Value AVSC_CC create_tonemap(AVS_ScriptEnvironment* env, AVS_Value args, void* param);
