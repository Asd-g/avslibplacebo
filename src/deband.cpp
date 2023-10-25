#include <mutex>

#include "avs_libplacebo.h"

static std::mutex mtx;

struct deband
{
    std::unique_ptr<priv> vf;
    int process[3];
    int dither;
    std::unique_ptr<pl_dither_params> dither_params;
    std::unique_ptr<pl_deband_params> deband_params;
    std::unique_ptr<pl_deband_params> deband_params1;
    uint8_t frame_index;
    std::string msg;
};

static bool deband_do_plane(priv& p, deband& data, const int planeIdx) noexcept
{
    pl_shader sh{ pl_dispatch_begin(p.dp) };

    pl_shader_params sh_p{};
    sh_p.gpu = p.gpu;
    sh_p.index = data.frame_index++;

    pl_shader_reset(sh, &sh_p);

    pl_sample_src src{};
    src.tex = p.tex_in[0];

    pl_shader_deband(sh, &src, ((planeIdx == AVS_PLANAR_U || planeIdx == AVS_PLANAR_V) &&
        data.deband_params1) ? data.deband_params1.get() : data.deband_params.get());

    if (data.dither)
        pl_shader_dither(sh, p.tex_out[0]->params.format->component_depth[0], &p.dither_state, data.dither_params.get());

    pl_dispatch_params d_p{};
    d_p.target = p.tex_out[0];
    d_p.shader = &sh;

    return pl_dispatch_finish(p.dp, &d_p);
}

static int deband_reconfig(priv& p, const pl_plane_data& data, AVS_VideoFrame* dst, const int planeIdx)
{
    pl_fmt fmt{ pl_plane_find_fmt(p.gpu, nullptr, &data) };
    if (!fmt)
        return -1;

    pl_tex_params t_r{};
    t_r.w = data.width;
    t_r.h = data.height;
    t_r.format = fmt;
    t_r.sampleable = true;
    t_r.host_writable = true;

    if (pl_tex_recreate(p.gpu, &p.tex_in[0], &t_r))
    {
        t_r.w = avs_get_row_size_p(dst, planeIdx) / data.pixel_stride;
        t_r.h = avs_get_height_p(dst, planeIdx);
        t_r.sampleable = false;
        t_r.host_writable = false;
        t_r.renderable = true;
        t_r.host_readable = true;

        if (!pl_tex_recreate(p.gpu, &p.tex_out[0], &t_r))
            return -2;
    }
    else
        return -2;

    return 0;
}

static int deband_filter(priv& p, AVS_VideoFrame* dst, const pl_plane_data& data, deband& d, const int planeIdx)
{
    // Upload planes
    pl_tex_transfer_params ttr{};
    ttr.tex = p.tex_in[0];
    ttr.row_pitch = data.row_stride;
    ttr.ptr = const_cast<void*>(data.pixels);

    if (!pl_tex_upload(p.gpu, &ttr))
        return -1;

    // Process plane
    if (!deband_do_plane(p, d, planeIdx))
        return -2;

    ttr.tex = p.tex_out[0];
    ttr.row_pitch = avs_get_pitch_p(dst, planeIdx);
    ttr.ptr = reinterpret_cast<void*>(avs_get_write_ptr_p(dst, planeIdx));

    // Download planes
    if (!pl_tex_download(p.gpu, &ttr))
        return -3;

    return 0;
}

static AVS_VideoFrame* AVSC_CC deband_get_frame(AVS_FilterInfo* fi, int n)
{
    deband* d{ reinterpret_cast<deband*>(fi->user_data) };

    const char* ErrorText{ 0 };
    AVS_VideoFrame* src{ avs_get_frame(fi->child, n) };
    if (!src)
        return nullptr;

    AVS_VideoFrame* dst{ avs_new_video_frame_p(fi->env, &fi->vi, src) };

    constexpr int planes_y[3]{ AVS_PLANAR_Y, AVS_PLANAR_U, AVS_PLANAR_V };
    constexpr int planes_r[3]{ AVS_PLANAR_R, AVS_PLANAR_G, AVS_PLANAR_B };
    const int* planes{ (avs_is_rgb(&fi->vi)) ? planes_r : planes_y };
    const int num_planes{ std::min(avs_num_components(&fi->vi), 3) };

    for (int i{ 0 }; i < num_planes; ++i)
    {
        if (d->process[i] == 2)
            avs_bit_blt(fi->env, avs_get_write_ptr_p(dst, planes[i]), avs_get_pitch_p(dst, planes[i]), avs_get_read_ptr_p(src, planes[i]), avs_get_pitch_p(src, planes[i]), avs_get_row_size_p(src, planes[i]), avs_get_height_p(src, planes[i]));
        else if (d->process[i] == 3)
        {
            pl_plane_data plane{};
            plane.type = (avs_component_size(&fi->vi) < 4) ? PL_FMT_UNORM : PL_FMT_FLOAT;
            plane.width = avs_get_row_size_p(src, planes[i]) / avs_component_size(&fi->vi);
            plane.height = avs_get_height_p(src, planes[i]);
            plane.pixel_stride = avs_component_size(&fi->vi);
            plane.row_stride = avs_get_pitch_p(src, planes[i]);
            plane.pixels = avs_get_read_ptr_p(src, planes[i]);
            plane.component_size[0] = { avs_bits_per_component(&fi->vi) };
            plane.component_pad[0] = 0;
            plane.component_map[0] = 0;

            {
                std::lock_guard<std::mutex> lck(mtx);

                const int reconf{ deband_reconfig(*d->vf.get(), plane, dst, planes[i]) };
                if (reconf == 0)
                {
                    const int filt{ deband_filter(*d->vf.get(), dst, plane, *d, planes[i]) };
                    if (filt < 0)
                    {
                        switch (filt)
                        {
                            case -1: ErrorText = "libplacebo_Deband: failed uploading data to the GPU!"; break;
                            case -2: ErrorText = "libplacebo_Deband: failed processing planes!"; break;
                            default: ErrorText = "libplacebo_Deband: failed downloading data from the GPU!";
                        }
                    }
                }
                else
                {
                    switch (reconf)
                    {
                        case -1: ErrorText = "libplacebo_Deband: failed configuring filter: no good texture format!"; break;
                        default: ErrorText = "libplacebo_Deband: failed creating GPU textures!";
                    }
                }
            }
        }
    }

    if (ErrorText)
    {
        avs_release_video_frame(src);
        avs_release_video_frame(dst);

        fi->error = ErrorText;

        return nullptr;
    }
    else
    {
        if (avs_num_components(&fi->vi) > 3)
            avs_bit_blt(fi->env, avs_get_write_ptr_p(dst, AVS_PLANAR_A), avs_get_pitch_p(dst, AVS_PLANAR_A), avs_get_read_ptr_p(src, AVS_PLANAR_A), avs_get_pitch_p(src, AVS_PLANAR_A),
                avs_get_row_size_p(src, AVS_PLANAR_A), avs_get_height_p(src, AVS_PLANAR_A));

        avs_release_video_frame(src);

        return dst;
    }
}

static void AVSC_CC free_deband(AVS_FilterInfo* fi)
{
    deband* d{ reinterpret_cast<deband*>(fi->user_data) };

    pl_shader_obj_destroy(&d->vf->dither_state);
    avs_libplacebo_uninit(d->vf);
    delete d;
}

static int AVSC_CC deband_set_cache_hints(AVS_FilterInfo* fi, int cachehints, int frame_range)
{
    return cachehints == AVS_CACHE_GET_MTMODE ? 2 : 0;
}

AVS_Value AVSC_CC create_deband(AVS_ScriptEnvironment* env, AVS_Value args, void* param)
{
    enum { Clip, Iterations, Threshold, Radius, Grainy, Grainc, Dither, Lut_size, Temporal, Planes, Device, List_device, Grain_neutral };

    AVS_FilterInfo* fi;
    AVS_Clip* clip{ avs_new_c_filter(env, &fi, avs_array_elt(args, Clip), 1) };

    deband* params{ new deband() };

    AVS_Value avs_ver{ avs_version("libplacebo_Deband", env) };
    if (avs_is_error(avs_ver))
        return avs_ver;

    if (!avs_is_planar(&fi->vi))
        return set_error(clip, "libplacebo_Deband: clip must be in planar format.", nullptr);
    if (avs_bits_per_component(&fi->vi) != 8 && avs_bits_per_component(&fi->vi) != 16 && avs_bits_per_component(&fi->vi) != 32)
        return set_error(clip, "libplacebo_Deband: bit depth must be 8, 16 or 32-bit.", nullptr);

    const int device{ avs_defined(avs_array_elt(args, Device)) ? avs_as_int(avs_array_elt(args, Device)) : -1 };
    const int list_device{ avs_defined(avs_array_elt(args, List_device)) ? avs_as_bool(avs_array_elt(args, List_device)) : 0 };

    if (list_device || device > -1)
    {
        std::vector<VkPhysicalDevice> devices{};
        VkInstance inst{};

        AVS_Value dev_info{ devices_info(clip, fi->env, devices, inst, params->msg, "libplacebo_Deband", device, list_device) };
        if (avs_is_error(dev_info) || avs_is_clip(dev_info))
            return dev_info;

        params->vf = avs_libplacebo_init(devices[device], params->msg);

        vkDestroyInstance(inst, nullptr);
    }
    else
    {
        if (device < -1)
            return set_error(clip, "libplacebo_Deband: device must be greater than or equal to -1.", nullptr);

        params->vf = avs_libplacebo_init(nullptr, params->msg);
    }

    if (params->msg.size())
    {
        params->msg = "libplacebo_Deband: " + params->msg;
        return set_error(clip, params->msg.c_str(), nullptr);
    }

    if (avs_bits_per_component(&fi->vi) == 8)
    {
        params->dither = avs_defined(avs_array_elt(args, Dither)) ? avs_as_bool(avs_array_elt(args, Dither)) : 1;

        if (params->dither)
        {
            params->dither_params = std::make_unique<pl_dither_params>();
            params->dither_params->method = static_cast<pl_dither_method>(params->dither - 1);
            if (params->dither_params->method < 0 || params->dither_params->method > 4)
                return set_error(clip, "libplacebo_Deband: dither must be between 0..4", params->vf);

            params->dither_params->lut_size = (avs_defined(avs_array_elt(args, Lut_size))) ? avs_as_int(avs_array_elt(args, Lut_size)) : 6;
            if (params->dither_params->lut_size > 8)
                return set_error(clip, "libplacebo_Deband: lut_size must be less than or equal to 8", params->vf);

            params->dither_params->temporal = (avs_defined(avs_array_elt(args, Temporal))) ? avs_as_bool(avs_array_elt(args, Temporal)) : false;
        }
    }
    else
        params->dither = false;

    if (avs_is_rgb(&fi->vi))
    {
        params->process[0] = 3;
        params->process[1] = 3;
        params->process[2] = 3;
    }
    else
    {
        params->process[0] = 3;
        params->process[1] = 2;
        params->process[2] = 2;

        const int num_planes{ (avs_defined(avs_array_elt(args, Planes))) ? avs_array_size(avs_array_elt(args, Planes)) : 0 };
        if (num_planes > avs_num_components(&fi->vi))
            return set_error(clip, "libplacebo_Deband: plane index out of range.", params->vf);

        for (int i{ 0 }; i < num_planes; ++i)
        {
            const int plane_v{ avs_as_int(*(avs_as_array(avs_array_elt(args, Planes)) + i)) };
            if (plane_v < 1 || plane_v > 3)
                return set_error(clip, "libplacebo_Deband: plane must be between 1..3.", params->vf);

            params->process[i] = avs_as_int(*(avs_as_array(avs_array_elt(args, Planes)) + i));
        }
    }

    params->deband_params = std::make_unique<pl_deband_params>();
    params->deband_params->iterations = (avs_defined(avs_array_elt(args, Iterations))) ? avs_as_int(avs_array_elt(args, Iterations)) : 1;
    if (params->deband_params->iterations < 0)
        return set_error(clip, "libplacebo_Deband: iterations must be greater than or equal to 0.", params->vf);

    params->deband_params->threshold = (avs_defined(avs_array_elt(args, Threshold))) ? avs_as_float(avs_array_elt(args, Threshold)) : 4.0f;
    if (params->deband_params->threshold < 0.0f)
        return set_error(clip, "libplacebo_Deband: threshold must be greater than or equal to 0.0", params->vf);

    params->deband_params->radius = (avs_defined(avs_array_elt(args, Radius))) ? avs_as_float(avs_array_elt(args, Radius)) : 16.0f;
    if (params->deband_params->radius < 0.0f)
        return set_error(clip, "libplacebo_Deband: radius must be greater than or equal to 0.0", params->vf);

    if (avs_defined(avs_array_elt(args, Grain_neutral)))
    {
        const int grain_neutral_num{ avs_array_size(avs_array_elt(args, Grain_neutral)) };
        if (grain_neutral_num > avs_num_components(&fi->vi))
            return set_error(clip, "libplacebo_Deband: grain_neutral index out of range.", params->vf);

        for (int i{ 0 }; i < grain_neutral_num; ++i)
        {
            params->deband_params->grain_neutral[i] = avs_as_float(*(avs_as_array(avs_array_elt(args, Grain_neutral)) + i));
            if (params->deband_params->grain_neutral[i] < 0.0f)
                return set_error(clip, "libplacebo_Deband: grain_neutral must be greater than or equal to 0.0", params->vf);
        }
    }

    params->deband_params->grain = (avs_defined(avs_array_elt(args, Grainy))) ? avs_as_float(avs_array_elt(args, Grainy)) : 6.0f;
    if (params->deband_params->grain < 0.0f)
        return set_error(clip, "libplacebo_Deband: grainY must be greater than or equal to 0.0", params->vf);

    const float grainC{ static_cast<float>((avs_defined(avs_array_elt(args, Grainc))) ? avs_as_float(avs_array_elt(args, Grainc)) : params->deband_params->grain) };
    if (grainC < 0.0f)
        return set_error(clip, "libplacebo_Deband: grainC must be greater than or equal to 0.0", params->vf);

    if (params->deband_params->grain != grainC)
    {
        params->deband_params1 = std::make_unique<pl_deband_params>();
        memcpy(params->deband_params1.get(), params->deband_params.get(), sizeof(pl_deband_params));
        params->deband_params1->grain = grainC;
    }

    params->frame_index = 0;

    AVS_Value v{ avs_new_value_clip(clip) };

    fi->user_data = reinterpret_cast<void*>(params);
    fi->get_frame = deband_get_frame;
    fi->set_cache_hints = deband_set_cache_hints;
    fi->free_filter = free_deband;

    avs_release_clip(clip);

    return v;
}
