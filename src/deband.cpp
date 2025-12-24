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

    int (*deband_process)(AVS_VideoFrame* dst, AVS_VideoFrame* src, deband* d, const AVS_FilterInfo* vi) noexcept;
};

static bool deband_do_plane(deband* d, const int planeIdx) noexcept
{
    pl_shader sh{pl_dispatch_begin(d->vf->dp)};

    pl_shader_params sh_p{};
    sh_p.gpu = d->vf->gpu;
    sh_p.index = d->frame_index++;

    pl_shader_reset(sh, &sh_p);

    pl_sample_src src{};
    src.tex = d->vf->tex_in[0];

    pl_shader_deband(sh, &src,
        ((planeIdx == AVS_PLANAR_U || planeIdx == AVS_PLANAR_V) && d->deband_params1) ? d->deband_params1.get() : d->deband_params.get());

    if (d->dither)
        pl_shader_dither(sh, d->vf->tex_out[0]->params.format->component_depth[0], &d->vf->dither_state, d->dither_params.get());

    pl_dispatch_params d_p{};
    d_p.target = d->vf->tex_out[0];
    d_p.shader = &sh;

    return pl_dispatch_finish(d->vf->dp, &d_p);
}

template<typename T>
static int deband_filter(AVS_VideoFrame* dst, AVS_VideoFrame* src, deband* d, const AVS_FilterInfo* fi) noexcept
{
    const int error{[&]() {
        pl_shader_obj_destroy(&d->vf->dither_state);
        pl_tex_destroy(d->vf->gpu, &d->vf->tex_out[0]);
        pl_tex_destroy(d->vf->gpu, &d->vf->tex_in[0]);

        return -1;
    }()};

    const pl_fmt fmt{[&]() {
        if constexpr (std::is_same_v<T, uint8_t>)
            return pl_find_named_fmt(d->vf->gpu, "r8");
        else if constexpr (std::is_same_v<T, uint16_t>)
            return pl_find_named_fmt(d->vf->gpu, "r16");
        else
            return pl_find_named_fmt(d->vf->gpu, "r32f");
    }()};
    if (!fmt)
        return error;

    constexpr int planes_y[3]{AVS_PLANAR_Y, AVS_PLANAR_U, AVS_PLANAR_V};
    constexpr int planes_r[3]{AVS_PLANAR_R, AVS_PLANAR_G, AVS_PLANAR_B};
    const int* planes{(avs_is_rgb(&fi->vi)) ? planes_r : planes_y};
    const int num_planes{std::min(g_avs_api->avs_num_components(&fi->vi), 3)};

    for (int i{0}; i < num_planes; ++i)
    {
        const int plane{planes[i]};

        if (d->process[i] == 2)
            g_avs_api->avs_bit_blt(fi->env, g_avs_api->avs_get_write_ptr_p(dst, plane), g_avs_api->avs_get_pitch_p(dst, plane),
                g_avs_api->avs_get_read_ptr_p(src, plane), g_avs_api->avs_get_pitch_p(src, plane),
                g_avs_api->avs_get_row_size_p(src, plane), g_avs_api->avs_get_height_p(src, plane));
        else if (d->process[i] == 3)
        {
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

            std::lock_guard<std::mutex> lck(mtx);

            // Upload planes
            if (!pl_upload_plane(d->vf->gpu, nullptr, &d->vf->tex_in[0], &pl))
                return error;

            pl_tex_params t_r{};
            t_r.format = fmt;
            t_r.w = pl.width;
            t_r.h = pl.height;
            t_r.sampleable = false;
            t_r.host_writable = false;
            t_r.renderable = true;
            t_r.host_readable = true;

            if (!pl_tex_recreate(d->vf->gpu, &d->vf->tex_out[0], &t_r))
                return error;

            // Process plane
            if (!deband_do_plane(d, plane))
                return error;

            const size_t dst_stride{(g_avs_api->avs_get_pitch_p(dst, plane) + (d->vf->gpu->limits.align_tex_xfer_pitch) - 1) &
                                    ~((d->vf->gpu->limits.align_tex_xfer_pitch) - 1)};
            pl_buf_params buf_params{};
            buf_params.size = dst_stride * t_r.h;
            buf_params.host_mapped = true;

            pl_buf dst_buf{};
            if (!pl_buf_recreate(d->vf->gpu, &dst_buf, &buf_params))
                return error;

            pl_tex_transfer_params ttr{};
            ttr.tex = d->vf->tex_out[0];
            ttr.row_pitch = dst_stride;
            ttr.buf = dst_buf;

            // Download planes
            if (!pl_tex_download(d->vf->gpu, &ttr))
            {
                pl_buf_destroy(d->vf->gpu, &dst_buf);
                return error;
            }

            pl_shader_obj_destroy(&d->vf->dither_state);
            pl_tex_destroy(d->vf->gpu, &d->vf->tex_out[0]);
            pl_tex_destroy(d->vf->gpu, &d->vf->tex_in[0]);

            while (pl_buf_poll(d->vf->gpu, dst_buf, 0))
                ;
            memcpy(g_avs_api->avs_get_write_ptr_p(dst, plane), dst_buf->data, dst_buf->params.size);
            pl_buf_destroy(d->vf->gpu, &dst_buf);
        }
    }

    return 0;
}

static AVS_VideoFrame* AVSC_CC deband_get_frame(AVS_FilterInfo* fi, int n)
{
    deband* d{reinterpret_cast<deband*>(fi->user_data)};

    avs_helpers::avs_video_frame_ptr src_ptr{ g_avs_api->avs_get_frame(fi->child, n) };
    AVS_VideoFrame* src{ src_ptr.get() };
    if (!src)
        return nullptr;

    avs_helpers::avs_video_frame_ptr dst_ptr{g_avs_api->avs_new_video_frame_p(fi->env, &fi->vi, src)};
    AVS_VideoFrame* dst{dst_ptr.get()};

    if (d->deband_process(dst, src, d, fi))
    {
        d->msg = "libplacebo_Deband: " + d->vf->log_buffer.str();

        fi->error = d->msg.c_str();

        return nullptr;
    }
    else
    {
        if (g_avs_api->avs_num_components(&fi->vi) > 3)
            g_avs_api->avs_bit_blt(fi->env, g_avs_api->avs_get_write_ptr_p(dst, AVS_PLANAR_A),
                g_avs_api->avs_get_pitch_p(dst, AVS_PLANAR_A), g_avs_api->avs_get_read_ptr_p(src, AVS_PLANAR_A),
                g_avs_api->avs_get_pitch_p(src, AVS_PLANAR_A), g_avs_api->avs_get_row_size_p(src, AVS_PLANAR_A),
                g_avs_api->avs_get_height_p(src, AVS_PLANAR_A));

        return dst_ptr.release();
    }
}

static void AVSC_CC free_deband(AVS_FilterInfo* fi)
{
    deband* d{reinterpret_cast<deband*>(fi->user_data)};

    avs_libplacebo_uninit(d->vf);
    delete d;
}

static int AVSC_CC deband_set_cache_hints(AVS_FilterInfo* fi, int cachehints, int frame_range)
{
    return cachehints == AVS_CACHE_GET_MTMODE ? 2 : 0;
}

AVS_Value AVSC_CC create_deband(AVS_ScriptEnvironment* env, AVS_Value args, void* param)
{
    enum
    {
        Clip,
        Iterations,
        Threshold,
        Radius,
        Grainy,
        Grainc,
        Dither,
        Lut_size,
        Temporal,
        Planes,
        Device,
        List_device,
        Grain_neutral
    };

    AVS_FilterInfo* fi;
    avs_helpers::avs_clip_ptr clip_ptr{g_avs_api->avs_new_c_filter(env, &fi, avs_array_elt(args, Clip), 1)};
    AVS_Clip* clip{clip_ptr.get()};

    std::unique_ptr<deband> params{std::make_unique<deband>()};

    AVS_Value avs_ver{avs_version(params->msg, "libplacebo_Deband", env)};
    if (avs_is_error(avs_ver))
        return avs_ver;

    const int bits{g_avs_api->avs_bits_per_component(&fi->vi)};

    if (!avs_is_planar(&fi->vi))
        return set_error("libplacebo_Deband: clip must be in planar format.", nullptr);
    if (bits != 8 && bits != 16 && bits != 32)
        return set_error("libplacebo_Deband: bit depth must be 8, 16 or 32-bit.", nullptr);

    const int device{avs_defined(avs_array_elt(args, Device)) ? avs_as_int(avs_array_elt(args, Device)) : -1};
    const int list_device{avs_defined(avs_array_elt(args, List_device)) ? avs_as_bool(avs_array_elt(args, List_device)) : 0};

    if (list_device || device > -1)
    {
        std::vector<VkPhysicalDevice> devices{};
        VkInstance inst{};

        AVS_Value dev_info{devices_info(clip, fi->env, devices, inst, params->msg, "libplacebo_Deband", device, list_device)};
        if (avs_is_error(dev_info) || avs_is_clip(dev_info))
        {
            fi->user_data = params.release();
            fi->free_filter = free_deband;

            return dev_info;
        }

        params->vf = avs_libplacebo_init(devices[device], params->msg);

        vkDestroyInstance(inst, nullptr);
    }
    else
    {
        if (device < -1)
            return set_error("libplacebo_Deband: device must be greater than or equal to -1.", nullptr);

        params->vf = avs_libplacebo_init(nullptr, params->msg);
    }

    if (params->msg.size())
    {
        params->msg = "libplacebo_Deband: " + params->msg;
        return set_error(params->msg.c_str(), nullptr);
    }

    if (bits == 8)
    {
        params->dither = avs_defined(avs_array_elt(args, Dither)) ? avs_as_bool(avs_array_elt(args, Dither)) : 1;

        if (params->dither)
        {
            params->dither_params = std::make_unique<pl_dither_params>();
            params->dither_params->method = static_cast<pl_dither_method>(params->dither - 1);
            if (params->dither_params->method < 0 || params->dither_params->method > 4)
                return set_error("libplacebo_Deband: dither must be between 0..4", params->vf);

            params->dither_params->lut_size = (avs_defined(avs_array_elt(args, Lut_size))) ? avs_as_int(avs_array_elt(args, Lut_size)) : 6;
            if (params->dither_params->lut_size > 8)
                return set_error("libplacebo_Deband: lut_size must be less than or equal to 8", params->vf);

            params->dither_params->temporal =
                (avs_defined(avs_array_elt(args, Temporal))) ? avs_as_bool(avs_array_elt(args, Temporal)) : false;
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

        const int num_planes{(avs_defined(avs_array_elt(args, Planes))) ? avs_array_size(avs_array_elt(args, Planes)) : 0};
        if (num_planes > g_avs_api->avs_num_components(&fi->vi))
            return set_error("libplacebo_Deband: plane index out of range.", params->vf);

        for (int i{0}; i < num_planes; ++i)
        {
            const int plane_v{avs_as_int(*(avs_as_array(avs_array_elt(args, Planes)) + i))};
            if (plane_v < 1 || plane_v > 3)
                return set_error("libplacebo_Deband: plane must be between 1..3.", params->vf);

            params->process[i] = avs_as_int(*(avs_as_array(avs_array_elt(args, Planes)) + i));
        }
    }

    params->deband_params = std::make_unique<pl_deband_params>();
    params->deband_params->iterations = (avs_defined(avs_array_elt(args, Iterations))) ? avs_as_int(avs_array_elt(args, Iterations)) : 1;
    if (params->deband_params->iterations < 0)
        return set_error("libplacebo_Deband: iterations must be greater than or equal to 0.", params->vf);

    params->deband_params->threshold = (avs_defined(avs_array_elt(args, Threshold))) ? avs_as_float(avs_array_elt(args, Threshold)) : 4.0f;
    if (params->deband_params->threshold < 0.0f)
        return set_error("libplacebo_Deband: threshold must be greater than or equal to 0.0", params->vf);

    params->deband_params->radius = (avs_defined(avs_array_elt(args, Radius))) ? avs_as_float(avs_array_elt(args, Radius)) : 16.0f;
    if (params->deband_params->radius < 0.0f)
        return set_error("libplacebo_Deband: radius must be greater than or equal to 0.0", params->vf);

    if (avs_defined(avs_array_elt(args, Grain_neutral)))
    {
        const int grain_neutral_num{avs_array_size(avs_array_elt(args, Grain_neutral))};
        if (grain_neutral_num > g_avs_api->avs_num_components(&fi->vi))
            return set_error("libplacebo_Deband: grain_neutral index out of range.", params->vf);

        for (int i{0}; i < grain_neutral_num; ++i)
        {
            params->deband_params->grain_neutral[i] = avs_as_float(*(avs_as_array(avs_array_elt(args, Grain_neutral)) + i));
            if (params->deband_params->grain_neutral[i] < 0.0f)
                return set_error("libplacebo_Deband: grain_neutral must be greater than or equal to 0.0", params->vf);
        }
    }

    params->deband_params->grain = (avs_defined(avs_array_elt(args, Grainy))) ? avs_as_float(avs_array_elt(args, Grainy)) : 6.0f;
    if (params->deband_params->grain < 0.0f)
        return set_error("libplacebo_Deband: grainY must be greater than or equal to 0.0", params->vf);

    const float grainC{static_cast<float>(
        (avs_defined(avs_array_elt(args, Grainc))) ? avs_as_float(avs_array_elt(args, Grainc)) : params->deband_params->grain)};
    if (grainC < 0.0f)
        return set_error("libplacebo_Deband: grainC must be greater than or equal to 0.0", params->vf);

    if (params->deband_params->grain != grainC)
    {
        params->deband_params1 = std::make_unique<pl_deband_params>();
        memcpy(params->deband_params1.get(), params->deband_params.get(), sizeof(pl_deband_params));
        params->deband_params1->grain = grainC;
    }

    params->frame_index = 0;

    switch (bits)
    {
    case 8:
        params->deband_process = deband_filter<uint8_t>;
        break;
    case 16:
        params->deband_process = deband_filter<uint16_t>;
        break;
    default:
        params->deband_process = deband_filter<float>;
        break;
    }

    AVS_Value v;
    g_avs_api->avs_set_to_clip(&v,clip);

    fi->user_data = params.release();
    fi->get_frame = deband_get_frame;
    fi->set_cache_hints = deband_set_cache_hints;
    fi->free_filter = free_deband;

    return v;
}
