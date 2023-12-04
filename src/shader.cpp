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
    std::unique_ptr<pl_sigmoid_params> sigmoid_params;
    enum pl_color_transfer trc;
    int linear;
    int subw;
    int subh;
    std::string msg;
};

static bool shader_do_plane(const shader* d, const pl_plane* planes) noexcept
{
    pl_color_repr crpr{};
    crpr.bits.bit_shift = 0;
    crpr.bits.color_depth = 16;
    crpr.bits.sample_depth = 16;
    crpr.sys = d->matrix;
    crpr.levels = d->range;

    pl_color_space csp{};
    csp.transfer = d->trc;

    pl_frame img{};
    img.num_planes = 3;
    img.repr = crpr;
    img.planes[0] = planes[0];
    img.planes[1] = planes[1];
    img.planes[2] = planes[2];
    img.color = csp;

    if (d->subw || d->subh)
        pl_frame_set_chroma_location(&img, d->chromaLocation);

    pl_frame out{};
    out.num_planes = 3;
    out.repr = crpr;
    out.color = csp;

    for (int i{ 0 }; i < 3; ++i)
    {
        out.planes[i].texture = d->vf->tex_out[i];
        out.planes[i].components = 1;
        out.planes[i].component_mapping[0] = i;
    }

    pl_render_params renderParams{};
    renderParams.hooks = &d->shader;
    renderParams.num_hooks = 1;
    renderParams.sigmoid_params = d->sigmoid_params.get();
    renderParams.disable_linear_scaling = !d->linear;
    renderParams.upscaler = &d->sample_params->filter;
    renderParams.downscaler = &d->sample_params->filter;
    renderParams.antiringing_strength = d->sample_params->antiring;

    return pl_render_image(d->vf->rr, &img, &out, &renderParams);
}

static int shader_filter(AVS_VideoFrame* dst, AVS_VideoFrame* src, shader* d, const AVS_FilterInfo* fi) noexcept
{
    const int error{ [&]()
    {
        for (int i{ 0 }; i < 3; ++i)
        {
            pl_tex_destroy(d->vf->gpu, &d->vf->tex_out[i]);
            pl_tex_destroy(d->vf->gpu, &d->vf->tex_in[i]);
        }

        return -1;
    }() };

    const pl_fmt fmt{ pl_find_named_fmt(d->vf->gpu, "r16") };
    if (!fmt)
        return error;

    pl_tex_params t_r{};
    t_r.w = fi->vi.width;
    t_r.h = fi->vi.height;
    t_r.format = fmt;
    t_r.renderable = true;
    t_r.host_readable = true;

    pl_plane pl_planes[3]{};
    constexpr int planes[3]{ AVS_PLANAR_Y, AVS_PLANAR_U, AVS_PLANAR_V };

    for (int i{ 0 }; i < 3; ++i)
    {
        const int plane{ planes[i] };

        pl_plane_data pl{};
        pl.type = PL_FMT_UNORM;
        pl.pixel_stride = 2;
        pl.component_size[0] = 16;
        pl.width = avs_get_row_size_p(src, plane) / avs_component_size(&fi->vi);
        pl.height = avs_get_height_p(src, plane);
        pl.row_stride = avs_get_pitch_p(src, plane);
        pl.pixels = avs_get_read_ptr_p(src, plane);
        pl.component_map[0] = i;

        // Upload planes
        if (!pl_upload_plane(d->vf->gpu, &pl_planes[i], &d->vf->tex_in[i], &pl))
            return error;

        if (!pl_tex_recreate(d->vf->gpu, &d->vf->tex_out[i], &t_r))
            return error;
    }

    // Process plane
    if (!shader_do_plane(d, pl_planes))
        return error;

    const int dst_stride = (avs_get_pitch(dst) + (d->vf->gpu->limits.align_tex_xfer_pitch) - 1) & ~((d->vf->gpu->limits.align_tex_xfer_pitch) - 1);
    pl_buf_params buf_params{};
    buf_params.size = dst_stride * fi->vi.height;
    buf_params.host_mapped = true;

    pl_buf dst_buf{};
    if (!pl_buf_recreate(d->vf->gpu, &dst_buf, &buf_params))
        return error;

    // Download planes
    for (int i{ 0 }; i < 3; ++i)
    {
        pl_tex_transfer_params ttr1{};
        ttr1.row_pitch = dst_stride;
        ttr1.buf = dst_buf;
        ttr1.tex = d->vf->tex_out[i];

        if (!pl_tex_download(d->vf->gpu, &ttr1))
        {
            pl_buf_destroy(d->vf->gpu, &dst_buf);
            return error;
        }

        pl_tex_destroy(d->vf->gpu, &d->vf->tex_out[i]);
        pl_tex_destroy(d->vf->gpu, &d->vf->tex_in[i]);

        while (pl_buf_poll(d->vf->gpu, dst_buf, 0));
        memcpy(avs_get_write_ptr_p(dst, planes[i]), dst_buf->data, dst_buf->params.size);
    }

    pl_buf_destroy(d->vf->gpu, &dst_buf);

    return 0;
}

static AVS_VideoFrame* AVSC_CC shader_get_frame(AVS_FilterInfo* fi, int n)
{
    shader* d{ reinterpret_cast<shader*>(fi->user_data) };

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

    if (std::lock_guard<std::mutex> lck(mtx); shader_filter(dst, src, d, fi))
    {
        d->msg = "libplacebo_Shader: " + d->vf->log_buffer.str();
        avs_release_video_frame(src);
        avs_release_video_frame(dst);

        fi->error = d->msg.c_str();

        return nullptr;
    }
    else
    {
        avs_release_video_frame(src);

        return dst;
    }
}

static void AVSC_CC free_shader(AVS_FilterInfo* fi)
{
    shader* d{ reinterpret_cast<shader*>(fi->user_data) };

    pl_mpv_user_shader_destroy(&d->shader);
    avs_libplacebo_uninit(d->vf);
    delete d;
}

static int AVSC_CC shader_set_cache_hints(AVS_FilterInfo* fi, int cachehints, int frame_range)
{
    return cachehints == AVS_CACHE_GET_MTMODE ? 2 : 0;
}

AVS_Value AVSC_CC create_shader(AVS_ScriptEnvironment* env, AVS_Value args, void* param)
{
    enum { Clip, Shader, Width, Height, Chroma_loc, Matrix, Trc, Filter, Radius, Clamp, Taper, Blur, Param1, Param2, Antiring, Sigmoidize, Linearize, Sigmoid_center, Sigmoid_slope, Shader_param, Device, List_device };

    AVS_FilterInfo* fi;
    AVS_Clip* clip{ avs_new_c_filter(env, &fi, avs_array_elt(args, Clip), 1) };

    shader* params{ new shader() };

    AVS_Value avs_ver{ avs_version(params->msg, "libplacebo_Shader", env) };
    if (avs_is_error(avs_ver))
        return avs_ver;

    if (!avs_is_planar(&fi->vi))
        return set_error(clip, "libplacebo_Shader: clip must be in planar format.", nullptr);
    if (avs_bits_per_component(&fi->vi) != 16)
        return set_error(clip, "libplacebo_Shader: bit depth must be 16-bit.", nullptr);
    if (avs_is_rgb(&fi->vi))
        return set_error(clip, "libplacebo_Shader: only YUV formats are supported.", nullptr);

    const int device{ avs_defined(avs_array_elt(args, Device)) ? avs_as_int(avs_array_elt(args, Device)) : -1 };
    const int list_device{ avs_defined(avs_array_elt(args, List_device)) ? avs_as_bool(avs_array_elt(args, List_device)) : 0 };

    if (list_device || device > -1)
    {
        std::vector<VkPhysicalDevice> devices{};
        VkInstance inst{};

        AVS_Value dev_info{ devices_info(clip, fi->env, devices, inst, params->msg, "libplacebo_Shader", device, list_device) };
        if (avs_is_error(dev_info) || avs_is_clip(dev_info))
            return dev_info;

        params->vf = avs_libplacebo_init(devices[device], params->msg);

        vkDestroyInstance(inst, nullptr);
    }
    else
    {
        if (device < -1)
            return set_error(clip, "libplacebo_Shader: device must be greater than or equal to -1.", nullptr);

        params->vf = avs_libplacebo_init(nullptr, params->msg);
    }

    if (params->msg.size())
    {
        params->msg = "libplacebo_Shader: " + params->msg;
        return set_error(clip, params->msg.c_str(), nullptr);
    }

    const char* shader_path{ avs_as_string(avs_array_elt(args, Shader)) };
    FILE* shader_file{ nullptr };

#ifdef _WIN32
    const int required_size{ MultiByteToWideChar(CP_UTF8, 0, shader_path, -1, nullptr, 0) };
    std::wstring wbuffer(required_size, 0);
    MultiByteToWideChar(CP_UTF8, 0, shader_path, -1, wbuffer.data(), required_size);
    shader_file = _wfopen(wbuffer.c_str(), L"rb");
    if (!shader_file)
    {
        const int req_size{ MultiByteToWideChar(CP_ACP, 0, shader_path, -1, nullptr, 0) };
        wbuffer.resize(req_size);
        MultiByteToWideChar(CP_ACP, 0, shader_path, -1, wbuffer.data(), req_size);
        shader_file = _wfopen(wbuffer.c_str(), L"rb");
    }
#else
    shader_file = std::fopen(shader_path, "rb");
#endif
    if (!shader_file)
    {
        params->msg = "libplacebo_Shader: error opening file " + std::string(shader_path) + " (" + std::strerror(errno) + ")";
        return set_error(clip, params->msg.c_str(), params->vf);
    }

    if (std::fseek(shader_file, 0, SEEK_END))
    {
        std::fclose(shader_file);
        params->msg = "libplacebo_Shader: error seeking to the end of file " + std::string(shader_path) + " (" + std::strerror(errno) + ")";
        return set_error(clip, params->msg.c_str(), params->vf);
    }

    const long shader_size{ std::ftell(shader_file) };

    if (shader_size == -1)
    {
        std::fclose(shader_file);
        params->msg = "libplacebo_Shader: error determining the size of file " + std::string(shader_path) + " (" + std::strerror(errno) + ")";
        return set_error(clip, params->msg.c_str(), params->vf);
    }

    std::rewind(shader_file);

    std::string bdata(shader_size, ' ');
    std::fread(bdata.data(), 1, shader_size, shader_file);
    bdata[shader_size] = '\0';

    std::fclose(shader_file);

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
            return set_error(clip, "libplacebo_Shader: failed parsing shader_param (wrong format).", params->vf);

        std::string reg_parse{ "(\\w+)=([^ >]+)" };
        for (int i{ 0 }; i < num_spaces; ++i)
            reg_parse += "(?: (\\w+)=([^ >]+))";

        std::regex reg(reg_parse);
        std::smatch match;
        if (!std::regex_match(shader_p.cbegin(), shader_p.cend(), match, reg))
            return set_error(clip, "libplacebo_Shader: regex failed parsing shader_param.", params->vf);

        for (int i = 1; match[i + 1].matched; i += 2)
            bdata = std::regex_replace(bdata, std::regex(std::string("(#define\\s") + match[i].str() + std::string("\\s+)(.+?)(?=\\/\\/|\\s)")), "$01" + match[i + 1].str());
    }

    params->shader = pl_mpv_user_shader_parse(params->vf->gpu, bdata.c_str(), bdata.size());
    if (!params->shader)
        return set_error(clip, "libplacebo_Shader: failed parsing shader!", params->vf);

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
        {
            pl_mpv_user_shader_destroy(&params->shader);
            return set_error(clip, "libplacebo_Shader: sigmoid_center must be between 0.0 and 1.0.", params->vf);
        }

        params->sigmoid_params->slope = (avs_defined(avs_array_elt(args, Sigmoid_slope))) ? avs_as_float(avs_array_elt(args, Sigmoid_slope)) : 6.5f;
        if (params->sigmoid_params->slope < 1.0f || params->sigmoid_params->slope > 20.0f)
        {
            pl_mpv_user_shader_destroy(&params->shader);
            return set_error(clip, "libplacebo_Shader: sigmoid_slope must be between 1.0 and 20.0.", params->vf);
        }
    }

    params->sample_params = std::make_unique<pl_sample_filter_params>();

    params->sample_params->antiring = (avs_defined(avs_array_elt(args, Antiring))) ? avs_as_float(avs_array_elt(args, Antiring)) : 0.0f;
    if (params->sample_params->antiring < 0.0f || params->sample_params->antiring > 1.0f)
    {
        pl_mpv_user_shader_destroy(&params->shader);
        return set_error(clip, "libplacebo_Shader: antiring must be between 0.0 and 1.0.", params->vf);
    }

    const pl_filter_config* filter_config{ pl_find_filter_config((avs_defined(avs_array_elt(args, Filter))) ? avs_as_string(avs_array_elt(args, Filter)) : "ewa_lanczos", PL_FILTER_UPSCALING) };
    if (!filter_config)
    {
        pl_mpv_user_shader_destroy(&params->shader);
        return set_error(clip, "libplacebo_Shader: not a valid filter.", params->vf);
    }

    params->sample_params->filter = *filter_config;
    params->sample_params->filter.clamp = (avs_defined(avs_array_elt(args, Clamp))) ? avs_as_float(avs_array_elt(args, Clamp)) : 0.0f;
    if (params->sample_params->filter.clamp < 0.0f || params->sample_params->filter.clamp > 1.0f)
    {
        pl_mpv_user_shader_destroy(&params->shader);
        return set_error(clip, "libplacebo_Shader: clamp must be between 0.0 and 1.0.", params->vf);
    }

    params->sample_params->filter.blur = (avs_defined(avs_array_elt(args, Blur))) ? avs_as_float(avs_array_elt(args, Blur)) : 0.0f;
    if (params->sample_params->filter.blur < 0.0f || params->sample_params->filter.blur > 100.0f)
    {
        pl_mpv_user_shader_destroy(&params->shader);
        return set_error(clip, "libplacebo_Shader: blur must be between 0.0 and 100.0.", params->vf);
    }

    params->sample_params->filter.taper = (avs_defined(avs_array_elt(args, Taper))) ? avs_as_float(avs_array_elt(args, Taper)) : 0.0f;
    if (params->sample_params->filter.taper < 0.0f || params->sample_params->filter.taper > 1.0f)
    {
        pl_mpv_user_shader_destroy(&params->shader);
        return set_error(clip, "libplacebo_Shader: taper must be between 0.0 and 1.0.", params->vf);
    }

    if (avs_defined(avs_array_elt(args, Radius)))
    {
        params->sample_params->filter.radius = avs_as_float(avs_array_elt(args, Radius));
        if (params->sample_params->filter.radius < 0.0f || params->sample_params->filter.radius > 16.0f)
        {
            pl_mpv_user_shader_destroy(&params->shader);
            return set_error(clip, "libplacebo_Shader: radius must be between 0.0 and 16.0.", params->vf);
        }
    }

    if (avs_defined(avs_array_elt(args, Param1)))
        params->sample_params->filter.params[0] = avs_as_float(avs_array_elt(args, Param1));
    if (avs_defined(avs_array_elt(args, Param2)))
        params->sample_params->filter.params[1] = avs_as_float(avs_array_elt(args, Param2));

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
