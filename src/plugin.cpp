#include "avslibplacebo.h"

std::unique_ptr<struct priv> avslibplacebo_init(VkPhysicalDevice device)
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
    avslibplacebo_uninit(std::move(p));
    return NULL;
}

void avslibplacebo_uninit(std::unique_ptr<struct priv> p)
{
    for (int i{ 0 }; i < MAX_PLANES; ++i)
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

const char* AVSC_CC avisynth_c_plugin_init(AVS_ScriptEnvironment* env)
{
    avs_add_function(env, "libplDeband", "c[iterations]i[threshold]f[radius]f[grain]f[dither]i[lut_size]i[temporal]b[planes]i*[device]i[list_device]b", Create_libplDeband, 0);
    return "avslibplacebo";
}
