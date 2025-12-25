#include <mutex>

#include "avs_libplacebo.h"

struct resample
{
    std::mutex mtx;
    std::unique_ptr<priv> vf;
    float src_x;
    float src_y;
    std::unique_ptr<pl_sample_filter_params> sample_params;
    std::unique_ptr<pl_sigmoid_params> sigmoid_params;
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

    int (*resample_process)(AVS_VideoFrame* dst, AVS_VideoFrame* src, resample* d, const AVS_FilterInfo* fi) noexcept;
};

static int resample_do_plane(
    const resample* d, const int w, const int h, const float sx, const float sy, const int planeIdx) noexcept
{
    pl_shader sh{pl_dispatch_begin(d->vf->dp)};

    pl_sample_filter_params* sample_params{d->sample_params.get()};
    sample_params->lut = &d->vf->lut;

    pl_color_space cs{};
    cs.transfer = d->trc;

    pl_sample_src src{};
    src.tex = d->vf->tex_in[0];

    //
    // linearization and sigmoidization
    //

    pl_shader ish{pl_dispatch_begin(d->vf->dp)};
    pl_tex_params tp{};
    tp.w = src.tex->params.w;
    tp.h = src.tex->params.h;
    tp.renderable = true;
    tp.sampleable = true;
    tp.format = src.tex->params.format;

    if (!pl_tex_recreate(d->vf->gpu, &d->vf->sample_fbo, &tp))
        return -1;

    pl_shader_sample_direct(ish, &src);

    if (d->linear)
        pl_shader_linearize(ish, &cs);

    if (d->sigmoid_params.get())
        pl_shader_sigmoidize(ish, d->sigmoid_params.get());

    pl_dispatch_params dp{};
    dp.target = d->vf->sample_fbo;
    dp.shader = &ish;

    if (!pl_dispatch_finish(d->vf->dp, &dp))
        return -1;

    //
    // sampling
    //

    const float src_w{[&]() {
        if (d->src_width > -1.0f)
            return (planeIdx == AVS_PLANAR_U || planeIdx == AVS_PLANAR_V) ? (d->src_width / d->subw) : d->src_width;
        else
            return static_cast<float>(d->vf->tex_in[0]->params.w);
    }()};

    const float src_h{[&]() {
        if (d->src_height > -1.0f)
            return (planeIdx == AVS_PLANAR_U || planeIdx == AVS_PLANAR_V) ? (d->src_height / d->subh) : d->src_height;
        else
            return static_cast<float>(d->vf->tex_in[0]->params.h);
    }()};

    pl_rect2df rect{
        sx,
        sy,
        src_w + sx,
        src_h + sy,
    };

    src.tex = d->vf->sample_fbo;
    src.rect = rect;
    src.new_h = h;
    src.new_w = w;

    if (d->sample_params->filter.polar)
    {
        if (!pl_shader_sample_polar(sh, &src, sample_params))
            return -1;
    }
    else
    {
        pl_sample_src src1 = src;
        src.new_w = src.tex->params.w;
        src.rect.x0 = 0;
        src.rect.x1 = src.new_w;
        src1.rect.y0 = 0;
        src1.rect.y1 = src.new_h;

        pl_shader tsh{pl_dispatch_begin(d->vf->dp)};

        if (!pl_shader_sample_ortho2(tsh, &src, sample_params))
        {
            pl_dispatch_abort(d->vf->dp, &tsh);
            return -1;
        }

        tp.w = src.new_w;
        tp.h = src.new_h;

        if (!pl_tex_recreate(d->vf->gpu, &d->vf->sep_fbo, &tp))
            return -1;

        dp.target = d->vf->sep_fbo;
        dp.shader = &tsh;

        if (!pl_dispatch_finish(d->vf->dp, &dp))
            return -1;

        src1.tex = d->vf->sep_fbo;
        src1.scale = 1.0f;

        if (!pl_shader_sample_ortho2(sh, &src1, sample_params))
            return -1;
    }

    if (d->sigmoid_params.get())
        pl_shader_unsigmoidize(sh, d->sigmoid_params.get());

    if (d->linear)
        pl_shader_delinearize(sh, &cs);

    dp.target = d->vf->tex_out[0];
    dp.shader = &sh;

    if (!pl_dispatch_finish(d->vf->dp, &dp))
        return -1;

    return 0;
}

template<typename T>
static int resample_filter(AVS_VideoFrame* dst, AVS_VideoFrame* src, resample* d, const AVS_FilterInfo* fi) noexcept
{
    const pl_fmt fmt{[&]() {
        if constexpr (std::is_same_v<T, uint8_t>)
            return pl_find_named_fmt(d->vf->gpu, "r8");
        else if constexpr (std::is_same_v<T, uint16_t>)
            return pl_find_named_fmt(d->vf->gpu, "r16");
        else
            return pl_find_named_fmt(d->vf->gpu, "r32f");
    }()};
    if (!fmt)
        return -1;

    constexpr int planes_y[4]{AVS_PLANAR_Y, AVS_PLANAR_U, AVS_PLANAR_V, AVS_PLANAR_A};
    constexpr int planes_r[4]{AVS_PLANAR_R, AVS_PLANAR_G, AVS_PLANAR_B, AVS_PLANAR_A};
    const int* planes{(avs_is_rgb(&fi->vi)) ? planes_r : planes_y};
    const int num_planes{g_avs_api->avs_num_components(&fi->vi)};

    std::lock_guard<std::mutex> lck(d->mtx);

    for (int i{0}; i < num_planes; ++i)
    {
        const int plane{planes[i]};

        const size_t dst_width{g_avs_api->avs_get_row_size_p(dst, plane) / sizeof(T)};
        const int dst_height{g_avs_api->avs_get_height_p(dst, plane)};

        pl_plane_data pl{};
        pl.pixel_stride = sizeof(T);
        if constexpr (std::is_same_v<T, uint8_t>)
        {
            pl.type = PL_FMT_UNORM;
            pl.component_size[0] = 8;
        }
        else if constexpr (std::is_same_v<T, uint16_t>)
        {
            pl.type = PL_FMT_UNORM;
            pl.component_size[0] = 16;
        }
        else
        {
            pl.type = PL_FMT_FLOAT;
            pl.component_size[0] = 32;
        }
        pl.width = g_avs_api->avs_get_row_size_p(src, plane) / sizeof(T);
        pl.height = g_avs_api->avs_get_height_p(src, plane);
        pl.row_stride = g_avs_api->avs_get_pitch_p(src, plane);
        pl.pixels = g_avs_api->avs_get_read_ptr_p(src, plane);

        // Upload planes
        if (!pl_upload_plane(d->vf->gpu, nullptr, &d->vf->tex_in[0], &pl))
            return -1;

        pl_tex_params t_r{};
        t_r.format = fmt;
        t_r.w = dst_width;
        t_r.h = dst_height;
        t_r.sampleable = false;
        t_r.host_writable = false;
        t_r.renderable = true;
        t_r.host_readable = true;
        t_r.storable = true;

        if (!pl_tex_recreate(d->vf->gpu, &d->vf->tex_out[0], &t_r))
            return -1;

        // Process plane
        if (resample_do_plane(d, dst_width, dst_height, (i > 0) ? (d->shift_w + d->src_x / d->subw) : d->src_x,
                (i > 0) ? (d->shift_h + d->src_y / d->subh) : d->src_y, plane))
            return -1;

        pl_tex_transfer_params ttr{};
        ttr.tex = d->vf->tex_out[0];
        ttr.row_pitch = g_avs_api->avs_get_pitch_p(dst, plane);
        ttr.ptr = g_avs_api->avs_get_write_ptr_p(dst, plane);

        // Download planes
        if (!pl_tex_download(d->vf->gpu, &ttr))
            return -1;
    }

    return 0;
}

static AVS_VideoFrame* AVSC_CC resample_get_frame(AVS_FilterInfo* fi, int n)
{
    resample* d{reinterpret_cast<resample*>(fi->user_data)};

    avs_helpers::avs_video_frame_ptr src_ptr{g_avs_api->avs_get_frame(fi->child, n)};
    AVS_VideoFrame* src{ src_ptr.get() };
    if (!src)
        return nullptr;

    avs_helpers::avs_video_frame_ptr dst_ptr{g_avs_api->avs_new_video_frame_p(fi->env, &fi->vi, src)};
    AVS_VideoFrame* dst{dst_ptr.get()};

    if (d->resample_process(dst, src, d, fi))
    {
        d->msg = "libplacebo_Resample: " + d->vf->log_buffer.str();

        fi->error = d->msg.c_str();

        return nullptr;
    }
    else
    {
        g_avs_api->avs_prop_set_int(fi->env, g_avs_api->avs_get_frame_props_rw(fi->env, dst), "_ChromaLocation", d->cplace, 0);

        return dst_ptr.release();
    }
}

static void AVSC_CC free_resample(AVS_FilterInfo* fi)
{
    resample* d{reinterpret_cast<resample*>(fi->user_data)};

    avs_libplacebo_uninit(d->vf);
    delete d;
}

static int AVSC_CC resample_set_cache_hints(AVS_FilterInfo* fi, int cachehints, int frame_range)
{
    return cachehints == AVS_CACHE_GET_MTMODE ? 2 : 0;
}

AVS_Value AVSC_CC create_resample(AVS_ScriptEnvironment* env, AVS_Value args, void* param)
{
    enum
    {
        Clip,
        Width,
        Height,
        Filter,
        Radius,
        Clamp,
        Taper,
        Blur,
        Param1,
        Param2,
        Sx,
        Sy,
        Antiring,
        Sigmoidize,
        Linearize,
        Sigmoid_center,
        Sigmoid_slope,
        Trc,
        Cplace,
        Device,
        List_device,
        Src_width,
        Src_height
    };

    AVS_FilterInfo* fi;
    avs_helpers::avs_clip_ptr clip_ptr{g_avs_api->avs_new_c_filter(env, &fi, avs_array_elt(args, Clip), 1)};
    AVS_Clip* clip{clip_ptr.get()};

    std::unique_ptr<resample> params{std::make_unique<resample>()};

    AVS_Value avs_ver{avs_version(params->msg, "libplacebo_Resample", env)};
    if (avs_is_error(avs_ver))
        return avs_ver;

    const int bits{g_avs_api->avs_bits_per_component(&fi->vi)};

    if (!avs_is_planar(&fi->vi))
        return set_error("libplacebo_Resample: clip must be in planar format.", nullptr);
    if (bits != 8 && bits != 16 && bits != 32)
        return set_error("libplacebo_Resample: bit depth must be 8, 16 or 32-bit.", nullptr);

    const int w{fi->vi.width};
    const int h{fi->vi.height};

    const int device{avs_defined(avs_array_elt(args, Device)) ? avs_as_int(avs_array_elt(args, Device)) : -1};
    const int list_device{avs_defined(avs_array_elt(args, List_device)) ? avs_as_bool(avs_array_elt(args, List_device)) : 0};

    if (list_device || device > -1)
    {
        std::vector<VkPhysicalDevice> devices{};
        VkInstance inst{};

        AVS_Value dev_info{devices_info(clip, fi->env, devices, inst, params->msg, "libplacebo_Resample", device, list_device)};
        if (avs_is_error(dev_info) || avs_is_clip(dev_info))
        {
            fi->user_data = params.release();
            fi->free_filter = free_resample;

            return dev_info;
        }

        params->vf = avs_libplacebo_init(devices[device], params->msg);

        vkDestroyInstance(inst, nullptr);
    }
    else
    {
        if (device < -1)
            return set_error("libplacebo_Resample: device must be greater than or equal to -1.", nullptr);

        params->vf = avs_libplacebo_init(nullptr, params->msg);
    }

    if (params->msg.size())
    {
        params->msg = "libplacebo_Resample: " + params->msg;
        return set_error(params->msg.c_str(), nullptr);
    }

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
            const int sigmoid{(avs_defined(avs_array_elt(args, Sigmoidize))) ? avs_as_bool(avs_array_elt(args, Sigmoidize)) : 1};

            if (sigmoid)
            {
                params->sigmoid_params = std::make_unique<pl_sigmoid_params>();

                params->sigmoid_params->center =
                    (avs_defined(avs_array_elt(args, Sigmoid_center))) ? avs_as_float(avs_array_elt(args, Sigmoid_center)) : 0.75f;
                if (params->sigmoid_params->center < 0.0f || params->sigmoid_params->center > 1.0f)
                    return set_error("libplacebo_Resample: sigmoid_center must be between 0.0 and 1.0.", params->vf);

                params->sigmoid_params->slope =
                    (avs_defined(avs_array_elt(args, Sigmoid_slope))) ? avs_as_float(avs_array_elt(args, Sigmoid_slope)) : 6.5f;
                if (params->sigmoid_params->slope < 1.0f || params->sigmoid_params->slope > 20.0f)
                    return set_error("libplacebo_Resample: sigmoid_slope must be between 1.0 and 20.0.", params->vf);
            }
        }
    }
    else
        params->linear = 0;

    params->sample_params = std::make_unique<pl_sample_filter_params>();
    params->sample_params->no_widening = false;
    params->sample_params->no_compute = false;
    params->sample_params->antiring = (avs_defined(avs_array_elt(args, Antiring))) ? avs_as_float(avs_array_elt(args, Antiring)) : 0.0f;
    if (params->sample_params->antiring < 0.0f || params->sample_params->antiring > 1.0f)
        return set_error("libplacebo_Resample: antiring must be between 0.0 and 1.0.", params->vf);

    const pl_filter_config* filter_config{pl_find_filter_config(
        (avs_defined(avs_array_elt(args, Filter))) ? avs_as_string(avs_array_elt(args, Filter)) : "ewa_lanczos", PL_FILTER_UPSCALING)};
    if (!filter_config)
        return set_error("libplacebo_Resample: not a valid filter.", params->vf);

    params->sample_params->filter = *filter_config;
    params->sample_params->filter.clamp = (avs_defined(avs_array_elt(args, Clamp))) ? avs_as_float(avs_array_elt(args, Clamp)) : 0.0f;
    if (params->sample_params->filter.clamp < 0.0f || params->sample_params->filter.clamp > 1.0f)
        return set_error("libplacebo_Resample: clamp must be between 0.0 and 1.0.", params->vf);

    params->sample_params->filter.blur = (avs_defined(avs_array_elt(args, Blur))) ? avs_as_float(avs_array_elt(args, Blur)) : 0.0f;
    if (params->sample_params->filter.blur < 0.0f || params->sample_params->filter.blur > 100.0f)
        return set_error("libplacebo_Resample: blur must be between 0.0 and 100.0.", params->vf);

    params->sample_params->filter.taper = (avs_defined(avs_array_elt(args, Taper))) ? avs_as_float(avs_array_elt(args, Taper)) : 0.0f;
    if (params->sample_params->filter.taper < 0.0f || params->sample_params->filter.taper > 1.0f)
        return set_error("libplacebo_Resample: taper must be between 0.0 and 1.0.", params->vf);

    if (avs_defined(avs_array_elt(args, Radius)))
    {
        params->sample_params->filter.radius = avs_as_float(avs_array_elt(args, Radius));
        if (params->sample_params->filter.radius < 0.0f || params->sample_params->filter.radius > 16.0f)
            return set_error("libplacebo_Resample: radius must be between 0.0 and 16.0.", params->vf);
    }

    if (avs_defined(avs_array_elt(args, Param1)))
        params->sample_params->filter.params[0] = avs_as_float(avs_array_elt(args, Param1));
    if (avs_defined(avs_array_elt(args, Param2)))
        params->sample_params->filter.params[1] = avs_as_float(avs_array_elt(args, Param2));

    if (g_avs_api->avs_is_420(&fi->vi) || g_avs_api->avs_is_422(&fi->vi))
    {
        params->cplace = (avs_defined(avs_array_elt(args, Cplace))) ? avs_as_int(avs_array_elt(args, Cplace)) : 0;
        if (params->cplace < 0 || params->cplace > 2)
            return set_error("libplacebo_Resample: cplace must be between 0 and 2.", params->vf);

        params->subw = (1 << g_avs_api->avs_get_plane_width_subsampling(&fi->vi, AVS_PLANAR_U));
        params->subh = (1 << g_avs_api->avs_get_plane_height_subsampling(&fi->vi, AVS_PLANAR_U));

        params->shift_w =
            (params->cplace == 0 || params->cplace == 2) ? (0.5f * (1.0f - static_cast<float>(w) / fi->vi.width)) / params->subw : 0.0f;
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
            return set_error("libplacebo_Resample: src_width must be greater than 0.0.", params->vf);
    }
    else
        params->src_width = -1.0f;

    if (avs_defined(avs_array_elt(args, Src_height)))
    {
        params->src_height = avs_as_float(avs_array_elt(args, Src_height));
        if (params->src_height <= 0.0f)
            return set_error("libplacebo_Resample: src_height must be greater than 0.0.", params->vf);
    }
    else
        params->src_height = -1.0f;

    switch (bits)
    {
    case 8:
        params->resample_process = resample_filter<uint8_t>;
        break;
    case 16:
        params->resample_process = resample_filter<uint16_t>;
        break;
    default:
        params->resample_process = resample_filter<float>;
        break;
    }

    AVS_Value v;
    g_avs_api->avs_set_to_clip(&v, clip);

    fi->user_data = params.release();
    fi->get_frame = resample_get_frame;
    fi->set_cache_hints = resample_set_cache_hints;
    fi->free_filter = free_resample;

    return v;
}
