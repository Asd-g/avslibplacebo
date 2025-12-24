#include <iostream>

#include "avs_libplacebo.h"

static_assert(PL_API_VER >= 349, "libplacebo version must be at least v7.349.0.");

static void pl_logging(void* stream, pl_log_level level, const char* msg)
{
    constexpr const char* const constants_list[]{"[fatal] ", "[error] ", "[warn] ", "[info] ", "[debug] ", "[trace] "};

    if (level <= PL_LOG_WARN)
        std::cerr << constants_list[level - 1] << msg << "\n";
    else
        std::cout << constants_list[level - 1] << msg << "\n";
}

std::unique_ptr<struct priv> avs_libplacebo_init(const VkPhysicalDevice& device, std::string& err_msg)
{
    std::unique_ptr<priv> p{std::make_unique<priv>()};
    // std::cout.rdbuf(p->log_buffer.rdbuf());
    std::cerr.rdbuf(p->log_buffer.rdbuf());
    pl_log_params log_params{pl_logging, nullptr, PL_LOG_ERR};
    p->log = pl_log_create(0, &log_params);

    pl_vulkan_params vp{};
    vp.allow_software = true;

    if (device)
    {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        vp.device_name = properties.deviceName;
    }

    p->vk = pl_vulkan_create(p->log, &vp);
    if (!p->vk)
    {
        err_msg = p->log_buffer.str();
        pl_log_destroy(&p->log);
        return nullptr;
    }
    // Give this a shorter name for convenience
    p->gpu = p->vk->gpu;

    p->dp = pl_dispatch_create(p->log, p->gpu);
    if (!p->dp)
    {
        pl_vulkan_destroy(&p->vk);

        err_msg = p->log_buffer.str();
        pl_log_destroy(&p->log);
        return nullptr;
    }

    p->rr = pl_renderer_create(p->log, p->gpu);
    if (!p->rr)
    {
        pl_dispatch_destroy(&p->dp);
        pl_vulkan_destroy(&p->vk);

        err_msg = p->log_buffer.str();
        pl_log_destroy(&p->log);
        return nullptr;
    }

    return p;
}

void avs_libplacebo_uninit(const std::unique_ptr<struct priv>& p)
{
    pl_renderer_destroy(&p->rr);
    pl_dispatch_destroy(&p->dp);
    pl_vulkan_destroy(&p->vk);
    pl_log_destroy(&p->log);
}

AVS_Value devices_info(AVS_Clip* clip, AVS_ScriptEnvironment* env, std::vector<VkPhysicalDevice>& devices, VkInstance& inst,
    std::string& msg, const std::string& name, const int device, const int list_device)
{
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;

    uint32_t dev_count{0};

    if (vkCreateInstance(&info, nullptr, &inst))
    {
        vkDestroyInstance(inst, nullptr);
        avs_release_clip(clip);

        msg = name + ": failed to create instance.";
        return avs_new_value_error(msg.c_str());
    }

    if (vkEnumeratePhysicalDevices(inst, &dev_count, nullptr))
    {
        vkDestroyInstance(inst, nullptr);
        avs_release_clip(clip);

        msg = name + ": failed to get devices number.";
        return avs_new_value_error(msg.c_str());
    }

    if (device < -1 || device > static_cast<int>(dev_count) - 1)
    {
        vkDestroyInstance(inst, nullptr);
        avs_release_clip(clip);

        msg = name + ": device must be between -1 and " + std::to_string(dev_count - 1);
        return avs_new_value_error(msg.c_str());
    }

    devices.resize(dev_count);

    if (vkEnumeratePhysicalDevices(inst, &dev_count, devices.data()))
    {
        vkDestroyInstance(inst, nullptr);
        avs_release_clip(clip);

        msg = name + ": failed to get devices.";
        return avs_new_value_error(msg.c_str());
    }

    if (list_device)
    {
        for (size_t i{0}; i < devices.size(); ++i)
        {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(devices[i], &properties);

            msg += std::to_string(i) + ": " + std::string(properties.deviceName) + "\n";
        }

        vkDestroyInstance(inst, nullptr);

        AVS_Value cl{avs_new_value_clip(clip)};
        AVS_Value args_[2]{cl, avs_new_value_string(msg.c_str())};
        AVS_Value inv{avs_invoke(env, "Text", avs_new_value_array(args_, 2), 0)};

        avs_release_value(cl);
        avs_release_clip(clip);

        return inv;
    }
    else
        return avs_void;
}

AVS_Value avs_version(std::string& msg, const std::string& name, AVS_ScriptEnvironment* env)
{
    if (!avs_check_version(env, 9))
    {
        if (avs_check_version(env, 10))
        {
            if (avs_get_env_property(env, AVS_AEP_INTERFACE_BUGFIX) < 2)
            {
                msg = name + ": AviSynth+ version must be r3688 or later.";
                return avs_new_value_error(msg.c_str());
            }
        }
    }
    else
    {
        msg = name + ": AviSynth+ version must be r3688 or later.";
        return avs_new_value_error(msg.c_str());
    }

    return avs_void;
}

AVS_Value set_error(AVS_Clip* clip, const char* error_message, const std::unique_ptr<struct priv>& p)
{
    avs_release_clip(clip);

    if (p)
        avs_libplacebo_uninit(p);

    return avs_new_value_error(error_message);
}
