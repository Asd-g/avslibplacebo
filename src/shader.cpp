#include <cstring>
#include <mutex>
#include <regex>

#ifdef _WIN32
#include <windows.h>
#endif

#include "avs_libplacebo.h"

static std::mutex mtx;

struct shader
{
    std::unique_ptr<priv> vf;
    const pl_hook* shader;
    enum pl_color_system matrix;
    enum pl_color_levels range;
    enum pl_chroma_location chromaLocation;
    std::unique_ptr<pl_sample_filter_params> sample_params;
    std::unique_ptr<pl_filter_function> filter;
    std::unique_ptr<pl_sigmoid_params> sigmoid_params;
    enum pl_color_transfer trc;
    int linear;
    int subw;
    int subh;
    std::string msg;
};

static bool shader_do_plane(priv& p, const shader& data, const pl_plane* planes) noexcept
{
    pl_color_repr crpr{};
    crpr.bits.bit_shift = 0;
    crpr.bits.color_depth = 16;
    crpr.bits.sample_depth = 16;
    crpr.sys = data.matrix;
    crpr.levels = data.range;

    pl_color_space csp{};
    csp.transfer = data.trc;

    pl_frame img{};
    img.num_planes = 3;
    img.repr = crpr;
    img.planes[0] = planes[0];
    img.planes[1] = planes[1];
    img.planes[2] = planes[2];
    img.color = csp;

    if (data.subw || data.subh)
        pl_frame_set_chroma_location(&img, data.chromaLocation);

    pl_frame out{};
    out.num_planes = 3;
    out.repr = crpr;
    out.color = csp;

    for (int i{ 0 }; i < 3; ++i)
    {
        out.planes[i].texture = p.tex_out[i];
        out.planes[i].components = 1;
        out.planes[i].component_mapping[0] = i;
    }

    pl_render_params renderParams{};
    renderParams.hooks = &data.shader;
    renderParams.num_hooks = 1;
    renderParams.sigmoid_params = data.sigmoid_params.get();
    renderParams.disable_linear_scaling = !data.linear;
    renderParams.upscaler = &data.sample_params->filter;
    renderParams.downscaler = &data.sample_params->filter;
    renderParams.antiringing_strength = data.sample_params->antiring;
    renderParams.lut_entries = data.sample_params->lut_entries;
    renderParams.polar_cutoff = data.sample_params->cutoff;

    return pl_render_image(p.rr, &img, &out, &renderParams);
}

static int shader_reconfig(priv& p, const pl_plane_data* data, const shader& d, const int w, const int h)
{
    for (int i{ 0 }; i < 3; ++i)
    {
        pl_fmt fmt{ pl_plane_find_fmt(p.gpu, nullptr, &data[i]) };
        if (!fmt)
            return -1;

        pl_tex_params t_r{};
        t_r.w = data[i].width;
        t_r.h = data[i].height;
        t_r.format = fmt;
        t_r.sampleable = true;
        t_r.host_writable = true;

        if (!pl_tex_recreate(p.gpu, &p.tex_in[i], &t_r))
            return -2;

        t_r.w = w;
        t_r.h = h;
        t_r.sampleable = false;
        t_r.host_writable = false;
        t_r.renderable = true;
        t_r.host_readable = true;

        if (!pl_tex_recreate(p.gpu, &p.tex_out[i], &t_r))
            return -2;
    }

    return 0;
}

static int shader_filter(priv& p, const pl_buf* dst, const pl_plane_data* src, shader& d, const int dst_stride)
{
    // Upload planes
    pl_plane planes[3]{};

    for (int i{ 0 }; i < 3; ++i)
    {
        if (!pl_upload_plane(p.gpu, &planes[i], &p.tex_in[i], &src[i]))
            return -1;
    }

    // Process plane
    if (!shader_do_plane(p, d, planes))
        return -2;

    // Download planes
    for (int i{ 0 }; i < 3; ++i)
    {
        pl_tex_transfer_params ttr1{};
        ttr1.tex = p.tex_out[i];
        ttr1.row_pitch = dst_stride;
        ttr1.buf = dst[i];

        if (!pl_tex_download(p.gpu, &ttr1))
            return -3;
    }

    return 0;
}

static AVS_VideoFrame* AVSC_CC shader_get_frame(AVS_FilterInfo* fi, int n)
{
    shader* d{ reinterpret_cast<shader*>(fi->user_data) };

    const char* ErrorText{ 0 };
    AVS_VideoFrame* src{ avs_get_frame(fi->child, n) };
    if (!src)
        return nullptr;

    AVS_VideoFrame* dst{ avs_new_video_frame_p(fi->env, &fi->vi, src) };

    if (d->range == PL_COLOR_LEVELS_UNKNOWN)
    {
        const AVS_Map* props{ avs_get_frame_props_ro(fi->env, src) };

        int err{ 0 };
        const int64_t r{ avs_prop_get_int(fi->env, props, "_ColorRange", 0, &err) };
        if (err)
            d->range = PL_COLOR_LEVELS_LIMITED;
        else
            d->range = (r) ? PL_COLOR_LEVELS_LIMITED : PL_COLOR_LEVELS_FULL;
    }

    constexpr int planes[3]{ AVS_PLANAR_Y, AVS_PLANAR_U, AVS_PLANAR_V };
    const int dst_stride = ((avs_get_pitch(dst)) + (d->vf->gpu->limits.align_tex_xfer_pitch) - 1) & ~((d->vf->gpu->limits.align_tex_xfer_pitch) - 1);
    pl_plane_data pl[4]{};
    pl_buf dst_buf[4]{};
    pl_buf_params buf_params{};
    buf_params.size = dst_stride * fi->vi.height;
    buf_params.host_mapped = true;

    for (int i{ 0 }; i < 3; ++i)
    {
        pl[i].type = PL_FMT_UNORM;
        pl[i].width = avs_get_row_size_p(src, planes[i]) / avs_component_size(&fi->vi);
        pl[i].height = avs_get_height_p(src, planes[i]);
        pl[i].pixel_stride = 2;
        pl[i].row_stride = avs_get_pitch_p(src, planes[i]);
        pl[i].pixels = avs_get_read_ptr_p(src, planes[i]);
        pl[i].component_size[0] = 16;
        pl[i].component_pad[0] = 0;
        pl[i].component_map[0] = i;

        dst_buf[i] = pl_buf_create(d->vf->gpu, &buf_params);
    }

    {
        std::lock_guard<std::mutex> lck(mtx);

        const int reconf{ shader_reconfig(*d->vf.get(), pl, *d, fi->vi.width, fi->vi.height) };
        if (reconf == 0)
        {
            const int filt{ shader_filter(*d->vf.get(), dst_buf, pl, *d, dst_stride) };

            if (filt)
            {
                switch (filt)
                {
                    case -1: ErrorText = "libplacebo_Shader: failed uploading data to the GPU!"; break;
                    case -2: ErrorText = "libplacebo_Shader: failed processing planes!"; break;
                    default: ErrorText = "libplacebo_Shader: failed downloading data from the GPU!";
                }
            }
        }
        else
        {
            switch (reconf)
            {
                case -1: ErrorText = "libplacebo_Shader: failed configuring filter: no good texture format!"; break;
                default: ErrorText = "libplacebo_Shader: failed creating GPU textures!";
            }
        }
    }

    if (ErrorText)
    {
        avs_release_video_frame(src);
        avs_release_video_frame(dst);

        for (int i{ 0 }; i < 3; ++i)
            pl_buf_destroy(d->vf->gpu, &dst_buf[i]);

        fi->error = ErrorText;

        return nullptr;
    }
    else
    {
        for (int i{ 0 }; i < 3; ++i)
        {
            while (pl_buf_poll(d->vf->gpu, dst_buf[i], 0));
            memcpy(avs_get_write_ptr_p(dst, planes[i]), dst_buf[i]->data, dst_buf[i]->params.size);
            pl_buf_destroy(d->vf->gpu, &dst_buf[i]);
        }

        avs_release_video_frame(src);

        return dst;
    }
}

static void AVSC_CC free_shader(AVS_FilterInfo* fi)
{
    shader* d{ reinterpret_cast<shader*>(fi->user_data) };

    pl_mpv_user_shader_destroy(&d->shader);
    avs_libplacebo_uninit(std::move(d->vf));
    delete d;
}

static int AVSC_CC shader_set_cache_hints(AVS_FilterInfo* fi, int cachehints, int frame_range)
{
    return cachehints == AVS_CACHE_GET_MTMODE ? 2 : 0;
}

AVS_Value AVSC_CC create_shader(AVS_ScriptEnvironment* env, AVS_Value args, void* param)
{
    enum { Clip, Shader, Width, Height, Chroma_loc, Matrix, Trc, Filter, Radius, Clamp, Taper, Blur, Param1, Param2, Antiring, Lut_entries, Cutoff, Sigmoidize, Linearize, Sigmoid_center, Sigmoid_slope, Shader_param, Device, List_device };

    AVS_FilterInfo* fi;
    AVS_Clip* clip{ avs_new_c_filter(env, &fi, avs_array_elt(args, Clip), 1) };

    shader* params{ new shader() };

    AVS_Value avs_ver{ avs_version("libplacebo_Shader", env) };
    if (avs_is_error(avs_ver))
        return avs_ver;

    if (!avs_is_planar(&fi->vi))
        return set_error(clip, "libplacebo_Shader: clip must be in planar format.");
    if (avs_bits_per_component(&fi->vi) != 16)
        return set_error(clip, "libplacebo_Shader: bit depth must be 16-bit.");
    if (avs_is_rgb(&fi->vi))
        return set_error(clip, "libplacebo_Shader: only YUV formats are supported.");

    const int device{ avs_defined(avs_array_elt(args, Device)) ? avs_as_int(avs_array_elt(args, Device)) : -1 };
    const int list_device{ avs_defined(avs_array_elt(args, List_device)) ? avs_as_bool(avs_array_elt(args, List_device)) : 0 };

    std::vector<VkPhysicalDevice> devices{};
    VkInstance inst{};

    if (list_device || device > -1)
    {
        AVS_Value dev_info{ devices_info(clip, fi->env, devices, inst, params->msg, "libplacebo_Shader", device, list_device) };
        if (avs_is_error(dev_info) || avs_is_clip(dev_info))
            return dev_info;
    }
    else if (device < -1)
    {
        vkDestroyInstance(inst, nullptr);
        return set_error(clip, "libplacebo_Shader: device must be greater than or equal to -1.");
    }

    const char* shader_path{ avs_as_string(avs_array_elt(args, Shader)) };
    FILE* shader_file{ nullptr };

#ifdef _WIN32
    const int required_size{ MultiByteToWideChar(CP_UTF8, 0, shader_path, -1, nullptr, 0) };
    std::unique_ptr<wchar_t[]> wbuffer{ std::make_unique<wchar_t[]>(required_size) };
    MultiByteToWideChar(CP_UTF8, 0, shader_path, -1, wbuffer.get(), required_size);
    shader_file = _wfopen(wbuffer.get(), L"rb");
#else
    shader_file = std::fopen(shader_path, "rb");
#endif
    if (!shader_file)
    {
        params->msg = "libplacebo_Shader: error opening file " + std::string(shader_path) + " (" + std::strerror(errno) + ")";
        return set_error(clip, params->msg.c_str());
    }

    if (std::fseek(shader_file, 0, SEEK_END))
    {
        std::fclose(shader_file);
        params->msg = "libplacebo_Shader: error seeking to the end of file " + std::string(shader_path) + " (" + std::strerror(errno) + ")";
        return set_error(clip, params->msg.c_str());
    }

    const long shader_size{ std::ftell(shader_file) };

    if (shader_size == -1)
    {
        std::fclose(shader_file);
        params->msg = "libplacebo_Shader: error determining the size of file " + std::string(shader_path) + " (" + std::strerror(errno) + ")";
        return set_error(clip, params->msg.c_str());
    }

    std::rewind(shader_file);

    std::string bdata;
    bdata.resize(shader_size);
    std::fread(bdata.data(), 1, shader_size, shader_file);
    bdata[shader_size] = '\0';

    std::fclose(shader_file);

    if (device == -1)
    {
        devices.resize(1);
        params->vf = avs_libplacebo_init(devices[0]);
    }
    else
        params->vf = avs_libplacebo_init(devices[device]);

    vkDestroyInstance(inst, nullptr);

    if (avs_defined(avs_array_elt(args, Shader_param)))
    {
        std::string shader_p{ avs_as_string(avs_array_elt(args, Shader_param)) };

        int num_spaces{ 0 };
        int num_equals{ -1 };
        for (auto& string : shader_p)
        {
            if (string == ' ')
                ++num_spaces;
            if (string == '=')
                ++num_equals;
        }
        if (num_spaces != num_equals)
            return set_error(clip, "libplacebo_Shader: failed parsing shader_param.");

        std::string reg_parse{ "(\\w+)=([^ >]+)" };
        for (int i{ 0 }; i < num_spaces; ++i)
            reg_parse += "(?: (\\w+)=([^ >]+))";

        std::regex reg(reg_parse);
        std::smatch match;
        if (!std::regex_match(shader_p.cbegin(), shader_p.cend(), match, reg))
            return set_error(clip, "libplacebo_Shader: failed parsing shader_param.");

        for (int i = 1; match[i + 1].matched; i += 2)
            bdata = std::regex_replace(bdata, std::regex(std::string("(#define\\s") + match[i].str() + std::string("\\s+)(.+?)(?=\\/\\/|\\s)")), "$01" + match[i + 1].str());
    }

    params->shader = pl_mpv_user_shader_parse(params->vf->gpu, bdata.c_str(), bdata.size());
    if (!params->shader)
        return set_error(clip, "libplacebo_Shader: failed parsing shader!");

    params->range = PL_COLOR_LEVELS_UNKNOWN;
    params->matrix = static_cast<pl_color_system>((avs_defined(avs_array_elt(args, Matrix))) ? avs_as_int(avs_array_elt(args, Matrix)) : 2);

    if (avs_defined(avs_array_elt(args, Width)))
        fi->vi.width = avs_as_int(avs_array_elt(args, Width));
    if (avs_defined(avs_array_elt(args, Height)))
        fi->vi.height = avs_as_int(avs_array_elt(args, Height));

    params->chromaLocation = static_cast<pl_chroma_location>((avs_defined(avs_array_elt(args, Chroma_loc))) ? avs_as_int(avs_array_elt(args, Chroma_loc)) : 1);
    params->linear = (avs_defined(avs_array_elt(args, Linearize))) ? avs_as_bool(avs_array_elt(args, Linearize)) : 1;
    params->trc = static_cast<pl_color_transfer>((avs_defined(avs_array_elt(args, Trc))) ? avs_as_int(avs_array_elt(args, Trc)) : 1);
    const int sigmoid{ (avs_defined(avs_array_elt(args, Sigmoidize))) ? avs_as_bool(avs_array_elt(args, Sigmoidize)) : 1 };

    if (sigmoid)
    {
        params->sigmoid_params = std::make_unique<pl_sigmoid_params>();

        params->sigmoid_params->center = (avs_defined(avs_array_elt(args, Sigmoid_center))) ? avs_as_float(avs_array_elt(args, Sigmoid_center)) : 0.75f;
        if (params->sigmoid_params->center < 0.0f || params->sigmoid_params->center > 1.0f)
            return set_error(clip, "libplacebo_Shader: sigmoid_center must be between 0.0 and 1.0.");

        params->sigmoid_params->slope = (avs_defined(avs_array_elt(args, Sigmoid_slope))) ? avs_as_float(avs_array_elt(args, Sigmoid_slope)) : 6.5f;
        if (params->sigmoid_params->slope < 1.0f || params->sigmoid_params->slope > 20.0f)
            return set_error(clip, "libplacebo_Shader: sigmoid_slope must be between 1.0 and 20.0.");
    }

    params->sample_params = std::make_unique<pl_sample_filter_params>();

    params->sample_params->lut_entries = (avs_defined(avs_array_elt(args, Lut_entries))) ? avs_as_int(avs_array_elt(args, Lut_entries)) : 0;
    params->sample_params->cutoff = (avs_defined(avs_array_elt(args, Cutoff))) ? avs_as_float(avs_array_elt(args, Cutoff)) : 0.0f;
    params->sample_params->antiring = (avs_defined(avs_array_elt(args, Antiring))) ? avs_as_float(avs_array_elt(args, Antiring)) : 0.0f;

    const pl_filter_preset* fil{ pl_find_filter_preset((avs_defined(avs_array_elt(args, Filter))) ? avs_as_string(avs_array_elt(args, Filter)) : "ewa_lanczos") };
    if (!fil)
        return set_error(clip, "libplacebo_Shader: not a valid filter.");

    params->sample_params->filter = *fil->filter;

    params->sample_params->filter.clamp = (avs_defined(avs_array_elt(args, Clamp))) ? avs_as_float(avs_array_elt(args, Clamp)) : 0.0f;
    if (params->sample_params->filter.clamp < 0.0f || params->sample_params->filter.clamp > 1.0f)
        return set_error(clip, "libplacebo_Shader: clamp must be between 0.0 and 1.0.");

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

    params->subw = avs_get_plane_width_subsampling(&fi->vi, AVS_PLANAR_U);
    params->subh = avs_get_plane_height_subsampling(&fi->vi, AVS_PLANAR_U);

    fi->vi.pixel_type = AVS_CS_YUV444P16;

    AVS_Value v{ avs_new_value_clip(clip) };

    fi->user_data = reinterpret_cast<void*>(params);
    fi->get_frame = shader_get_frame;
    fi->set_cache_hints = shader_set_cache_hints;
    fi->free_filter = free_shader;

    avs_release_clip(clip);

    return v;
}
