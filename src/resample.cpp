#include <mutex>

#include "avs_libplacebo.h"

static std::mutex mtx;

struct resample
{
    std::unique_ptr<priv> vf;
    float src_x;
    float src_y;
    std::unique_ptr<pl_sample_filter_params> sample_params;
    std::unique_ptr<pl_filter_function> filter;
    std::unique_ptr<pl_sigmoid_params> sigmoid_params;
    pl_shader_obj lut;
    pl_color_transfer trc;
    int linear;
    std::string msg;
    int subw;
    int subh;
    float shift_w;
    float shift_h;
    int cplace;
    float src_width;
    float src_height;
};

static int resample_do_plane(priv& p, resample& data, const int w, const int h, const float sx, const float sy, const int planeIdx)
{
    pl_shader sh{ pl_dispatch_begin(p.dp) };
    pl_tex sample_fbo{};
    pl_tex sep_fbo{};

    pl_sample_filter_params sample_params{ *data.sample_params.get() };
    sample_params.lut = &data.lut;

    pl_color_space cs{};
    cs.transfer = data.trc;

    pl_sample_src src{};
    src.tex = p.tex_in[0];

    //
    // linearization and sigmoidization
    //

    pl_shader ish{ pl_dispatch_begin(p.dp) };
    pl_tex_params tp{};
    tp.w = src.tex->params.w;
    tp.h = src.tex->params.h;
    tp.renderable = true;
    tp.sampleable = true;
    tp.format = src.tex->params.format;

    if (!pl_tex_recreate(p.gpu, &sample_fbo, &tp))
        return 1;

    pl_shader_sample_direct(ish, &src);

    if (data.linear)
        pl_shader_linearize(ish, &cs);

    if (data.sigmoid_params.get())
        pl_shader_sigmoidize(ish, data.sigmoid_params.get());

    pl_dispatch_params dp{};
    dp.target = sample_fbo;
    dp.shader = &ish;

    if (!pl_dispatch_finish(p.dp, &dp))
        return 2;

    //
    // sampling
    //

    const float src_w{ [&]()
    {
        if (data.src_width > -1.0f)
            return (planeIdx == AVS_PLANAR_U || planeIdx == AVS_PLANAR_V) ? (data.src_width / data.subw) : data.src_width;
        else
            return static_cast<float>(p.tex_in[0]->params.w);
    }() };

    const float src_h{ [&]()
    {
        if (data.src_height > -1.0f)
            return (planeIdx == AVS_PLANAR_U || planeIdx == AVS_PLANAR_V) ? (data.src_height / data.subh) : data.src_height;
        else
            return static_cast<float>(p.tex_in[0]->params.h);
    }() };

    pl_rect2df rect{ sx, sy, src_w + sx, src_h + sy, };

    src.tex = sample_fbo;
    src.rect = rect;
    src.new_h = h;
    src.new_w = w;

    if (data.sample_params->filter.polar)
    {
        if (!pl_shader_sample_polar(sh, &src, &sample_params))
            return 3;
    }
    else
    {
        pl_shader tsh{ pl_dispatch_begin(p.dp) };

        if (!pl_shader_sample_ortho(tsh, PL_SEP_VERT, &src, &sample_params))
        {
            pl_dispatch_abort(p.dp, &tsh);
            return 4;
        }

        tp.h = src.new_h;

        if (!pl_tex_recreate(p.gpu, &sep_fbo, &tp))
            return 5;

        src.tex = sep_fbo;

        dp.target = sep_fbo;
        dp.shader = &tsh;

        if (!pl_dispatch_finish(p.dp, &dp))
            return 6;

        if (!pl_shader_sample_ortho(sh, PL_SEP_HORIZ, &src, &sample_params))
            return 7;
    }

    if (data.sigmoid_params.get())
        pl_shader_unsigmoidize(sh, data.sigmoid_params.get());

    if (data.linear)
        pl_shader_delinearize(sh, &cs);

    dp.target = p.tex_out[0];
    dp.shader = &sh;

    if (!pl_dispatch_finish(p.dp, &dp))
        return 8;

    pl_tex_destroy(p.gpu, &sep_fbo);
    pl_tex_destroy(p.gpu, &sample_fbo);

    return 0;
}

static int resample_reconfig(priv& p, const pl_plane_data& data, const int w, const int h)
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
        t_r.w = w;
        t_r.h = h;
        t_r.sampleable = false;
        t_r.host_writable = false;
        t_r.renderable = true;
        t_r.host_readable = true;
        t_r.storable = true;

        if (!pl_tex_recreate(p.gpu, &p.tex_out[0], &t_r))
            return -2;
    }
    else
        return -2;

    return 0;
}

static int resample_filter(priv& p, AVS_VideoFrame* dst, const pl_plane_data& src, resample& d, const int w, const int h, const float sx, const float sy, const int planeIdx)
{
    // Upload planes
    pl_tex_transfer_params ttr{};
    ttr.tex = p.tex_in[0];
    ttr.row_pitch = src.row_stride;
    ttr.ptr = const_cast<void*>(src.pixels);

    if (!pl_tex_upload(p.gpu, &ttr))
        return -1;

    // Process plane
    const int proc{ resample_do_plane(p, d, w, h, sx, sy, planeIdx) };
    if (proc)
        return proc;

    ttr.tex = p.tex_out[0];
    ttr.row_pitch = avs_get_pitch_p(dst, planeIdx);
    ttr.ptr = reinterpret_cast<void*>(avs_get_write_ptr_p(dst, planeIdx));

    // Download planes
    if (!pl_tex_download(p.gpu, &ttr))
        return -3;

    return 0;
}

static AVS_VideoFrame* AVSC_CC resample_get_frame(AVS_FilterInfo* fi, int n)
{
    resample* d{ reinterpret_cast<resample*>(fi->user_data) };

    const char* ErrorText{ 0 };
    AVS_VideoFrame* src{ avs_get_frame(fi->child, n) };
    if (!src)
        return nullptr;

    AVS_VideoFrame* dst{ avs_new_video_frame_p(fi->env, &fi->vi, src) };

    constexpr int planes_y[4]{ AVS_PLANAR_Y, AVS_PLANAR_U, AVS_PLANAR_V, AVS_PLANAR_A };
    constexpr int planes_r[4]{ AVS_PLANAR_R, AVS_PLANAR_G, AVS_PLANAR_B, AVS_PLANAR_A };
    const int* planes{ (avs_is_rgb(&fi->vi)) ? planes_r : planes_y };
    const int num_planes{ avs_num_components(&fi->vi) };

    for (int i{ 0 }; i < num_planes; ++i)
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

        const int dst_width{ avs_get_row_size_p(dst, planes[i]) / avs_component_size(&fi->vi) };
        const int dst_height = avs_get_height_p(dst, planes[i]);

        {
            std::lock_guard<std::mutex> lck(mtx);

            const int reconf{ resample_reconfig(*d->vf.get(), plane, dst_width, dst_height) };
            if (reconf == 0)
            {
                const int filt{ resample_filter(*d->vf.get(), dst, plane, *d, dst_width, dst_height,
                    (i > 0) ? (d->shift_w + d->src_x / d->subw) : d->src_x,
                    (i > 0) ? (d->shift_h + d->src_y / d->subh) : d->src_y,
                    planes[i]) };

                if (filt)
                {
                    switch (filt)
                    {
                        case -1: ErrorText = "libplacebo_Resample: failed uploading data to the GPU!"; break;
                        case 1: ErrorText = "libplacebo_Resample: failed creating intermediate color texture!"; break;
                        case 2: ErrorText = "libplacebo_Resample: failed linearizing/sigmoidizing!"; break;
                        case 3: ErrorText = "libplacebo_Resample: failed dispatching scaler..."; break;
                        case 4: ErrorText = "libplacebo_Resample: failed dispatching vertical pass!"; break;
                        case 5: ErrorText = "libplacebo_Resample: failed creating intermediate texture!"; break;
                        case 6: ErrorText = "libplacebo_Resample: failed rendering vertical pass!"; break;
                        case 7: ErrorText = "libplacebo_Resample: failed dispatching horizontal pass!"; break;
                        case 8: ErrorText = "libplacebo_Resample: failed rendering horizontal pass!"; break;
                        default: ErrorText = "libplacebo_Resample: failed downloading data from the GPU!";
                    }
                }
            }
            else
            {
                switch (reconf)
                {
                    case -1: ErrorText = "libplacebo_Resample: failed configuring filter: no good texture format!"; break;
                    default: ErrorText = "libplacebo_Resample: failed creating GPU textures!";
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
        avs_prop_set_int(fi->env, avs_get_frame_props_rw(fi->env, dst), "_ChromaLocation", d->cplace, 0);

        avs_release_video_frame(src);

        return dst;
    }
}

static void AVSC_CC free_resample(AVS_FilterInfo* fi)
{
    resample* d{ reinterpret_cast<resample*>(fi->user_data) };

    pl_shader_obj_destroy(&d->lut);
    avs_libplacebo_uninit(std::move(d->vf));
    delete d;
}

static int AVSC_CC resample_set_cache_hints(AVS_FilterInfo* fi, int cachehints, int frame_range)
{
    return cachehints == AVS_CACHE_GET_MTMODE ? 2 : 0;
}

AVS_Value AVSC_CC create_resample(AVS_ScriptEnvironment* env, AVS_Value args, void* param)
{
    enum { Clip, Width, Height, Filter, Radius, Clamp, Taper, Blur, Param1, Param2, Sx, Sy, Antiring, Lut_entries, Cutoff, Sigmoidize, Linearize, Sigmoid_center, Sigmoid_slope, Trc, Cplace, Device, List_device, Src_width, Src_height };

    AVS_FilterInfo* fi;
    AVS_Clip* clip{ avs_new_c_filter(env, &fi, avs_array_elt(args, Clip), 1) };

    resample* params{ new resample() };

    if (!avs_is_planar(&fi->vi))
        return set_error(clip, "libplacebo_Resample: clip must be in planar format.");
    if (avs_bits_per_component(&fi->vi) != 8 && avs_bits_per_component(&fi->vi) != 16 && avs_bits_per_component(&fi->vi) != 32)
        return set_error(clip, "libplacebo_Resample: bit depth must be 8, 16 or 32-bit.");

    const int w{ fi->vi.width };
    const int h{ fi->vi.height };

    const int device{ avs_defined(avs_array_elt(args, Device)) ? avs_as_int(avs_array_elt(args, Device)) : -1 };
    const int list_device{ avs_defined(avs_array_elt(args, List_device)) ? avs_as_bool(avs_array_elt(args, List_device)) : 0 };

    std::vector<VkPhysicalDevice> devices{};
    VkInstance inst{};

    if (list_device || device > -1)
    {
        AVS_Value dev_info{ devices_info(clip, fi->env, devices, inst, params->msg, std::string("libplacebo_Resample"), device, list_device) };
        if (avs_is_error(dev_info) || avs_is_clip(dev_info))
            return dev_info;
    }
    else if (device < -1)
    {
        vkDestroyInstance(inst, nullptr);
        return set_error(clip, "libplacebo_Resample: device must be greater than or equal to -1.");
    }

    if (device == -1)
    {
        devices.resize(1);
        params->vf = avs_libplacebo_init(devices[0]);
    }
    else
        params->vf = avs_libplacebo_init(devices[device]);

    vkDestroyInstance(inst, nullptr);

    fi->vi.width = avs_as_int(avs_array_elt(args, Width));
    fi->vi.height = avs_as_int(avs_array_elt(args, Height));

    params->src_x = (avs_defined(avs_array_elt(args, Sx))) ? avs_as_float(avs_array_elt(args, Sx)) : 0.0f;
    params->src_y = (avs_defined(avs_array_elt(args, Sy))) ? avs_as_float(avs_array_elt(args, Sy)) : 0.0f;

    params->trc = static_cast<pl_color_transfer>((avs_defined(avs_array_elt(args, Trc))) ? avs_as_int(avs_array_elt(args, Trc)) : 1);

    if (avs_is_rgb(&fi->vi))
    {
        params->linear = (avs_defined(avs_array_elt(args, Linearize))) ? avs_as_bool(avs_array_elt(args, Linearize)) : 1;

        if (params->linear)
        {
            const int sigmoid{ (avs_defined(avs_array_elt(args, Sigmoidize))) ? avs_as_bool(avs_array_elt(args, Sigmoidize)) : 1 };

            if (sigmoid)
            {
                params->sigmoid_params = std::make_unique<pl_sigmoid_params>();

                params->sigmoid_params->center = (avs_defined(avs_array_elt(args, Sigmoid_center))) ? avs_as_float(avs_array_elt(args, Sigmoid_center)) : 0.75f;
                if (params->sigmoid_params->center < 0.0f || params->sigmoid_params->center > 1.0f)
                    return set_error(clip, "libplacebo_Resample: sigmoid_center must be between 0.0 and 1.0.");

                params->sigmoid_params->slope = (avs_defined(avs_array_elt(args, Sigmoid_slope))) ? avs_as_float(avs_array_elt(args, Sigmoid_slope)) : 6.5f;
                if (params->sigmoid_params->slope < 1.0f || params->sigmoid_params->slope > 20.0f)
                    return set_error(clip, "libplacebo_Resample: sigmoid_slope must be between 1.0 and 20.0.");
            }
        }
    }
    else
        params->linear = 0;

    params->sample_params = std::make_unique<pl_sample_filter_params>();

    params->lut = nullptr;
    params->sample_params->no_widening = false;
    params->sample_params->no_compute = false;
    params->sample_params->lut_entries = (avs_defined(avs_array_elt(args, Lut_entries))) ? avs_as_int(avs_array_elt(args, Lut_entries)) : 0;
    params->sample_params->cutoff = (avs_defined(avs_array_elt(args, Cutoff))) ? avs_as_float(avs_array_elt(args, Cutoff)) : 0.0f;
    params->sample_params->antiring = (avs_defined(avs_array_elt(args, Antiring))) ? avs_as_float(avs_array_elt(args, Antiring)) : 0.0f;

    const pl_filter_preset* fil{ pl_find_filter_preset((avs_defined(avs_array_elt(args, Filter))) ? avs_as_string(avs_array_elt(args, Filter)) : "ewa_lanczos") };
    if (!fil)
        return set_error(clip, "libplacebo_Resample: not a valid filter.");

    params->sample_params->filter = *fil->filter;

    params->sample_params->filter.clamp = (avs_defined(avs_array_elt(args, Clamp))) ? avs_as_float(avs_array_elt(args, Clamp)) : 0.0f;
    if (params->sample_params->filter.clamp < 0.0f || params->sample_params->filter.clamp > 1.0f)
        return set_error(clip, "libplacebo_Resample: clamp must be between 0.0 and 1.0.");

    params->sample_params->filter.blur = (avs_defined(avs_array_elt(args, Blur))) ? avs_as_float(avs_array_elt(args, Blur)) : 0.0f;
    params->sample_params->filter.taper = (avs_defined(avs_array_elt(args, Taper))) ? avs_as_float(avs_array_elt(args, Taper)) : 0.0f;

    params->filter = std::make_unique<pl_filter_function>();
    *params->filter.get() = *params->sample_params->filter.kernel;

    if (params->filter->resizable)
    {
        if (avs_defined(avs_array_elt(args, Radius)))
            params->filter->radius = avs_as_float(avs_array_elt(args, Radius));
    }

    if (avs_defined(avs_array_elt(args, Param1)) && params->filter->tunable[0])
        params->filter->params[0] = avs_as_float(avs_array_elt(args, Param1));
    if (avs_defined(avs_array_elt(args, Param2)) && params->filter->tunable[1])
        params->filter->params[1] = avs_as_float(avs_array_elt(args, Param2));

    params->sample_params->filter.kernel = params->filter.get();

    if (avs_is_420(&fi->vi) || avs_is_422(&fi->vi))
    {
        params->cplace = (avs_defined(avs_array_elt(args, Cplace))) ? avs_as_int(avs_array_elt(args, Cplace)) : 0;
        if (params->cplace < 0 || params->cplace > 2)
            return set_error(clip, "libplacebo_Resample: cplace must be between 0 and 2.");

        params->subw = (1 << avs_get_plane_width_subsampling(&fi->vi, AVS_PLANAR_U));
        params->subh = (1 << avs_get_plane_height_subsampling(&fi->vi, AVS_PLANAR_U));

        params->shift_w = (params->cplace == 0 || params->cplace == 2) ? (0.5f * (1.0f - static_cast<float>(w) / fi->vi.width)) / params->subw : 0.0f;
        params->shift_h = (params->cplace == 2) ? (0.5f * (1.0f - static_cast<float>(h) / fi->vi.height)) / params->subh : 0.0f;
    }
    else
    {
        params->subw = 1;
        params->subh = 1;
        params->shift_w = 0.0f;
        params->shift_h = 0.0f;
    }

    if (avs_defined(avs_array_elt(args, Src_width)))
    {
        params->src_width = avs_as_float(avs_array_elt(args, Src_width));
        if (params->src_width <= 0.0f)
            return set_error(clip, "libplacebo_Resample: src_width must be greater than 0.0.");
    }
    else
        params->src_width = -1.0f;

    if (avs_defined(avs_array_elt(args, Src_height)))
    {
        params->src_height = avs_as_float(avs_array_elt(args, Src_height));
        if (params->src_height <= 0.0f)
            return set_error(clip, "libplacebo_Resample: src_height must be greater than 0.0.");
    }
    else
        params->src_height = -1.0f;

    AVS_Value v{ avs_new_value_clip(clip) };

    fi->user_data = reinterpret_cast<void*>(params);
    fi->get_frame = resample_get_frame;
    fi->set_cache_hints = resample_set_cache_hints;
    fi->free_filter = free_resample;

    avs_release_clip(clip);

    return v;
}
