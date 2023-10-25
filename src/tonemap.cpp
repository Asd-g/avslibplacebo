#include <algorithm>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#endif

#include "avs_libplacebo.h"

extern "C"
#include "libdovi/rpu_parser.h"

static std::mutex mtx;

static std::unique_ptr<pl_dovi_metadata> create_dovi_meta(DoviRpuOpaque& rpu, const DoviRpuDataHeader& hdr)
{
    std::unique_ptr<pl_dovi_metadata> dovi_meta{ std::make_unique<pl_dovi_metadata>() }; // persist state
    if (hdr.use_prev_vdr_rpu_flag)
        goto done;

    {
        const DoviRpuDataMapping* mapping = dovi_rpu_get_data_mapping(&rpu);
        if (!mapping)
            goto skip_mapping;

        {
            const uint64_t bits{ hdr.bl_bit_depth_minus8 + 8 };
            const float scale{ 1.0f / (1 << hdr.coefficient_log2_denom) };

            for (int c{ 0 }; c < 3; ++c)
            {
                const DoviReshapingCurve curve{ mapping->curves[c] };

                pl_dovi_metadata::pl_reshape_data* cmp{ &dovi_meta->comp[c] };
                cmp->num_pivots = curve.pivots.len;
                memset(cmp->method, curve.mapping_idc, sizeof(cmp->method));

                uint16_t pivot{ 0 };
                for (int pivot_idx{ 0 }; pivot_idx < cmp->num_pivots; ++pivot_idx)
                {
                    pivot += curve.pivots.data[pivot_idx];
                    cmp->pivots[pivot_idx] = static_cast<float>(pivot) / ((1 << bits) - 1);
                }

                for (int i{ 0 }; i < cmp->num_pivots - 1; ++i)
                {
                    memset(cmp->poly_coeffs[i], 0, sizeof(cmp->poly_coeffs[i]));

                    if (curve.polynomial)
                    {
                        const DoviPolynomialCurve* poly_curve = curve.polynomial;

                        for (int k{ 0 }; k <= poly_curve->poly_order_minus1.data[i] + 1; ++k)
                        {
                            int64_t ipart{ poly_curve->poly_coef_int.list[i]->data[k] };
                            uint64_t fpart{ poly_curve->poly_coef.list[i]->data[k] };
                            cmp->poly_coeffs[i][k] = ipart + scale * fpart;
                        }
                    }
                    else if (curve.mmr)
                    {
                        const DoviMMRCurve* mmr_curve = curve.mmr;

                        int64_t ipart{ mmr_curve->mmr_constant_int.data[i] };
                        uint64_t fpart{ mmr_curve->mmr_constant.data[i] };
                        cmp->mmr_constant[i] = ipart + scale * fpart;
                        cmp->mmr_order[i] = mmr_curve->mmr_order_minus1.data[i] + 1;

                        for (int j{ 0 }; j < cmp->mmr_order[i]; ++j)
                        {
                            for (int k{ 0 }; k < 7; ++k)
                            {
                                ipart = mmr_curve->mmr_coef_int.list[i]->list[j]->data[k];
                                fpart = mmr_curve->mmr_coef.list[i]->list[j]->data[k];
                                cmp->mmr_coeffs[i][j][k] = ipart + scale * fpart;
                            }
                        }
                    }
                }
            }
        }

        dovi_rpu_free_data_mapping(mapping);
    }
skip_mapping:

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
            dst[i] = src[i] / 8192.0f;

        src = &dm_data->rgb_to_lms_coef0;
        dst = &dovi_meta->linear.m[0][0];
        for (int i{ 0 }; i < 9; ++i)
            dst[i] = src[i] / 16384.0f;

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
    float original_src_max;
    float original_src_min;
    int is_subsampled;
    enum pl_chroma_location chromaLocation;
    std::string msg;
    std::unique_ptr<pl_color_repr> src_repr;
    std::unique_ptr<pl_color_repr> dst_repr;
    int use_dovi;
    std::unique_ptr<pl_color_map_params> colorMapParams;
    std::unique_ptr<pl_peak_detect_params> peakDetectParams;
    std::unique_ptr<pl_dovi_metadata> dovi_meta;
};

static bool tonemap_do_plane(priv& p, const tonemap& data, const pl_plane* planes, const pl_color_repr& src_repr, const pl_color_repr& dst_repr) noexcept
{
    pl_frame img{};
    img.num_planes = 3;
    img.repr = src_repr;
    img.planes[0] = planes[0];
    img.planes[1] = planes[1];
    img.planes[2] = planes[2];
    img.color = *data.src_pl_csp;

    if (data.is_subsampled)
        pl_frame_set_chroma_location(&img, data.chromaLocation);

    pl_frame out{};
    out.num_planes = 3;
    out.repr = dst_repr;
    out.color = *data.dst_pl_csp;

    for (int i{ 0 }; i < 3; ++i)
    {
        out.planes[i].texture = p.tex_out[i];
        out.planes[i].components = 1;
        out.planes[i].component_mapping[0] = i;
    }

    return pl_render_image(p.rr, &img, &out, data.render_params.get());
}

static int tonemap_reconfig(priv& p, const pl_plane_data* data)
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

        pl_plane_data data1{ data[i] };
        data1.width = data[0].width;
        data1.height = data[0].height;

        const pl_fmt out{ pl_plane_find_fmt(p.gpu, nullptr, &data1) };

        t_r.w = data->width;
        t_r.h = data->height;
        t_r.format = out;
        t_r.sampleable = false;
        t_r.host_writable = false;
        t_r.renderable = true;
        t_r.host_readable = true;

        if (!pl_tex_recreate(p.gpu, &p.tex_out[i], &t_r))
            return -2;
    }

    return 0;
}

static int tonemap_filter(priv& p, const pl_buf* dst, const pl_plane_data* src, const tonemap& d, const pl_color_repr& src_repr, const pl_color_repr& dst_repr, const int dst_stride)
{
    // Upload planes
    pl_plane planes[4]{};

    for (int i{ 0 }; i < 3; ++i)
    {
        if (!pl_upload_plane(p.gpu, &planes[i], &p.tex_in[i], &src[i]))
            return -1;
    }

    // Process plane
    if (!tonemap_do_plane(p, d, planes, src_repr, dst_repr))
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

static AVS_VideoFrame* AVSC_CC tonemap_get_frame(AVS_FilterInfo* fi, int n)
{
    tonemap* d{ reinterpret_cast<tonemap*>(fi->user_data) };

    AVS_VideoFrame* src{ avs_get_frame(fi->child, n) };
    if (!src)
        return nullptr;

    AVS_VideoFrame* dst{ avs_new_video_frame_p(fi->env, &fi->vi, src) };

    const auto error{ [&](const std::string& msg, pl_buf* dst_buf)
    {
        avs_release_video_frame(src);
        avs_release_video_frame(dst);

        if (dst_buf)
        {
            for (int i{ 0 }; i < 3; ++i)
                pl_buf_destroy(d->vf->gpu, &dst_buf[i]);
        }

        d->msg = msg;
        fi->error = d->msg.c_str();

        return nullptr;
    }
    };

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
    if (d->src_csp == CSP_DOVI)
    {
        if (avs_prop_num_elements(fi->env, props, "DolbyVisionRPU") > -1)
        {
            if (d->use_dovi)
            {
                uint8_t dovi_profile{ 0 };
                const uint8_t* doviRpu{ reinterpret_cast<const uint8_t*>(avs_prop_get_data(fi->env, props, "DolbyVisionRPU", 0, &err)) };
                size_t doviRpuSize{ static_cast<size_t>(avs_prop_get_data_size(fi->env, props, "DolbyVisionRPU", 0, &err)) };

                if (doviRpu && doviRpuSize)
                {
                    DoviRpuOpaque* rpu{ dovi_parse_unspec62_nalu(doviRpu, doviRpuSize) };
                    const DoviRpuDataHeader* header{ dovi_rpu_get_header(rpu) };

                    if (!header)
                    {
                        std::string err{ dovi_rpu_get_error(rpu) };
                        dovi_rpu_free(rpu);
                        return error("libplacebo_Tonemap: failed parsing RPU: " + err, nullptr);
                    }
                    else
                    {
                        dovi_profile = header->guessed_profile;
                        d->dovi_meta = create_dovi_meta(*rpu, *header);
                        dovi_rpu_free_header(header);
                    }

                    // Profile 5, 7 or 8 mapping
                    d->src_repr->sys = PL_COLOR_SYSTEM_DOLBYVISION;
                    d->src_repr->dovi = d->dovi_meta.get();

                    if (dovi_profile == 5)
                        d->dst_repr->levels = PL_COLOR_LEVELS_FULL;

                    // Update mastering display from RPU
                    if (header->vdr_dm_metadata_present_flag)
                    {
                        const DoviVdrDmData* vdr_dm_data{ dovi_rpu_get_vdr_dm_data(rpu) };

                        // Should avoid changing the source black point when mapping to PQ
                        // As the source image already has a specific black point,
                        // and the RPU isn't necessarily ground truth on the actual coded values

                        // Set target black point to the same as source
                        if (d->dst_csp == CSP_HDR10)
                            d->dst_pl_csp->hdr.min_luma = d->src_pl_csp->hdr.min_luma;
                        else
                            d->src_pl_csp->hdr.min_luma = pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS, vdr_dm_data->source_min_pq / 4095.0f);

                        d->src_pl_csp->hdr.max_luma = pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS, vdr_dm_data->source_max_pq / 4095.0f);

                        if (vdr_dm_data->dm_data.level1)
                        {
                            const DoviExtMetadataBlockLevel1* l1{ vdr_dm_data->dm_data.level1 };
                            d->src_pl_csp->hdr.avg_pq_y = l1->avg_pq / 4095.0f;
                            d->src_pl_csp->hdr.max_pq_y = l1->max_pq / 4095.0f;
                            d->src_pl_csp->hdr.scene_avg = pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS, l1->avg_pq / 4095.0f);
                            d->src_pl_csp->hdr.scene_max[0] = d->src_pl_csp->hdr.scene_max[1] = d->src_pl_csp->hdr.scene_max[2] = pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS, l1->max_pq / 4095.0f);
                        }

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

                    dovi_rpu_free(rpu);
                }
                else
                    return error("libplacebo_Tonemap: invlid DolbyVisionRPU frame property!", nullptr);
            }
        }
        else
            return error("libplacebo_Tonemap: DolbyVisionRPU frame property is required for src_csp=3!", nullptr);
    }

    pl_color_space_infer_map(d->src_pl_csp.get(), d->dst_pl_csp.get());

    constexpr int planes_y[3]{ AVS_PLANAR_Y, AVS_PLANAR_U, AVS_PLANAR_V };
    constexpr int planes_r[3]{ AVS_PLANAR_R, AVS_PLANAR_G, AVS_PLANAR_B };
    const int* planes{ (avs_is_rgb(&fi->vi)) ? planes_r : planes_y };
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

        const int reconf{ tonemap_reconfig(*d->vf.get(), pl) };
        if (reconf == 0)
        {
            const int filt{ tonemap_filter(*d->vf.get(), dst_buf, pl, *d, *d->src_repr.get(), *d->dst_repr.get(), dst_stride) };

            if (filt)
            {
                switch (filt)
                {
                    case -1: return error("libplacebo_Tonemap: failed uploading data to the GPU!", dst_buf);
                    case -2: return error("libplacebo_Tonemap: failed processing planes!", dst_buf);
                    default: return error("libplacebo_Tonemap: failed downloading data from the GPU!", dst_buf);
                }
            }
        }
        else
        {
            switch (reconf)
            {
                case -1: return error("libplacebo_Tonemap: failed configuring filter: no good texture format!", dst_buf);
                default: return error("libplacebo_Tonemap: failed creating GPU textures!", dst_buf);
            }
        }
    }

    for (int i{ 0 }; i < 3; ++i)
    {
        while (pl_buf_poll(d->vf->gpu, dst_buf[i], 0));
        memcpy(avs_get_write_ptr_p(dst, planes[i]), dst_buf[i]->data, dst_buf[i]->params.size);
        pl_buf_destroy(d->vf->gpu, &dst_buf[i]);
    }

    AVS_Map* dst_props{ avs_get_frame_props_rw(fi->env, dst) };
    avs_prop_set_int(fi->env, dst_props, "_ColorRange", (d->dst_repr->levels == PL_COLOR_LEVELS_FULL) ? 0 : 1, 0);

    if (d->dst_pl_csp->transfer == PL_COLOR_TRC_BT_1886)
    {
        avs_prop_set_int(fi->env, dst_props, "_Matrix", (d->dst_repr->sys == PL_COLOR_SYSTEM_RGB) ? 0 : 1, 0);
        avs_prop_set_int(fi->env, dst_props, "_Transfer", 1, 0);
        avs_prop_set_int(fi->env, dst_props, "_Primaries", 1, 0);

        avs_prop_delete_key(fi->env, dst_props, "ContentLightLevelMax");
        avs_prop_delete_key(fi->env, dst_props, "ContentLightLevelAverage");
        avs_prop_delete_key(fi->env, dst_props, "MasteringDisplayMaxLuminance");
        avs_prop_delete_key(fi->env, dst_props, "MasteringDisplayMinLuminance");
        avs_prop_delete_key(fi->env, dst_props, "MasteringDisplayPrimariesX");
        avs_prop_delete_key(fi->env, dst_props, "MasteringDisplayPrimariesY");
        avs_prop_delete_key(fi->env, dst_props, "MasteringDisplayWhitePointX");
        avs_prop_delete_key(fi->env, dst_props, "MasteringDisplayWhitePointY");
    }
    else if (d->dst_pl_csp->transfer == PL_COLOR_TRC_PQ)
    {
        avs_prop_set_int(fi->env, dst_props, "_Matrix", (d->dst_repr->sys == PL_COLOR_SYSTEM_RGB) ? 0 : 9, 0);
        avs_prop_set_int(fi->env, dst_props, "_Transfer", 16, 0);
        avs_prop_set_int(fi->env, dst_props, "_Primaries", 9, 0);
    }
    else
    {
        avs_prop_set_int(fi->env, dst_props, "_Matrix", (d->dst_repr->sys == PL_COLOR_SYSTEM_RGB) ? 0 : 9, 0);
        avs_prop_set_int(fi->env, dst_props, "_Transfer", 18, 0);
        avs_prop_set_int(fi->env, dst_props, "_Primaries", 9, 0);
    }

    if (avs_num_components(&fi->vi) > 3)
        avs_bit_blt(fi->env, avs_get_write_ptr_p(dst, AVS_PLANAR_A), avs_get_pitch_p(dst, AVS_PLANAR_A), avs_get_read_ptr_p(src, AVS_PLANAR_A), avs_get_pitch_p(src, AVS_PLANAR_A),
            avs_get_row_size_p(src, AVS_PLANAR_A), avs_get_height_p(src, AVS_PLANAR_A));

    avs_release_video_frame(src);

    return dst;
}

static void AVSC_CC free_tonemap(AVS_FilterInfo* fi)
{
    tonemap* d{ reinterpret_cast<tonemap*>(fi->user_data) };

    if (d->render_params->lut)
        pl_lut_free(const_cast<pl_custom_lut**>(&d->render_params->lut));

    avs_libplacebo_uninit(std::move(d->vf));
    delete d;
}

static int AVSC_CC tonemap_set_cache_hints(AVS_FilterInfo* fi, int cachehints, int frame_range)
{
    return cachehints == AVS_CACHE_GET_MTMODE ? 2 : 0;
}

AVS_Value AVSC_CC create_tonemap(AVS_ScriptEnvironment* env, AVS_Value args, void* param)
{
    enum
    {
        Clip, Src_csp, Dst_csp, Src_max, Src_min, Dst_max, Dst_min, Dynamic_peak_detection, Smoothing_period, Scene_threshold_low, Scene_threshold_high, Percentile, Gamut_mapping_mode, Tone_mapping_function, Tone_mapping_mode, Tone_mapping_param,
        Tone_mapping_crosstalk, Metadata, Contrast_recovery, Contrast_smoothness, Visualize_lut, Show_clipping, Use_dovi, Device, List_device, Cscale, Lut, Lut_type, Dst_prim, Dst_trc, Dst_sys
    };

    AVS_FilterInfo* fi;
    AVS_Clip* clip{ avs_new_c_filter(env, &fi, avs_array_elt(args, Clip), 1) };

    tonemap* params{ new tonemap() };
    const int srcIsRGB{ avs_is_rgb(&fi->vi) };

    AVS_Value avs_ver{ avs_version("libplacebo_Tonemap", env) };
    if (avs_is_error(avs_ver))
        return avs_ver;

    if (!avs_is_planar(&fi->vi))
        return set_error(clip, "libplacebo_Tonemap: clip must be in planar format.");
    if (avs_bits_per_component(&fi->vi) != 16)
        return set_error(clip, "libplacebo_Tonemap: bit depth must be 16-bit.");
    if (avs_num_components(&fi->vi) < 3)
        return set_error(clip, "libplacebo_Tonemap: the clip must have at least three planes.");

    const int device{ avs_defined(avs_array_elt(args, Device)) ? avs_as_int(avs_array_elt(args, Device)) : -1 };
    const int list_device{ avs_defined(avs_array_elt(args, List_device)) ? avs_as_bool(avs_array_elt(args, List_device)) : 0 };

    if (list_device || device > -1)
    {
        std::vector<VkPhysicalDevice> devices{};
        VkInstance inst{};

        AVS_Value dev_info{ devices_info(clip, fi->env, devices, inst, params->msg, "libplacebo_Tonemap", device, list_device) };
        if (avs_is_error(dev_info) || avs_is_clip(dev_info))
            return dev_info;

        params->vf = avs_libplacebo_init(devices[device]);

        vkDestroyInstance(inst, nullptr);
    }
    else
    {
        if (device < -1)
            return set_error(clip, "libplacebo_Tonemap: device must be greater than or equal to -1.");

        params->vf = avs_libplacebo_init(nullptr);
    }

    params->src_pl_csp = std::make_unique<pl_color_space>();
    params->src_csp = static_cast<supported_colorspace>(avs_defined(avs_array_elt(args, Src_csp)) ? avs_as_int(avs_array_elt(args, Src_csp)) : 1);
    if (params->src_csp == CSP_DOVI && srcIsRGB)
        return set_error(clip, "libplacebo_Tonemap: Dolby Vision source colorspace must be a YUV clip!");

    switch (params->src_csp)
    {
        case CSP_HDR10:
        case CSP_DOVI: *params->src_pl_csp.get() = pl_color_space_hdr10; break;
        case CSP_HLG: *params->src_pl_csp.get() = pl_color_space_bt2020_hlg; break;
        default: return set_error(clip, "libplacebo_Tonemap: Invalid source colorspace for tonemapping.");
    }

    params->dst_pl_csp = std::make_unique<pl_color_space>();

    const int dst_prim_defined{ avs_defined(avs_array_elt(args, Dst_prim)) };
    const int dst_trc_defined{ avs_defined(avs_array_elt(args, Dst_trc)) };
    if (dst_prim_defined && dst_trc_defined)
    {
        const int dst_prim{ avs_as_int(avs_array_elt(args, Dst_prim)) };
        const int dst_trc{ avs_as_int(avs_array_elt(args, Dst_trc)) };

        if (dst_prim < 1 || dst_prim > 17)
            return set_error(clip, "libplacebo_Tonemap: dst_prim must be between 1 and 17.");
        if (dst_trc < 1 || dst_trc > 16)
            return set_error(clip, "libplacebo_Tonemap: dst_trc must be between 1 and 16.");

        params->dst_pl_csp->primaries = static_cast<pl_color_primaries>(dst_prim);
        params->dst_pl_csp->transfer = static_cast<pl_color_transfer>(dst_trc);
    }
    else if (!dst_prim_defined && !dst_trc_defined)
    {
        params->dst_csp = static_cast<supported_colorspace>(avs_defined(avs_array_elt(args, Dst_csp)) ? avs_as_int(avs_array_elt(args, Dst_csp)) : 0);
        switch (params->dst_csp)
        {
            case CSP_SDR: *params->dst_pl_csp.get() = pl_color_space_bt709; break;
            case CSP_HDR10: *params->dst_pl_csp.get() = pl_color_space_hdr10; break;
            case CSP_HLG: *params->dst_pl_csp.get() = pl_color_space_bt2020_hlg; break;
            default: return set_error(clip, "libplacebo_Tonemap: Invalid target colorspace for tonemapping.");
        }
    }
    else
        return set_error(clip, "libplacebo_Tonemap: dst_prim/dst_trc must be defined.");

    const int lut_defined{ avs_defined(avs_array_elt(args, Lut)) };

    if (lut_defined)
    {
        params->render_params = std::make_unique<pl_render_params>();

        const char* lut_path{ avs_as_string(avs_array_elt(args, Lut)) };
        FILE* lut_file{ nullptr };

#ifdef _WIN32
        const int required_size{ MultiByteToWideChar(CP_UTF8, 0, lut_path, -1, nullptr, 0) };
        std::unique_ptr<wchar_t[]> wbuffer{ std::make_unique<wchar_t[]>(required_size) };
        MultiByteToWideChar(CP_UTF8, 0, lut_path, -1, wbuffer.get(), required_size);
        lut_file = _wfopen(wbuffer.get(), L"rb");
#else
        lut_file = std::fopen(lut_path, "rb");
#endif
        if (!lut_file)
        {
            params->msg = "libplacebo_Tonemap: error opening file " + std::string(lut_path) + " (" + std::strerror(errno) + ")";
            return set_error(clip, params->msg.c_str());
        }
        if (std::fseek(lut_file, 0, SEEK_END))
        {
            std::fclose(lut_file);
            params->msg = "libplacebo_Tonemap: error seeking to the end of file " + std::string(lut_path) + " (" + std::strerror(errno) + ")";
            return set_error(clip, params->msg.c_str());
    }
        const long lut_size{ std::ftell(lut_file) };
        if (lut_size == -1)
        {
            std::fclose(lut_file);
            params->msg = "libplacebo_Tonemap: error determining the size of file " + std::string(lut_path) + " (" + std::strerror(errno) + ")";
            return set_error(clip, params->msg.c_str());
        }

        std::rewind(lut_file);

        std::string bdata;
        bdata.resize(lut_size);
        std::fread(bdata.data(), 1, lut_size, lut_file);
        bdata[lut_size] = '\0';

        std::fclose(lut_file);

        params->render_params->lut = pl_lut_parse_cube(nullptr, bdata.c_str(), bdata.size());
        if (!params->render_params->lut)
            return set_error(clip, "libplacebo_Tonemap: failed lut parsing.");

        const int lut_type{ (avs_defined(avs_array_elt(args, Lut_type))) ? avs_as_int(avs_array_elt(args, Lut_type)) : 3 };
        if (lut_type < 1 || lut_type > 3)
            return set_error(clip, "libplacebo_Tonemap: lut_type must be between 1 and 3.");

        params->render_params->lut_type = static_cast<pl_lut_type>(lut_type);
}
    else
    {
        if (params->src_csp == CSP_DOVI)
            params->dovi_meta = std::make_unique<pl_dovi_metadata>();

        params->colorMapParams = std::make_unique<pl_color_map_params>(pl_color_map_default_params);

        // Tone mapping function
        int function_index{ avs_defined(avs_array_elt(args, Tone_mapping_function)) ? avs_as_int(avs_array_elt(args, Tone_mapping_function)) : 0 };

        if (function_index >= pl_num_tone_map_functions)
            function_index = 0;

        params->colorMapParams->tone_mapping_function = pl_tone_map_functions[function_index];

        params->colorMapParams->tone_mapping_param = (avs_defined(avs_array_elt(args, Tone_mapping_param))) ? avs_as_float(avs_array_elt(args, Tone_mapping_param)) : params->colorMapParams->tone_mapping_function->param_def;

        if (avs_defined(avs_array_elt(args, Gamut_mapping_mode)))
        {
            const int gamut_mapping{ avs_as_int(avs_array_elt(args, Gamut_mapping_mode)) };
            switch (gamut_mapping)
            {
                case 0: break;
                case 1: params->colorMapParams->gamut_mapping = &pl_gamut_map_clip; break;
                case 2: params->colorMapParams->gamut_mapping = &pl_gamut_map_perceptual; break;
                case 3: params->colorMapParams->gamut_mapping = &pl_gamut_map_relative; break;
                case 4: params->colorMapParams->gamut_mapping = &pl_gamut_map_saturation; break;
                case 5: params->colorMapParams->gamut_mapping = &pl_gamut_map_absolute; break;
                case 6: params->colorMapParams->gamut_mapping = &pl_gamut_map_desaturate; break;
                case 7: params->colorMapParams->gamut_mapping = &pl_gamut_map_darken; break;
                case 8: params->colorMapParams->gamut_mapping = &pl_gamut_map_highlight; break;
                case 9: params->colorMapParams->gamut_mapping = &pl_gamut_map_linear; break;
                default: return set_error(clip, "libplacebo_Tonemap: wrong gamut_mapping_mode.");
            }
        }

        if (avs_defined(avs_array_elt(args, Tone_mapping_mode)))
            params->colorMapParams->tone_mapping_mode = static_cast<pl_tone_map_mode>(avs_as_int(avs_array_elt(args, Tone_mapping_mode)));
        if (avs_defined(avs_array_elt(args, Tone_mapping_crosstalk)))
            params->colorMapParams->tone_mapping_crosstalk = avs_as_float(avs_array_elt(args, Tone_mapping_crosstalk));
        if (avs_defined(avs_array_elt(args, Metadata)))
            params->colorMapParams->metadata = static_cast<pl_hdr_metadata_type>(avs_as_int(avs_array_elt(args, Metadata)));
        if (avs_defined(avs_array_elt(args, Visualize_lut)))
            params->colorMapParams->visualize_lut = avs_as_bool(avs_array_elt(args, Visualize_lut));
        if (avs_defined(avs_array_elt(args, Show_clipping)))
            params->colorMapParams->show_clipping = avs_as_bool(avs_array_elt(args, Show_clipping));

        params->colorMapParams->contrast_recovery = (avs_defined(avs_array_elt(args, Contrast_recovery))) ? avs_as_float(avs_array_elt(args, Contrast_recovery)) : 0.30f;
        params->colorMapParams->contrast_smoothness = (avs_defined(avs_array_elt(args, Contrast_smoothness))) ? avs_as_float(avs_array_elt(args, Contrast_smoothness)) : 3.5f;

        if (params->colorMapParams->contrast_recovery < 0.0f)
            return set_error(clip, "libplacebo_Tonemap: contrast_recovery must be equal to or greater than 0.0f.");
        if (params->colorMapParams->contrast_smoothness < 0.0f)
            return set_error(clip, "libplacebo_Tonemap: contrast_smoothness must be equal to or greater than 0.0f.");

        params->peakDetectParams = std::make_unique<pl_peak_detect_params>(pl_peak_detect_default_params);
        if (avs_defined(avs_array_elt(args, Smoothing_period)))
            params->peakDetectParams->smoothing_period = avs_as_float(avs_array_elt(args, Smoothing_period));
        if (avs_defined(avs_array_elt(args, Scene_threshold_low)))
            params->peakDetectParams->scene_threshold_low = avs_as_float(avs_array_elt(args, Scene_threshold_low));
        if (avs_defined(avs_array_elt(args, Scene_threshold_high)))
            params->peakDetectParams->scene_threshold_high = avs_as_float(avs_array_elt(args, Scene_threshold_high));
        if (avs_defined(avs_array_elt(args, Percentile)))
            params->peakDetectParams->percentile = avs_as_float(avs_array_elt(args, Percentile));

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

        if (avs_defined(avs_array_elt(args, Dst_max)))
            params->dst_pl_csp->hdr.max_luma = avs_as_float(avs_array_elt(args, Dst_max));
        if (avs_defined(avs_array_elt(args, Dst_min)))
            params->dst_pl_csp->hdr.min_luma = avs_as_float(avs_array_elt(args, Dst_min));

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
    }

    const char* cscale{ (avs_defined(avs_array_elt(args, Cscale))) ? avs_as_string(avs_array_elt(args, Cscale)) : "spline36" };
    const pl_filter_preset* cscaler{ pl_find_filter_preset(cscale) };
    if (!cscaler)
    {
        if (lut_defined)
            pl_lut_free(const_cast<pl_custom_lut**>(&params->render_params->lut));

        return set_error(clip, "libplacebo_Tonemap: not a valid cscale.");
    }

    params->render_params->plane_upscaler = cscaler->filter;

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

        if (dst_prim_defined && avs_defined(avs_array_elt(args, Dst_sys)))
        {
            const int dst_sys{ avs_as_int(avs_array_elt(args, Dst_sys)) };
            if (dst_sys < 1 || dst_sys > 9)
            {
                if (lut_defined)
                    pl_lut_free(const_cast<pl_custom_lut**>(&params->render_params->lut));

                return set_error(clip, "libplacebo_Tonemap: dst_sys must be between 1 and 9.");
            }

            params->dst_repr->sys = static_cast<pl_color_system>(dst_sys);
        }
        else
        {
            switch (params->dst_pl_csp->transfer)
            {
                case PL_COLOR_TRC_BT_1886:
                case PL_COLOR_TRC_SRGB:
                case PL_COLOR_TRC_LINEAR:
                case PL_COLOR_TRC_GAMMA18:
                case PL_COLOR_TRC_GAMMA20:
                case PL_COLOR_TRC_GAMMA22:
                case PL_COLOR_TRC_GAMMA24:
                case PL_COLOR_TRC_GAMMA26:
                case PL_COLOR_TRC_GAMMA28:
                case PL_COLOR_TRC_PRO_PHOTO:
                case PL_COLOR_TRC_ST428: params->dst_repr->sys = PL_COLOR_SYSTEM_BT_709; break;
                case PL_COLOR_TRC_PQ:
                case PL_COLOR_TRC_HLG:
                case PL_COLOR_TRC_V_LOG:
                case PL_COLOR_TRC_S_LOG1:
                case PL_COLOR_TRC_S_LOG2: params->dst_repr->sys = PL_COLOR_SYSTEM_BT_2020_NC; break;
            }
        }

        fi->vi.pixel_type = AVS_CS_YUV444P16;
    }
    else
    {
        params->src_repr->sys = PL_COLOR_SYSTEM_RGB;

        params->dst_repr->levels = PL_COLOR_LEVELS_FULL;
        params->dst_repr->sys = PL_COLOR_SYSTEM_RGB;
    }

    AVS_Value v{ avs_new_value_clip(clip) };

    fi->user_data = reinterpret_cast<void*>(params);
    fi->get_frame = tonemap_get_frame;
    fi->set_cache_hints = tonemap_set_cache_hints;
    fi->free_filter = free_tonemap;

    avs_release_clip(clip);

    return v;
}
