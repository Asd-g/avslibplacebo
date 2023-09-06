#include "avs_libplacebo.h"

static_assert(PL_API_VER >= 269, "libplacebo version must be at least v6.287.0-rc1.");

std::unique_ptr<struct priv> avs_libplacebo_init(VkPhysicalDevice device)
{
    std::unique_ptr<priv> p{ std::make_unique<priv>() };

    pl_log_params pl_l{};
    pl_l.log_cb = pl_log_color;
    pl_l.log_level = PL_LOG_ERR;

    p->log = pl_log_create(PL_API_VER, &pl_l);

    if (!p->log)
    {
        fprintf(stderr, "Failed initializing libplacebo\n");
        goto error;
    }

    {
        pl_vulkan_params vp{ pl_vulkan_default_params };
        pl_vk_inst_params ip{ pl_vk_inst_default_params };
        //    ip.debug = true;
        vp.instance_params = &ip;

        if (device)
        {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(device, &properties);
            vp.device_name = properties.deviceName;
        }

        p->vk = pl_vulkan_create(p->log, &vp);
        if (!p->vk)
        {
            fprintf(stderr, "Failed creating vulkan context\n");
            goto error;
        }
        // Give this a shorter name for convenience
        p->gpu = p->vk->gpu;

        p->dp = pl_dispatch_create(p->log, p->gpu);
        if (!p->dp)
        {
            fprintf(stderr, "Failed creating shader dispatch object\n");
            goto error;
        }

        p->rr = pl_renderer_create(p->log, p->gpu);
        if (!p->rr)
        {
            fprintf(stderr, "Failed creating renderer\n");
            goto error;
        }

        return p;
    }

error:
    avs_libplacebo_uninit(std::move(p));
    return NULL;
}

void avs_libplacebo_uninit(std::unique_ptr<struct priv> p)
{
    for (int i{ 0 }; i < 4; ++i)
    {
        pl_tex_destroy(p->gpu, &p->tex_in[i]);
        pl_tex_destroy(p->gpu, &p->tex_out[i]);
    }

    pl_renderer_destroy(&p->rr);
    pl_shader_obj_destroy(&p->dither_state);
    pl_dispatch_destroy(&p->dp);
    pl_vulkan_destroy(&p->vk);
    pl_log_destroy(&p->log);
}

AVS_Value devices_info(AVS_Clip* clip, AVS_ScriptEnvironment* env, std::vector<VkPhysicalDevice>& devices, VkInstance& inst, std::string& msg, const std::string& name, const int device, const int list_device)
{
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;

    uint32_t dev_count{ 0 };

    if (vkCreateInstance(&info, nullptr, &inst))
    {
        vkDestroyInstance(inst, nullptr);
        avs_release_clip(clip);

        return avs_new_value_error((name + ": failed to create instance.").c_str());
    }

    if (vkEnumeratePhysicalDevices(inst, &dev_count, nullptr))
    {
        vkDestroyInstance(inst, nullptr);
        avs_release_clip(clip);

        return avs_new_value_error((name + ": failed to get devices number.").c_str());
    }

    if (device < -1 || device > static_cast<int>(dev_count) - 1)
    {
        msg = name + ": device must be between -1 and " + std::to_string(dev_count - 1);
        vkDestroyInstance(inst, nullptr);
        avs_release_clip(clip);

        return avs_new_value_error(msg.c_str());
    }

    devices.resize(dev_count);

    if (vkEnumeratePhysicalDevices(inst, &dev_count, devices.data()))
    {
        vkDestroyInstance(inst, nullptr);
        avs_release_clip(clip);

        return avs_new_value_error((name + ": failed to get devices.").c_str());
    }

    if (list_device)
    {
        for (size_t i{ 0 }; i < devices.size(); ++i)
        {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(devices[i], &properties);

            msg += std::to_string(i) + ": " + std::string(properties.deviceName) + "\n";
        }

        vkDestroyInstance(inst, nullptr);

        AVS_Value cl{ avs_new_value_clip(clip) };
        AVS_Value args_[2]{ cl, avs_new_value_string(msg.c_str()) };
        AVS_Value inv{ avs_invoke(env, "Text", avs_new_value_array(args_, 2), 0) };

        avs_release_value(cl);
        avs_release_clip(clip);

        return inv;
    }
    else
        return avs_void;
}

AVS_Value avs_version(const std::string& name, AVS_ScriptEnvironment* env)
{
    if (!avs_check_version(env, 9))
    {
        if (avs_check_version(env, 10))
        {
            if (avs_get_env_property(env, AVS_AEP_INTERFACE_BUGFIX) < 2)
                return avs_new_value_error((name + ": AviSynth+ version must be r3688 or later.").c_str());
        }
    }
    else
        return avs_new_value_error((name + ": AviSynth+ version must be r3688 or later.").c_str());

    return avs_void;
}
