#include <mutex>

#include "avs_libplacebo.h"
#include "p2p_api.h"

extern "C" {
#include "libdovi/rpu_parser.h"
}

static std::mutex mtx;

static std::unique_ptr<pl_dovi_metadata> create_dovi_meta(DoviRpuOpaque& rpu, const DoviRpuDataHeader& hdr)
{
    std::unique_ptr<pl_dovi_metadata> dovi_meta{ std::make_unique<pl_dovi_metadata>() }; // persist state
    if (hdr.use_prev_vdr_rpu_flag)
        goto done;

    {
        const DoviRpuDataMapping* vdm{ dovi_rpu_get_data_mapping(&rpu) };
        if (!vdm)
            goto skip_vdm;

        {
            const uint64_t bits{ hdr.bl_bit_depth_minus8 + 8 };
            const float scale{ 1.0f / (1 << hdr.coefficient_log2_denom) };

            for (int c{ 0 }; c < 3; ++c)
            {
                pl_dovi_metadata::pl_reshape_data* cmp{ &dovi_meta->comp[c] };
                uint16_t pivot{ 0 };
                cmp->num_pivots = hdr.num_pivots_minus_2[c] + 2;
                for (int pivot_idx{ 0 }; pivot_idx < cmp->num_pivots; ++pivot_idx)
                {
                    pivot += hdr.pred_pivot_value[c].data[pivot_idx];
                    cmp->pivots[pivot_idx] = static_cast<float>(pivot) / ((1 << bits) - 1);
                }

                for (int i{ 0 }; i < cmp->num_pivots - 1; ++i)
                {
                    memset(cmp->poly_coeffs[i], 0, sizeof(cmp->poly_coeffs[i]));
                    cmp->method[i] = vdm->mapping_idc[c].data[i];

                    switch (cmp->method[i])
                    {
                        case 0: // polynomial
                            for (int k{ 0 }; k <= vdm->poly_order_minus1[c].data[i] + 1; ++k)
                            {
                                int64_t ipart{ vdm->poly_coef_int[c].list[i]->data[k] };
                                uint64_t fpart{ vdm->poly_coef[c].list[i]->data[k] };
                                cmp->poly_coeffs[i][k] = ipart + scale * fpart;
                            }
                            break;
                        case 1: // MMR
                            int64_t ipart{ vdm->mmr_constant_int[c].data[i] };
                            uint64_t fpart{ vdm->mmr_constant[c].data[i] };
                            cmp->mmr_constant[i] = ipart + scale * fpart;
                            cmp->mmr_order[i] = vdm->mmr_order_minus1[c].data[i] + 1;
                            for (int j{ 1 }; j <= cmp->mmr_order[i]; ++j)
                            {
                                for (int k{ 0 }; k < 7; ++k)
                                {
                                    ipart = vdm->mmr_coef_int[c].list[i]->list[j]->data[k];
                                    fpart = vdm->mmr_coef[c].list[i]->list[j]->data[k];
                                    cmp->mmr_coeffs[i][j - 1][k] = ipart + scale * fpart;
                                }
                            }
                            break;
                    }
                }
            }
        }

        dovi_rpu_free_data_mapping(vdm);
    }
skip_vdm:

    if (hdr.vdr_dm_metadata_present_flag)
    {
        const DoviVdrDmData* dm_data{ dovi_rpu_get_vdr_dm_data(&rpu) };
        if (!dm_data)
            goto done;

        const uint32_t* off{ &dm_data->ycc_to_rgb_offset0 };
        for (int i{ 0 }; i < 3; ++i)
            dovi_meta->nonlinear_offset[i] = static_cast<float>(off[i]) / (1 << 28);

        const int16_t* src{ &dm_data->ycc_to_rgb_coef0 };
        float* dst{ &dovi_meta->nonlinear.m[0][0] };
        for (int i{ 0 }; i < 9; ++i)
            dst[i] = src[i] / 8192.0;

        src = &dm_data->rgb_to_lms_coef0;
        dst = &dovi_meta->linear.m[0][0];
        for (int i{ 0 }; i < 9; ++i)
            dst[i] = src[i] / 16384.0;

        dovi_rpu_free_vdr_dm_data(dm_data);
    }

done:
    return dovi_meta;
}

enum supported_colorspace
{
    CSP_SDR = 0,
    CSP_HDR10,
    CSP_HLG,
    CSP_DOVI,
};

struct tonemap
{
    std::unique_ptr<priv> vf;
    std::unique_ptr<pl_render_params> render_params;
    enum supported_colorspace src_csp;
    enum supported_colorspace dst_csp;
    std::unique_ptr<pl_color_space> src_pl_csp;
    std::unique_ptr<pl_color_space> dst_pl_csp;
    int64_t original_src_max;
    int64_t original_src_min;
    int is_subsampled;
    enum pl_chroma_location chromaLocation;
    int list_device;
    std::unique_ptr<char[]> msg;
    std::unique_ptr<pl_color_repr> src_repr;
    std::unique_ptr<pl_color_repr> dst_repr;
    int use_dovi;
    void* packed_dst;
    std::unique_ptr<pl_color_map_params> colorMapParams;
    std::unique_ptr<pl_peak_detect_params> peakDetectParams;
};

static bool tonemap_do_plane(priv& p, tonemap& data, const pl_plane& planes_, const pl_color_repr& src_repr, const pl_color_repr& dst_repr) noexcept
{
    tonemap* d{ &data };
    const pl_plane* planes{ &planes_ };

    pl_frame img{};
    img.num_planes = 3;
    img.repr = src_repr;
    img.planes[0] = planes[0];
    img.planes[1] = planes[1];
    img.planes[2] = planes[2];
    img.color = *d->src_pl_csp;

    if (d->is_subsampled)
        pl_frame_set_chroma_location(&img, d->chromaLocation);

    pl_frame out{};
    out.num_planes = 1;
    out.repr = dst_repr;
    out.color = *d->dst_pl_csp;

    for (int i{ 0 }; i < 3; ++i)
    {
        out.planes[i].texture = p.tex_out[0];
        out.planes[i].components = p.tex_out[0]->params.format->num_components;
        out.planes[i].component_mapping[0] = 0;
        out.planes[i].component_mapping[1] = 1;
        out.planes[i].component_mapping[2] = 2;
    }

    return pl_render_image(p.rr, &img, &out, d->render_params.get());
}

static int tonemap_reconfig(priv& priv_, const pl_plane_data& data)
{
    priv* p{ &priv_ };

    pl_fmt fmt{ pl_plane_find_fmt(p->gpu, nullptr, &data) };
    if (!fmt)
        return -1;

    for (int i{ 0 }; i < 3; ++i)
    {
        pl_tex_params t_r{};
        t_r.w = data.width;
        t_r.h = data.height;
        t_r.format = fmt;
        t_r.sampleable = true;
        t_r.host_writable = true;

        if (!pl_tex_recreate(p->gpu, &p->tex_in[i], &t_r))
            return -2;
    }

    pl_plane_data plane_data{};
    plane_data.type = PL_FMT_UNORM;
    plane_data.pixel_stride = 6;
    plane_data.width = 10;
    plane_data.height = 10;
    plane_data.row_stride = 60;
    plane_data.pixel_stride = 6;

    for (int i{ 0 }; i < 3; ++i)
    {
        plane_data.component_map[i] = i;
        plane_data.component_pad[i] = 0;
        plane_data.component_size[i] = 16;
    }

    const pl_fmt out{ pl_plane_find_fmt(p->gpu, nullptr, &plane_data) };

    pl_tex_params t_r1{};
    t_r1.w = data.width;
    t_r1.h = data.height;
    t_r1.format = out;
    t_r1.renderable = true;
    t_r1.host_readable = true;
    t_r1.storable = true;
    t_r1.blit_dst = true;

    if (!pl_tex_recreate(p->gpu, &p->tex_out[0], &t_r1))
        return -2;

    return 0;
}

static int tonemap_filter(priv& priv_, void* dst, const pl_plane_data& src_, tonemap& d, const pl_color_repr& src_repr, const pl_color_repr& dst_repr)
{
    priv* p{ &priv_ };
    const pl_plane_data* src{ &src_ };

    // Upload planes
    pl_plane planes[4]{};

    for (int i{ 0 }; i < 3; ++i)
    {
        if (!pl_upload_plane(p->gpu, &planes[i], &p->tex_in[i], &src[i]))
            return -1;
    }

    // Process plane
    if (!tonemap_do_plane(*p, d, *planes, src_repr, dst_repr))
        return -2;

    pl_fmt out_fmt = p->tex_out[0]->params.format;

    // Download planes
    pl_tex_transfer_params ttr1{};
    ttr1.tex = p->tex_out[0];
    ttr1.row_pitch = (src->row_stride / src->pixel_stride) * out_fmt->texel_size;
    ttr1.ptr = dst;

    if (!pl_tex_download(p->gpu, &ttr1))
        return -3;

    return 0;
}

static AVS_VideoFrame* AVSC_CC tonemap_get_frame(AVS_FilterInfo* fi, int n)
{
    tonemap* d{ reinterpret_cast<tonemap*>(fi->user_data) };

    const char* ErrorText{ 0 };
    AVS_VideoFrame* src{ avs_get_frame(fi->child, n) };
    if (!src)
        return nullptr;

    AVS_VideoFrame* dst{ avs_new_video_frame_p(fi->env, &fi->vi, src) };

    int err;
    const AVS_Map* props{ avs_get_frame_props_ro(fi->env, src) };

    int64_t props_levels{ avs_prop_get_int(fi->env, props, "_ColorRange", 0, &err) };
    if (!err)
        // Existing range prop
        d->src_repr->levels = (props_levels) ? PL_COLOR_LEVELS_LIMITED : PL_COLOR_LEVELS_FULL;

    if (!avs_is_rgb(&fi->vi))
    {
        if (!err && !props_levels)
            // Existing range & not limited
            d->dst_repr->levels = PL_COLOR_LEVELS_FULL;
    }

    // ST2086 metadata
    // Update metadata from props
    const double maxCll{ avs_prop_get_float(fi->env, props, "ContentLightLevelMax", 0, &err) };
    const double maxFall{ avs_prop_get_float(fi->env, props, "ContentLightLevelAverage", 0, &err) };

    d->src_pl_csp->hdr.max_cll = maxCll;
    d->src_pl_csp->hdr.max_fall = maxFall;

    if (d->original_src_max < 1)
        d->src_pl_csp->hdr.max_luma = avs_prop_get_float(fi->env, props, "MasteringDisplayMaxLuminance", 0, &err);
    if (d->original_src_min <= 0)
        d->src_pl_csp->hdr.min_luma = avs_prop_get_float(fi->env, props, "MasteringDisplayMinLuminance", 0, &err);

    pl_color_space_infer(d->src_pl_csp.get());

    const double* primariesX{ avs_prop_get_float_array(fi->env, props, "MasteringDisplayPrimariesX", &err) };
    const double* primariesY{ avs_prop_get_float_array(fi->env, props, "MasteringDisplayPrimariesY", &err) };

    if (primariesX && primariesY && avs_prop_num_elements(fi->env, props, "MasteringDisplayPrimariesX") == 3 && avs_prop_num_elements(fi->env, props, "MasteringDisplayPrimariesY") == 3)
    {
        d->src_pl_csp->hdr.prim.red.x = primariesX[0];
        d->src_pl_csp->hdr.prim.red.y = primariesY[0];
        d->src_pl_csp->hdr.prim.green.x = primariesX[1];
        d->src_pl_csp->hdr.prim.green.y = primariesY[1];
        d->src_pl_csp->hdr.prim.blue.x = primariesX[2];
        d->src_pl_csp->hdr.prim.blue.y = primariesY[2];

        // White point comes with primaries
        const double whitePointX{ avs_prop_get_float(fi->env, props, "MasteringDisplayWhitePointX", 0, &err) };
        const double whitePointY{ avs_prop_get_float(fi->env, props, "MasteringDisplayWhitePointY", 0, &err) };

        if (whitePointX && whitePointY)
        {
            d->src_pl_csp->hdr.prim.white.x = whitePointX;
            d->src_pl_csp->hdr.prim.white.y = whitePointY;
        }
    }
    else
        // Assume DCI-P3 D65 default?
        pl_raw_primaries_merge(&d->src_pl_csp->hdr.prim, pl_raw_primaries_get(PL_COLOR_PRIM_DISPLAY_P3));

    d->chromaLocation = static_cast<pl_chroma_location>(avs_prop_get_int(fi->env, props, "_ChromaLocation", 0, &err));
    if (!err)
        d->chromaLocation = static_cast<pl_chroma_location>(static_cast<int>(d->chromaLocation) + 1);

    // DOVI
    std::unique_ptr<pl_dovi_metadata> dovi_meta{ std::make_unique<pl_dovi_metadata>() };
    uint8_t dovi_profile{ 0 };

    if (d->use_dovi && avs_prop_num_elements(fi->env, props, "DolbyVisionRPU") > -1)
    {
        const uint8_t* doviRpu{ reinterpret_cast<const uint8_t*>(avs_prop_get_data(fi->env, props, "DolbyVisionRPU", 0, &err)) };
        size_t doviRpuSize{ static_cast<size_t>(avs_prop_get_data_size(fi->env, props, "DolbyVisionRPU", 0, &err)) };

        if (doviRpu && doviRpuSize)
        {
            // fprintf(stderr, "Got Dolby Vision RPU, size %"PRIi64" at %"PRIxPTR"\n", doviRpuSize, (uintptr_t) doviRpu);

            DoviRpuOpaque* rpu{ dovi_parse_unspec62_nalu(doviRpu, doviRpuSize) };
            const DoviRpuDataHeader* header{ dovi_rpu_get_header(rpu) };

            if (!header)
            {
                const std::string err{ std::string("libplacebo_Tonemap: failed parsing RPU: ") + std::string(dovi_rpu_get_error(rpu)) };
                d->msg = std::make_unique<char[]>(err.size() + 1);
                strcpy(d->msg.get(), err.c_str());
                ErrorText = d->msg.get();
            }
            else
            {
                dovi_profile = header->guessed_profile;

                dovi_meta = create_dovi_meta(*rpu, *header);
                dovi_rpu_free_header(header);
            }

            if (!ErrorText)
            {
                // Profile 5, 7 or 8 mapping
                if (d->src_csp == CSP_DOVI)
                {
                    d->src_repr->sys = PL_COLOR_SYSTEM_DOLBYVISION;
                    d->src_repr->dovi = dovi_meta.get();

                    if (dovi_profile == 5)
                        d->dst_repr->levels = PL_COLOR_LEVELS_FULL;
                }

                // Update mastering display from RPU
                if (header->vdr_dm_metadata_present_flag)
                {
                    const DoviVdrDmData* vdr_dm_data{ dovi_rpu_get_vdr_dm_data(rpu) };

                    // Should avoid changing the source black point when mapping to PQ
                    // As the source image already has a specific black point,
                    // and the RPU isn't necessarily ground truth on the actual coded values

                    // Set target black point to the same as source
                    if (d->src_csp == CSP_DOVI && d->dst_csp == CSP_HDR10)
                        d->dst_pl_csp->hdr.min_luma = d->src_pl_csp->hdr.min_luma;
                    else
                        d->src_pl_csp->hdr.min_luma = pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS, vdr_dm_data->source_min_pq / 4095.0f);

                    d->src_pl_csp->hdr.max_luma = pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS, vdr_dm_data->source_max_pq / 4095.0f);

                    if (vdr_dm_data->dm_data.level6)
                    {
                        const DoviExtMetadataBlockLevel6* meta{ vdr_dm_data->dm_data.level6 };

                        if (!maxCll || !maxFall)
                        {
                            d->src_pl_csp->hdr.max_cll = meta->max_content_light_level;
                            d->src_pl_csp->hdr.max_fall = meta->max_frame_average_light_level;
                        }
                    }

                    dovi_rpu_free_vdr_dm_data(vdr_dm_data);
                }
            }

            dovi_rpu_free(rpu);
        }
        else
            ErrorText = "libplacebo_Tonemap: DolbyVisionRPU frame property is required for src_csp=3!";
    }

    if (!ErrorText)
    {
        const int planes_y[3]{ AVS_PLANAR_Y, AVS_PLANAR_U, AVS_PLANAR_V };
        const int planes_r[3]{ AVS_PLANAR_R, AVS_PLANAR_G, AVS_PLANAR_B };
        const int* planes{ (avs_is_rgb(&fi->vi)) ? planes_r : planes_y };
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

            const int reconf{ tonemap_reconfig(*d->vf.get(), *pl) };
            if (reconf == 0)
            {
                const int filt{ tonemap_filter(*d->vf.get(), d->packed_dst, *pl, *d, *d->src_repr.get(), *d->dst_repr.get()) };

                if (filt)
                {
                    switch (filt)
                    {
                        case -1: ErrorText = "libplacebo_Tonemap: failed uploading data to the GPU!"; break;
                        case -2: ErrorText = "libplacebo_Tonemap: failed processing planes!"; break;
                        default: ErrorText = "libplacebo_Tonemap: failed downloading data from the GPU!";
                    }
                }
            }
            else
            {
                switch (reconf)
                {
                    case -1: ErrorText = "libplacebo_Tonemap: failed configuring filter: no good texture format!"; break;
                    default: ErrorText = "libplacebo_Tonemap: failed creating GPU textures!";
                }
            }
        }

        if (!ErrorText)
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

            AVS_Map* props{ avs_get_frame_props_rw(fi->env, dst) };
            avs_prop_set_int(fi->env, props, "_ColorRange", (d->dst_repr->levels = PL_COLOR_LEVELS_FULL) ? 0 : 1, 0);

            if (d->dst_pl_csp->transfer == PL_COLOR_TRC_BT_1886)
            {
                avs_prop_set_int(fi->env, props, "_Matrix", (d->dst_repr->sys == PL_COLOR_SYSTEM_RGB) ? 0 : 1, 0);
                avs_prop_set_int(fi->env, props, "_Transfer", 1, 0);
                avs_prop_set_int(fi->env, props, "_Primaries", 1, 0);
            }
            else if (d->dst_pl_csp->transfer == PL_COLOR_TRC_PQ)
            {
                avs_prop_set_int(fi->env, props, "_Matrix", (d->dst_repr->sys == PL_COLOR_SYSTEM_RGB) ? 0 : 9, 0);
                avs_prop_set_int(fi->env, props, "_Transfer", 16, 0);
                avs_prop_set_int(fi->env, props, "_Primaries", 9, 0);
            }
            else
            {
                avs_prop_set_int(fi->env, props, "_Matrix", (d->dst_repr->sys == PL_COLOR_SYSTEM_RGB) ? 0 : 9, 0);
                avs_prop_set_int(fi->env, props, "_Transfer", 18, 0);
                avs_prop_set_int(fi->env, props, "_Primaries", 9, 0);
            }

            if (avs_num_components(&fi->vi) > 3)
                avs_bit_blt(fi->env, avs_get_write_ptr_p(dst, AVS_PLANAR_A), avs_get_pitch_p(dst, AVS_PLANAR_A), avs_get_read_ptr_p(src, AVS_PLANAR_A), avs_get_pitch_p(src, AVS_PLANAR_A),
                    avs_get_row_size_p(src, AVS_PLANAR_A), avs_get_height_p(src, AVS_PLANAR_A));

            avs_release_video_frame(src);

            return dst;
        }
    }

    avs_release_video_frame(src);
    avs_release_video_frame(dst);

    fi->error = ErrorText;

    return nullptr;
}

static void AVSC_CC free_tonemap(AVS_FilterInfo* fi)
{
    tonemap* d{ reinterpret_cast<tonemap*>(fi->user_data) };

    operator delete (d->packed_dst);
    avs_libplacebo_uninit(std::move(d->vf));
    delete d;
}

static int AVSC_CC tonemap_set_cache_hints(AVS_FilterInfo* fi, int cachehints, int frame_range)
{
    return cachehints == AVS_CACHE_GET_MTMODE ? 2 : 0;
}

AVS_Value AVSC_CC create_tonemap(AVS_ScriptEnvironment* env, AVS_Value args, void* param)
{
    enum {
        Clip, Src_csp, Dst_csp, Src_max, Src_min, Dst_max, Dst_min, Dynamic_peak_detection, Smoothing_period, Scene_threshold_low, Scene_threshold_high, Intent, Gamut_mode, Tone_mapping_function, Tone_mapping_mode, Tone_mapping_param,
        Tone_mapping_crosstalk, Use_dovi, Device, List_device
    };

    AVS_FilterInfo* fi;
    AVS_Clip* clip{ avs_new_c_filter(env, &fi, avs_array_elt(args, Clip), 1) };
    AVS_Value v{ avs_void };

    tonemap* params{ new tonemap() };
    const int srcIsRGB{ avs_is_rgb(&fi->vi) };

    if (!avs_is_planar(&fi->vi))
        v = avs_new_value_error("libplacebo_Tonemap: clip must be in planar format.");
    if (!avs_defined(v) && avs_bits_per_component(&fi->vi) != 16)
        v = avs_new_value_error("libplacebo_Tonemap: bit depth must be 16-bit.");
    if (!avs_defined(v) && avs_num_components(&fi->vi) < 3)
        v = avs_new_value_error("libplacebo_Tonemap: the clip must have at least three planes.");
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
                v = avs_new_value_error("libplacebo_Tonemap: failed to create instance.");
                vkDestroyInstance(inst, nullptr);
            }
            if (!avs_defined(v))
            {
                if (vkEnumeratePhysicalDevices(inst, &dev_count, nullptr))
                    v = avs_new_value_error("libplacebo_Tonemap: failed to get devices number.");
            }
            if (!avs_defined(v))
            {
                if (device < -1 || device > static_cast<int>(dev_count) - 1)
                {
                    const std::string err_{ (std::string("libplacebo_Tonemap: device must be between -1 and ") + std::to_string(dev_count - 1)) };
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
                    v = avs_new_value_error("libplacebo_Tonemap: failed to get get devices.");
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
                    AVS_Clip* clip1{ avs_take_clip(inv, env) };

                    v = avs_new_value_clip(clip1);

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
            if (device == -1)
            {
                devices.resize(1);
                params->vf = avs_libplacebo_init(devices[0]);
            }
            else
                params->vf = avs_libplacebo_init(devices[device]);

            vkDestroyInstance(inst, nullptr);

            params->colorMapParams = std::make_unique<pl_color_map_params>(pl_color_map_default_params);

            // Tone mapping function
            int function_index{ avs_defined(avs_array_elt(args, Tone_mapping_function)) ? avs_as_int(avs_array_elt(args, Tone_mapping_function)) : 0 };

            if (function_index >= pl_num_tone_map_functions)
                function_index = 0;

            params->colorMapParams->tone_mapping_function = pl_tone_map_functions[function_index];

            params->colorMapParams->tone_mapping_param = avs_defined(avs_array_elt(args, Tone_mapping_param)) ? avs_as_float(avs_array_elt(args, Tone_mapping_param)) : params->colorMapParams->tone_mapping_function->param_def;

            if (avs_defined(avs_array_elt(args, Intent)))
                params->colorMapParams->intent = static_cast<pl_rendering_intent>(avs_as_int(avs_array_elt(args, Intent)));
            if (avs_defined(avs_array_elt(args, Gamut_mode)))
                params->colorMapParams->gamut_mode = static_cast<pl_gamut_mode>(avs_as_int(avs_array_elt(args, Gamut_mode)));
            if (avs_defined(avs_array_elt(args, Tone_mapping_mode)))
                params->colorMapParams->tone_mapping_mode = static_cast<pl_tone_map_mode>(avs_as_int(avs_array_elt(args, Tone_mapping_mode)));
            if (avs_defined(avs_array_elt(args, Tone_mapping_crosstalk)))
                params->colorMapParams->tone_mapping_crosstalk = avs_as_float(avs_array_elt(args, Tone_mapping_crosstalk));

            params->peakDetectParams = std::make_unique<pl_peak_detect_params>(pl_peak_detect_default_params);
            if (avs_defined(avs_array_elt(args, Smoothing_period)))
                params->peakDetectParams->smoothing_period = avs_as_float(avs_array_elt(args, Smoothing_period));
            if (avs_defined(avs_array_elt(args, Scene_threshold_low)))
                params->peakDetectParams->scene_threshold_low = avs_as_float(avs_array_elt(args, Scene_threshold_low));
            if (avs_defined(avs_array_elt(args, Scene_threshold_high)))
                params->peakDetectParams->scene_threshold_high = avs_as_float(avs_array_elt(args, Scene_threshold_high));

            params->src_csp = static_cast<supported_colorspace>(avs_defined(avs_array_elt(args, Src_csp)) ? avs_as_int(avs_array_elt(args, Src_csp)) : 1);

            if (params->src_csp == CSP_DOVI && srcIsRGB)
                v = avs_new_value_error("libplacebo_Tonemap: Dolby Vision source colorspace must be a YUV clip!");
        }
    }
    if (!avs_defined(v))
    {
        params->src_pl_csp = std::make_unique<pl_color_space>();

        switch (params->src_csp)
        {
            case CSP_HDR10:
            case CSP_DOVI: *params->src_pl_csp.get() = pl_color_space_hdr10; break;
            case CSP_HLG: *params->src_pl_csp.get() = pl_color_space_bt2020_hlg; break;
            default: v = avs_new_value_error("libplacebo_Tonemap: Invalid source colorspace for tonemapping.");
        }
    }
    if (!avs_defined(v))
    {
        params->dst_pl_csp = std::make_unique<pl_color_space>();
        params->dst_csp = static_cast<supported_colorspace>(avs_defined(avs_array_elt(args, Dst_csp)) ? avs_as_int(avs_array_elt(args, Dst_csp)) : 0);

        switch (params->dst_csp)
        {
            case CSP_SDR: *params->dst_pl_csp.get() = pl_color_space_bt709; break;
            case CSP_HDR10: *params->dst_pl_csp.get() = pl_color_space_hdr10; break;
            case CSP_HLG: *params->dst_pl_csp.get() = pl_color_space_bt2020_hlg; break;
            default: v = avs_new_value_error("libplacebo_Tonemap: Invalid target colorspace for tonemapping.");
        }
    }
    if (!avs_defined(v))
    {
        if (avs_defined(avs_array_elt(args, Src_max)))
        {
            params->original_src_max = avs_as_float(avs_array_elt(args, Src_max));
            params->src_pl_csp->hdr.max_luma = params->original_src_max;
        }
        if (avs_defined(avs_array_elt(args, Src_min)))
        {
            params->original_src_min = avs_as_float(avs_array_elt(args, Src_min));
            params->src_pl_csp->hdr.min_luma = params->original_src_min;
        }

        pl_color_space_infer(params->src_pl_csp.get());

        if (avs_defined(avs_array_elt(args, Dst_max)))
        {
            params->dst_pl_csp->hdr.max_luma = avs_as_float(avs_array_elt(args, Dst_max));
            params->dst_pl_csp->hdr.min_luma = avs_defined(avs_array_elt(args, Dst_min)) ? avs_as_float(avs_array_elt(args, Dst_min)) : 0;
        }

        pl_color_space_infer(params->dst_pl_csp.get());

        const int peak_detection{ avs_defined(avs_array_elt(args, Dynamic_peak_detection)) ? avs_as_bool(avs_array_elt(args, Dynamic_peak_detection)) : 1 };
        params->use_dovi = avs_defined(avs_array_elt(args, Use_dovi)) ? avs_as_bool(avs_array_elt(args, Use_dovi)) : params->src_csp == CSP_DOVI;

        params->render_params = std::make_unique<pl_render_params>(pl_render_default_params);
        params->render_params->color_map_params = params->colorMapParams.get();
        params->render_params->peak_detect_params = (peak_detection) ? params->peakDetectParams.get() : nullptr;
        params->render_params->sigmoid_params = &pl_sigmoid_default_params;
        params->render_params->dither_params = &pl_dither_default_params;
        params->render_params->cone_params = nullptr;
        params->render_params->color_adjustment = nullptr;
        params->render_params->deband_params = nullptr;

        if (srcIsRGB)
            params->is_subsampled = 0;
        else
            params->is_subsampled = avs_get_plane_width_subsampling(&fi->vi, AVS_PLANAR_U) | avs_get_plane_height_subsampling(&fi->vi, AVS_PLANAR_U);

        params->src_repr = std::make_unique<pl_color_repr>();
        params->src_repr->bits.sample_depth = 16;
        params->src_repr->bits.color_depth = 16;
        params->src_repr->bits.bit_shift = 0;

        params->dst_repr = std::make_unique< pl_color_repr>();
        params->dst_repr->bits.sample_depth = 16;
        params->dst_repr->bits.color_depth = 16;
        params->dst_repr->bits.bit_shift = 0;
        params->dst_repr->alpha = PL_ALPHA_PREMULTIPLIED;

        if (!srcIsRGB)
        {
            params->src_repr->sys = PL_COLOR_SYSTEM_BT_2020_NC;
            params->dst_repr->levels = PL_COLOR_LEVELS_LIMITED;

            if (params->dst_pl_csp->transfer == PL_COLOR_TRC_BT_1886)
                params->dst_repr->sys = PL_COLOR_SYSTEM_BT_709;
            else if (params->dst_pl_csp->transfer == PL_COLOR_TRC_PQ || params->dst_pl_csp->transfer == PL_COLOR_TRC_HLG)
                params->dst_repr->sys = PL_COLOR_SYSTEM_BT_2020_NC;

            fi->vi.pixel_type = AVS_CS_YUV444P16;
        }
        else
        {
            params->src_repr->sys = PL_COLOR_SYSTEM_RGB;

            params->dst_repr->levels = PL_COLOR_LEVELS_FULL;
            params->dst_repr->sys = PL_COLOR_SYSTEM_RGB;
        }

        params->packed_dst = operator new(fi->vi.width * fi->vi.height * 2 * 3);

        v = avs_new_value_clip(clip);

        fi->user_data = reinterpret_cast<void*>(params);
        fi->get_frame = tonemap_get_frame;
        fi->set_cache_hints = tonemap_set_cache_hints;
        fi->free_filter = free_tonemap;
    }

    avs_release_clip(clip);

    return v;
}
