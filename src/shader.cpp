#include <mutex>

#ifdef _WIN32
#include <windows.h>
#endif

#include "avs_libplacebo.h"
#include "libp2p/p2p_api.h"

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
    int list_device;
    std::unique_ptr<char[]> msg;
    void* packed_dst;
};

static bool shader_do_plane(priv& p, shader& data, const pl_plane& planes_) noexcept
{
    shader* d{ &data };
    const pl_plane* pl{ &planes_ };

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
    img.planes[0] = pl[0];
    img.planes[1] = pl[1];
    img.planes[2] = pl[2];
    img.color = csp;

    if (d->subw || d->subh)
        pl_frame_set_chroma_location(&img, d->chromaLocation);

    pl_frame out{};
    out.num_planes = 1;
    out.repr = crpr;
    out.color = csp;

    for (int i{ 0 }; i < 3; ++i)
    {
        out.planes[i].texture = p.tex_out[0];
        out.planes[i].components = p.tex_out[0]->params.format->num_components;
        out.planes[i].component_mapping[0] = 0;
        out.planes[i].component_mapping[1] = 1;
        out.planes[i].component_mapping[2] = 2;
    }

    pl_render_params renderParams{};
    renderParams.hooks = &d->shader;
    renderParams.num_hooks = 1;
    renderParams.sigmoid_params = d->sigmoid_params.get();
    renderParams.disable_linear_scaling = !d->linear;
    renderParams.upscaler = &d->sample_params->filter;
    renderParams.downscaler = &d->sample_params->filter;
    renderParams.antiringing_strength = d->sample_params->antiring;
    renderParams.lut_entries = d->sample_params->lut_entries;
    renderParams.polar_cutoff = d->sample_params->cutoff;

    return pl_render_image(p.rr, &img, &out, &renderParams);
}

static int shader_reconfig(priv& priv_, const pl_plane_data& data_, shader& d, const int w, const int h)
{
    priv* p{ &priv_ };
    const pl_plane_data* data{ &data_ };

    pl_fmt fmt[3];
    for (int i{ 0 }; i < 3; ++i)
    {
        fmt[i] = pl_plane_find_fmt(p->gpu, nullptr, &data[i]);
        if (!fmt[i])
            return -1;
    }

    for (int i{ 0 }; i < 3; ++i)
    {
        pl_tex_params t_r{};
        t_r.w = data->width;
        t_r.h = data->height;
        t_r.format = fmt[i];
        t_r.sampleable = true;
        t_r.host_writable = true;

        if (!pl_tex_recreate(p->gpu, &p->tex_in[i], &t_r))
            return -2;
    }

    pl_plane_data plane_data{};
    plane_data.type = PL_FMT_UNORM;
    plane_data.pixel_stride = 6;

    for (int i{ 0 }; i < 3; ++i)
    {
        plane_data.component_map[i] = i;
        plane_data.component_pad[i] = 0;
        plane_data.component_size[i] = 16;
    }

    const pl_fmt out{ pl_plane_find_fmt(p->gpu, nullptr, &plane_data) };

    pl_tex_params t_r1{};
    t_r1.w = w;
    t_r1.h = h;
    t_r1.format = out;
    t_r1.renderable = true;
    t_r1.host_readable = true;

    if (!pl_tex_recreate(p->gpu, &p->tex_out[0], &t_r1))
        return -2;

    return 0;
}

static int shader_filter(priv& priv_, void* dst, const pl_plane_data& src_, shader& d)
{
    priv* p{ &priv_ };
    const pl_plane_data* src{ &src_ };

    // Upload planes
    pl_plane planes[3]{};

    for (int i{ 0 }; i < 3; ++i)
    {
        if (!pl_upload_plane(p->gpu, &planes[i], &p->tex_in[i], &src[i]))
            return -1;
    }

    // Process plane
    if (!shader_do_plane(*p, d, *planes))
        return -2;

    // Download planes
    pl_tex_transfer_params ttr1{};
    ttr1.tex = p->tex_out[0];
    ttr1.ptr = dst;

    if (!pl_tex_download(p->gpu, &ttr1))
        return -3;

    return 0;
}

static AVS_VideoFrame* AVSC_CC shader_get_frame(AVS_FilterInfo* fi, int n)
{
    shader* d{ reinterpret_cast<shader*>(fi->user_data) };

    if (d->list_device)
        return avs_get_frame(fi->child, n);

    const char* ErrorText{ 0 };
    AVS_VideoFrame* src{ avs_get_frame(fi->child, n) };
    AVS_VideoFrame* dst{ avs_new_video_frame(fi->env, &fi->vi) };
    avs_copy_frame_props(fi->env, src, dst);

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

    const int planes[3]{ AVS_PLANAR_Y, AVS_PLANAR_U, AVS_PLANAR_V };
    pl_plane_data pl[3]{};

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
    }

    {
        std::lock_guard<std::mutex> lck(mtx);

        const int reconf{ shader_reconfig(*d->vf.get(), *pl, *d, fi->vi.width, fi->vi.height) };
        if (reconf == 0)
        {
            const int filt{ shader_filter(*d->vf.get(), d->packed_dst, *pl, *d) };

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

        fi->error = ErrorText;

        return nullptr;
    }
    else
    {
        p2p_buffer_param pack_params{};
        pack_params.width = fi->vi.width;
        pack_params.height = fi->vi.height;
        pack_params.packing = p2p_bgr48_le;
        pack_params.src[0] = d->packed_dst;
        pack_params.src_stride[0] = fi->vi.width * 2 * 3;

        for (int k = 0; k < 3; ++k)
        {
            pack_params.dst[k] = avs_get_write_ptr_p(dst, planes[k]);
            pack_params.dst_stride[k] = avs_get_pitch_p(dst, planes[k]);
        }

        p2p_unpack_frame(&pack_params, 0);

        avs_release_video_frame(src);

        return dst;
    }
}

static void AVSC_CC free_shader(AVS_FilterInfo* fi)
{
    shader* d{ reinterpret_cast<shader*>(fi->user_data) };

    if (!d->list_device)
    {
        operator delete (d->packed_dst);
        pl_mpv_user_shader_destroy(&d->shader);
        avs_libplacebo_uninit(std::move(d->vf));
    }

    delete d;
}

static int AVSC_CC shader_set_cache_hints(AVS_FilterInfo* fi, int cachehints, int frame_range)
{
    return cachehints == AVS_CACHE_GET_MTMODE ? 2 : 0;
}

AVS_Value AVSC_CC create_shader(AVS_ScriptEnvironment* env, AVS_Value args, void* param)
{
    enum { Clip, Shader, Width, Height, Chroma_loc, Matrix, Trc, Filter, Radius, Clamp, Taper, Blur, Param1, Param2, Antiring, Lut_entries, Cutoff, Sigmoidize, Linearize, Sigmoid_center, Sigmoid_slope, Device, List_device };

    AVS_FilterInfo* fi;
    AVS_Clip* clip{ avs_new_c_filter(env, &fi, avs_array_elt(args, Clip), 1) };
    AVS_Value v{ avs_void };

    shader* params{ new shader() };

    if (!avs_is_planar(&fi->vi))
        v = avs_new_value_error("libplacebo_Shader: clip must be in planar format.");
    if (!avs_defined(v) && avs_bits_per_component(&fi->vi) != 16)
        v = avs_new_value_error("libplacebo_Shader: bit depth must be 16-bit.");
    if (!avs_defined(v) && avs_is_rgb(&fi->vi))
        v = avs_new_value_error("libplacebo_Shader: only YUV formats are supported.");
    if (!avs_defined(v))
    {
        const int device{ avs_defined(avs_array_elt(args, Device)) ? avs_as_int(avs_array_elt(args, Device)) : -1 };
        params->list_device = avs_defined(avs_array_elt(args, List_device)) ? avs_as_bool(avs_array_elt(args, List_device)) : 0;

        std::vector<VkPhysicalDevice> devices{};
        VkInstance inst{};

        if (params->list_device || device > -1)
        {
            VkInstanceCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;

            uint32_t dev_count{ 0 };

            if (vkCreateInstance(&info, nullptr, &inst))
            {
                v = avs_new_value_error("libplacebo_Shader: failed to create instance.");
                vkDestroyInstance(inst, nullptr);
            }
            if (!avs_defined(v))
            {
                if (vkEnumeratePhysicalDevices(inst, &dev_count, nullptr))
                    v = avs_new_value_error("libplacebo_Shader: failed to get devices number.");
            }
            if (!avs_defined(v))
            {
                if (device < -1 || device > static_cast<int>(dev_count) - 1)
                {
                    const std::string err_{ (std::string("libplacebo_Shader: device must be between -1 and ") + std::to_string(dev_count - 1)) };
                    params->msg = std::make_unique<char[]>(err_.size() + 1);
                    strcpy(params->msg.get(), err_.c_str());
                    v = avs_new_value_error(params->msg.get());
                }
            }
            if (!avs_defined(v))
                devices.resize(dev_count);
            if (!avs_defined(v))
            {
                if (vkEnumeratePhysicalDevices(inst, &dev_count, devices.data()))
                    v = avs_new_value_error("libplacebo_Shader: failed to get get devices.");
            }
            if (!avs_defined(v))
            {
                if (params->list_device)
                {
                    std::string text;

                    for (size_t i{ 0 }; i < devices.size(); ++i)
                    {
                        VkPhysicalDeviceProperties properties{};
                        vkGetPhysicalDeviceProperties(devices[i], &properties);

                        text += std::to_string(i) + ": " + std::string(properties.deviceName) + "\n";
                    }

                    params->msg = std::make_unique<char[]>(text.size() + 1);
                    strcpy(params->msg.get(), text.c_str());

                    vkDestroyInstance(inst, nullptr);

                    AVS_Value cl{ avs_new_value_clip(clip) };
                    AVS_Value args_[2]{ cl, avs_new_value_string(params->msg.get()) };
                    AVS_Value inv{ avs_invoke(fi->env, "Text", avs_new_value_array(args_, 2), 0) };
                    AVS_Clip* clip1{ avs_new_c_filter(env, &fi, inv, 1) };

                    v = avs_new_value_clip(clip1);

                    fi->user_data = reinterpret_cast<void*>(params);
                    fi->get_frame = shader_get_frame;
                    fi->set_cache_hints = shader_set_cache_hints;
                    fi->free_filter = free_shader;

                    avs_release_clip(clip1);
                    avs_release_value(inv);
                    avs_release_value(cl);
                    avs_release_clip(clip);

                    return v;
                }
            }
        }
        if (!avs_defined(v))
        {
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
                const std::string err{ "libplacebo_Shader: error opening file " + std::string(shader_path) + " (" + std::strerror(errno) + ")" };
                params->msg = std::make_unique<char[]>(err.size() + 1);
                strcpy(params->msg.get(), err.c_str());
                v = avs_new_value_error(params->msg.get());
            }
            if (!avs_defined(v))
            {
                if (std::fseek(shader_file, 0, SEEK_END))
                {
                    std::fclose(shader_file);
                    const std::string err{ "libplacebo_Shader: error seeking to the end of file " + std::string(shader_path) + " (" + std::strerror(errno) + ")" };
                    params->msg = std::make_unique<char[]>(err.size() + 1);
                    strcpy(params->msg.get(), err.c_str());
                    v = avs_new_value_error(params->msg.get());
                }
            }
            if (!avs_defined(v))
            {
                const long shader_size{ std::ftell(shader_file) };

                if (shader_size == -1)
                {
                    std::fclose(shader_file);
                    const std::string err{ "libplacebo_Shader: error determining the size of file " + std::string(shader_path) + " (" + std::strerror(errno) + ")" };
                    params->msg = std::make_unique<char[]>(err.size() + 1);
                    strcpy(params->msg.get(), err.c_str());
                    v = avs_new_value_error(params->msg.get());
                }

                if (!avs_defined(v))
                {
                    std::rewind(shader_file);

                    char* bdata{ new char[shader_size + 1] };
                    std::fread(bdata, 1, shader_size, shader_file);
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

                    params->shader = pl_mpv_user_shader_parse(params->vf->gpu, bdata, strlen(bdata));
                    delete[](bdata);

                    if (!params->shader)
                        v = avs_new_value_error("libplacebo_Shader: Failed parsing shader!");
                }
            }
        }
    }
    if (!avs_defined(v))
    {
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
                v = avs_new_value_error("libplacebo_Shader: sigmoid_center must be between 0.0 and 1.0.");

            if (!avs_defined(v))
            {
                params->sigmoid_params->slope = (avs_defined(avs_array_elt(args, Sigmoid_slope))) ? avs_as_float(avs_array_elt(args, Sigmoid_slope)) : 6.5f;
                if (params->sigmoid_params->slope < 1.0f || params->sigmoid_params->slope > 20.0f)
                    v = avs_new_value_error("libplacebo_Shader: sigmoid_slope must be between 1.0 and 20.0.");
            }
        }
    }
    if (!avs_defined(v))
    {
        params->sample_params = std::make_unique<pl_sample_filter_params>();

        params->sample_params->lut_entries = (avs_defined(avs_array_elt(args, Lut_entries))) ? avs_as_int(avs_array_elt(args, Lut_entries)) : 0;
        params->sample_params->cutoff = (avs_defined(avs_array_elt(args, Cutoff))) ? avs_as_float(avs_array_elt(args, Cutoff)) : 0.0f;
        params->sample_params->antiring = (avs_defined(avs_array_elt(args, Antiring))) ? avs_as_float(avs_array_elt(args, Antiring)) : 0.0f;

        const pl_filter_preset* fil{ pl_find_filter_preset((avs_defined(avs_array_elt(args, Filter))) ? avs_as_string(avs_array_elt(args, Filter)) : "ewa_lanczos") };
        if (!fil)
            v = avs_new_value_error("libplacebo_Shader: not a valid filter.");

        if (!avs_defined(v))
            params->sample_params->filter = *fil->filter;
    }
    if (!avs_defined(v))
    {
        params->sample_params->filter.clamp = (avs_defined(avs_array_elt(args, Clamp))) ? avs_as_float(avs_array_elt(args, Clamp)) : 0.0f;
        if (params->sample_params->filter.clamp < 0.0f || params->sample_params->filter.clamp > 1.0f)
            v = avs_new_value_error("libplacebo_Shader: clamp must be between 0.0 and 1.0.");
    }
    if (!avs_defined(v))
    {
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

        params->packed_dst = operator new(fi->vi.width * fi->vi.height * 2 * 3);
        fi->vi.pixel_type = AVS_CS_YUV444P16;
    }
    if (!avs_defined(v))
    {
        v = avs_new_value_clip(clip);

        fi->user_data = reinterpret_cast<void*>(params);
        fi->get_frame = shader_get_frame;
        fi->set_cache_hints = shader_set_cache_hints;
        fi->free_filter = free_shader;
    }

    avs_release_clip(clip);

    return v;
}
