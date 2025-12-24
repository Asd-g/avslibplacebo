#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <regex>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

#include "avs_libplacebo.h"

extern "C"
#include "libdovi/rpu_parser.h"

    static std::mutex mtx;

static std::unique_ptr<pl_dovi_metadata> create_dovi_meta(DoviRpuOpaque& rpu, const DoviRpuDataHeader& hdr)
{
    std::unique_ptr<pl_dovi_metadata> dovi_meta{std::make_unique<pl_dovi_metadata>()}; // persist state
    if (hdr.use_prev_vdr_rpu_flag)
        goto done;

    {
        const DoviRpuDataMapping* mapping = dovi_rpu_get_data_mapping(&rpu);
        if (!mapping)
            goto skip_mapping;

        {
            const uint64_t bits{hdr.bl_bit_depth_minus8 + 8};
            const float scale{1.0f / (1 << hdr.coefficient_log2_denom)};

            for (int c{0}; c < 3; ++c)
            {
                const DoviReshapingCurve curve{mapping->curves[c]};

                pl_dovi_metadata::pl_reshape_data* cmp{&dovi_meta->comp[c]};
                cmp->num_pivots = curve.pivots.len;
                memset(cmp->method, curve.mapping_idc, sizeof(cmp->method));

                uint16_t pivot{0};
                for (int pivot_idx{0}; pivot_idx < cmp->num_pivots; ++pivot_idx)
                {
                    pivot += curve.pivots.data[pivot_idx];
                    cmp->pivots[pivot_idx] = static_cast<float>(pivot) / ((1 << bits) - 1);
                }

                for (int i{0}; i < cmp->num_pivots - 1; ++i)
                {
                    memset(cmp->poly_coeffs[i], 0, sizeof(cmp->poly_coeffs[i]));

                    if (curve.polynomial)
                    {
                        const DoviPolynomialCurve* poly_curve = curve.polynomial;

                        for (int k{0}; k <= poly_curve->poly_order_minus1.data[i] + 1; ++k)
                        {
                            int64_t ipart{poly_curve->poly_coef_int.list[i]->data[k]};
                            uint64_t fpart{poly_curve->poly_coef.list[i]->data[k]};
                            cmp->poly_coeffs[i][k] = ipart + scale * fpart;
                        }
                    }
                    else if (curve.mmr)
                    {
                        const DoviMMRCurve* mmr_curve = curve.mmr;

                        int64_t ipart{mmr_curve->mmr_constant_int.data[i]};
                        uint64_t fpart{mmr_curve->mmr_constant.data[i]};
                        cmp->mmr_constant[i] = ipart + scale * fpart;
                        cmp->mmr_order[i] = mmr_curve->mmr_order_minus1.data[i] + 1;

                        for (int j{0}; j < cmp->mmr_order[i]; ++j)
                        {
                            for (int k{0}; k < 7; ++k)
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
        const DoviVdrDmData* dm_data{dovi_rpu_get_vdr_dm_data(&rpu)};
        if (!dm_data)
            goto done;

        const uint32_t* off{&dm_data->ycc_to_rgb_offset0};
        for (int i{0}; i < 3; ++i)
            dovi_meta->nonlinear_offset[i] = static_cast<float>(off[i]) / (1 << 28);

        const int16_t* src{&dm_data->ycc_to_rgb_coef0};
        float* dst{&dovi_meta->nonlinear.m[0][0]};
        for (int i{0}; i < 9; ++i)
            dst[i] = src[i] / 8192.0f;

        src = &dm_data->rgb_to_lms_coef0;
        dst = &dovi_meta->linear.m[0][0];
        for (int i{0}; i < 9; ++i)
            dst[i] = src[i] / 16384.0f;

        dovi_rpu_free_vdr_dm_data(dm_data);
    }

done:
    return dovi_meta;
}

enum class supported_colorspace
{
    CSP_SDR = 0,
    CSP_HDR10,
    CSP_HLG,
    CSP_DOVI,
};

template<typename Key, typename Value, std::size_t Size>
struct Map
{
    std::array<std::pair<Key, Value>, Size> data;
    constexpr Value at(const Key& key) const
    {
        const auto itr{std::find_if(begin(data), end(data), [&key](const auto& v) { return v.first == key; })};

        if (itr != end(data))
            return itr->second;
        if constexpr (std::is_same_v<Key, int>)
            return 2;
        else
            return std::make_tuple(nullptr, -1.0f, -1.0f);
    }
};

static constexpr std::array<std::pair<int, int>, 7> frame_prop_matrix{std::make_pair(PL_COLOR_SYSTEM_BT_709, 1),
    std::make_pair(PL_COLOR_SYSTEM_BT_601, 5), std::make_pair(PL_COLOR_SYSTEM_SMPTE_240M, 7), std::make_pair(PL_COLOR_SYSTEM_YCGCO, 8),
    std::make_pair(PL_COLOR_SYSTEM_BT_2020_NC, 9), std::make_pair(PL_COLOR_SYSTEM_BT_2020_C, 10),
    std::make_pair(PL_COLOR_SYSTEM_BT_2100_PQ, 14)};

static constexpr std::array<std::pair<int, int>, 6> frame_prop_transfer{std::make_pair(PL_COLOR_TRC_BT_1886, 1),
    std::make_pair(PL_COLOR_TRC_LINEAR, 8), std::make_pair(PL_COLOR_TRC_SRGB, 13), std::make_pair(PL_COLOR_TRC_PQ, 16),
    std::make_pair(PL_COLOR_TRC_HLG, 18), std::make_pair(PL_COLOR_TRC_PRO_PHOTO, 30)};

static constexpr std::array<std::pair<int, int>, 10> frame_prop_primaries{std::make_pair(PL_COLOR_PRIM_BT_709, 1),
    std::make_pair(PL_COLOR_PRIM_BT_470M, 4), std::make_pair(PL_COLOR_PRIM_BT_601_625, 5), std::make_pair(PL_COLOR_PRIM_BT_601_525, 6),
    std::make_pair(PL_COLOR_PRIM_FILM_C, 8), std::make_pair(PL_COLOR_PRIM_BT_2020, 9), std::make_pair(PL_COLOR_PRIM_DCI_P3, 11),
    std::make_pair(PL_COLOR_PRIM_DISPLAY_P3, 12), std::make_pair(PL_COLOR_PRIM_EBU_3213, 22), std::make_pair(PL_COLOR_PRIM_PRO_PHOTO, 30)};

static constexpr auto map_matrix{Map<int, int, 7>{{frame_prop_matrix}}};
static constexpr auto map_transfer{Map<int, int, 6>{{frame_prop_transfer}}};
static constexpr auto map_primaries{Map<int, int, 10>{{frame_prop_primaries}}};

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

static bool tonemap_do_plane(tonemap* d, const pl_plane* planes) noexcept
{
    pl_frame img{};
    img.num_planes = 3;
    img.repr = *d->src_repr;
    img.planes[0] = planes[0];
    img.planes[1] = planes[1];
    img.planes[2] = planes[2];
    img.color = *d->src_pl_csp;

    if (d->is_subsampled)
        pl_frame_set_chroma_location(&img, d->chromaLocation);

    pl_frame out{};
    out.num_planes = 3;
    out.repr = *d->dst_repr;
    out.color = *d->dst_pl_csp;

    for (int i{0}; i < 3; ++i)
    {
        out.planes[i].texture = d->vf->tex_out[i];
        out.planes[i].components = 1;
        out.planes[i].component_mapping[0] = i;
    }

    return pl_render_image(d->vf->rr, &img, &out, d->render_params.get());
}

static int tonemap_filter(AVS_VideoFrame* dst, AVS_VideoFrame* src, tonemap* d, const AVS_FilterInfo* fi) noexcept
{
    const int error{[&]() {
        for (int i{0}; i < 3; ++i)
        {
            pl_tex_destroy(d->vf->gpu, &d->vf->tex_out[i]);
            pl_tex_destroy(d->vf->gpu, &d->vf->tex_in[i]);
        }

        return -1;
    }()};

    const pl_fmt fmt{pl_find_named_fmt(d->vf->gpu, "r16")};
    if (!fmt)
        return error;

    pl_tex_params t_r{};
    t_r.w = fi->vi.width;
    t_r.h = fi->vi.height;
    t_r.format = fmt;
    t_r.renderable = true;
    t_r.host_readable = true;

    pl_plane pl_planes[3]{};
    constexpr int planes_y[3]{AVS_PLANAR_Y, AVS_PLANAR_U, AVS_PLANAR_V};
    constexpr int planes_r[3]{AVS_PLANAR_R, AVS_PLANAR_G, AVS_PLANAR_B};
    const int* planes{(avs_is_rgb(&fi->vi)) ? planes_r : planes_y};

    for (int i{0}; i < 3; ++i)
    {
        const int plane{planes[i]};

        pl_plane_data pl{};
        pl.type = PL_FMT_UNORM;
        pl.pixel_stride = 2;
        pl.component_size[0] = 16;
        pl.width = g_avs_api->avs_get_row_size_p(src, plane) / g_avs_api->avs_component_size(&fi->vi);
        pl.height = g_avs_api->avs_get_height_p(src, plane);
        pl.row_stride = g_avs_api->avs_get_pitch_p(src, plane);
        pl.pixels = g_avs_api->avs_get_read_ptr_p(src, plane);
        pl.component_map[0] = i;

        // Upload planes
        if (!pl_upload_plane(d->vf->gpu, &pl_planes[i], &d->vf->tex_in[i], &pl))
            return error;
        if (!pl_tex_recreate(d->vf->gpu, &d->vf->tex_out[i], &t_r))
            return error;
    }

    // Process plane
    if (!tonemap_do_plane(d, pl_planes))
        return error;

    const int dst_stride = (g_avs_api->avs_get_pitch_p(dst, AVS_DEFAULT_PLANE) + (d->vf->gpu->limits.align_tex_xfer_pitch) - 1) &
                           ~((d->vf->gpu->limits.align_tex_xfer_pitch) - 1);
    pl_buf_params buf_params{};
    buf_params.size = dst_stride * fi->vi.height;
    buf_params.host_mapped = true;

    pl_buf dst_buf{};
    if (!pl_buf_recreate(d->vf->gpu, &dst_buf, &buf_params))
        return error;

    // Download planes
    for (int i{0}; i < 3; ++i)
    {
        pl_tex_transfer_params ttr1{};
        ttr1.row_pitch = dst_stride;
        ttr1.buf = dst_buf;
        ttr1.tex = d->vf->tex_out[i];

        if (!pl_tex_download(d->vf->gpu, &ttr1))
            return error;

        pl_tex_destroy(d->vf->gpu, &d->vf->tex_out[i]);
        pl_tex_destroy(d->vf->gpu, &d->vf->tex_in[i]);

        while (pl_buf_poll(d->vf->gpu, dst_buf, 0))
            ;
        memcpy(g_avs_api->avs_get_write_ptr_p(dst, planes[i]), dst_buf->data, dst_buf->params.size);
    }

    pl_buf_destroy(d->vf->gpu, &dst_buf);

    return 0;
}

static AVS_VideoFrame* AVSC_CC tonemap_get_frame(AVS_FilterInfo* fi, int n)
{
    tonemap* d{reinterpret_cast<tonemap*>(fi->user_data)};

    avs_helpers::avs_video_frame_ptr src_ptr{g_avs_api->avs_get_frame(fi->child, n)};
    AVS_VideoFrame* src{src_ptr.get()};
    if (!src)
        return nullptr;

    avs_helpers::avs_video_frame_ptr dst_ptr{g_avs_api->avs_new_video_frame_p(fi->env, &fi->vi, src)};
    AVS_VideoFrame* dst{dst_ptr.get()};

    const auto error{[&](const std::string& msg) {
        d->msg = msg;
        fi->error = d->msg.c_str();

        return nullptr;
    }};

    int err;
    const AVS_Map* props{g_avs_api->avs_get_frame_props_ro(fi->env, src)};

    int64_t props_levels{g_avs_api->avs_prop_get_int(fi->env, props, "_ColorRange", 0, &err)};
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

    const double maxCll{g_avs_api->avs_prop_get_float(fi->env, props, "ContentLightLevelMax", 0, &err)};
    const double maxFall{g_avs_api->avs_prop_get_float(fi->env, props, "ContentLightLevelAverage", 0, &err)};

    d->src_pl_csp->hdr.max_cll = maxCll;
    d->src_pl_csp->hdr.max_fall = maxFall;

    if (d->original_src_max < 1)
        d->src_pl_csp->hdr.max_luma = g_avs_api->avs_prop_get_float(fi->env, props, "MasteringDisplayMaxLuminance", 0, &err);
    if (d->original_src_min <= 0)
        d->src_pl_csp->hdr.min_luma = g_avs_api->avs_prop_get_float(fi->env, props, "MasteringDisplayMinLuminance", 0, &err);

    const double* primariesX{g_avs_api->avs_prop_get_float_array(fi->env, props, "MasteringDisplayPrimariesX", &err)};
    const double* primariesY{g_avs_api->avs_prop_get_float_array(fi->env, props, "MasteringDisplayPrimariesY", &err)};

    if (primariesX && primariesY && g_avs_api->avs_prop_num_elements(fi->env, props, "MasteringDisplayPrimariesX") == 3 &&
        g_avs_api->avs_prop_num_elements(fi->env, props, "MasteringDisplayPrimariesY") == 3)
    {
        d->src_pl_csp->hdr.prim.red.x = primariesX[0];
        d->src_pl_csp->hdr.prim.red.y = primariesY[0];
        d->src_pl_csp->hdr.prim.green.x = primariesX[1];
        d->src_pl_csp->hdr.prim.green.y = primariesY[1];
        d->src_pl_csp->hdr.prim.blue.x = primariesX[2];
        d->src_pl_csp->hdr.prim.blue.y = primariesY[2];

        // White point comes with primaries
        const double whitePointX{g_avs_api->avs_prop_get_float(fi->env, props, "MasteringDisplayWhitePointX", 0, &err)};
        const double whitePointY{g_avs_api->avs_prop_get_float(fi->env, props, "MasteringDisplayWhitePointY", 0, &err)};

        if (whitePointX && whitePointY)
        {
            d->src_pl_csp->hdr.prim.white.x = whitePointX;
            d->src_pl_csp->hdr.prim.white.y = whitePointY;
        }
    }
    else
        // Assume DCI-P3 D65 default?
        pl_raw_primaries_merge(&d->src_pl_csp->hdr.prim,
            pl_raw_primaries_get((d->src_csp == supported_colorspace::CSP_SDR) ? d->src_pl_csp->primaries : PL_COLOR_PRIM_DISPLAY_P3));

    d->chromaLocation = static_cast<pl_chroma_location>(g_avs_api->avs_prop_get_int(fi->env, props, "_ChromaLocation", 0, &err));
    if (!err)
        d->chromaLocation = static_cast<pl_chroma_location>(static_cast<int>(d->chromaLocation) + 1);

    // DOVI
    if (d->src_csp == supported_colorspace::CSP_DOVI)
    {
        if (g_avs_api->avs_prop_num_elements(fi->env, props, "DolbyVisionRPU") > -1)
        {
            if (d->use_dovi)
            {
                uint8_t dovi_profile{0};
                const uint8_t* doviRpu{reinterpret_cast<const uint8_t*>(g_avs_api->avs_prop_get_data(fi->env, props, "DolbyVisionRPU", 0, &err))};
                size_t doviRpuSize{static_cast<size_t>(g_avs_api->avs_prop_get_data_size(fi->env, props, "DolbyVisionRPU", 0, &err))};

                if (doviRpu && doviRpuSize)
                {
                    DoviRpuOpaque* rpu{dovi_parse_unspec62_nalu(doviRpu, doviRpuSize)};
                    const DoviRpuDataHeader* header{dovi_rpu_get_header(rpu)};

                    if (!header)
                    {
                        std::string err{dovi_rpu_get_error(rpu)};
                        dovi_rpu_free(rpu);
                        return error("libplacebo_Tonemap: failed parsing RPU: " + err);
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
                        const DoviVdrDmData* vdr_dm_data{dovi_rpu_get_vdr_dm_data(rpu)};

                        // Should avoid changing the source black point when mapping to PQ
                        // As the source image already has a specific black point,
                        // and the RPU isn't necessarily ground truth on the actual coded values

                        // Set target black point to the same as source
                        if (d->dst_csp == supported_colorspace::CSP_HDR10)
                            d->dst_pl_csp->hdr.min_luma = d->src_pl_csp->hdr.min_luma;
                        else
                            d->src_pl_csp->hdr.min_luma = pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS, vdr_dm_data->source_min_pq / 4095.0f);

                        d->src_pl_csp->hdr.max_luma = pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS, vdr_dm_data->source_max_pq / 4095.0f);

                        if (vdr_dm_data->dm_data.level1)
                        {
                            const DoviExtMetadataBlockLevel1* l1{vdr_dm_data->dm_data.level1};
                            d->src_pl_csp->hdr.avg_pq_y = l1->avg_pq / 4095.0f;
                            d->src_pl_csp->hdr.max_pq_y = l1->max_pq / 4095.0f;
                            d->src_pl_csp->hdr.scene_avg = pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS, l1->avg_pq / 4095.0f);
                            d->src_pl_csp->hdr.scene_max[0] = d->src_pl_csp->hdr.scene_max[1] = d->src_pl_csp->hdr.scene_max[2] =
                                pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS, l1->max_pq / 4095.0f);
                        }

                        if (vdr_dm_data->dm_data.level6)
                        {
                            const DoviExtMetadataBlockLevel6* meta{vdr_dm_data->dm_data.level6};

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
                    return error("libplacebo_Tonemap: invlid DolbyVisionRPU frame property!");
            }
        }
        else
            return error("libplacebo_Tonemap: DolbyVisionRPU frame property is required for src_csp=3!");
    }

    pl_color_space_infer_map(d->src_pl_csp.get(), d->dst_pl_csp.get());

    if (std::lock_guard<std::mutex> lck(mtx); tonemap_filter(dst, src, d, fi))
        return error("libplacebo_Tonemap: " + d->vf->log_buffer.str());

    AVS_Map* dst_props{g_avs_api->avs_get_frame_props_rw(fi->env, dst)};
    g_avs_api->avs_prop_set_int(fi->env, dst_props, "_ColorRange", (d->dst_repr->levels == PL_COLOR_LEVELS_FULL) ? 0 : 1, 0);
    g_avs_api->avs_prop_set_int(fi->env, dst_props, "_Matrix", (d->dst_repr->sys == PL_COLOR_SYSTEM_RGB) ? 0 : map_matrix.at(d->dst_repr->sys), 0);
    g_avs_api->avs_prop_set_int(fi->env, dst_props, "_Transfer", map_transfer.at(d->dst_pl_csp->transfer), 0);
    g_avs_api->avs_prop_set_int(fi->env, dst_props, "_Primaries", map_primaries.at(d->dst_pl_csp->primaries), 0);

    if (d->dst_pl_csp->transfer <= PL_COLOR_TRC_ST428)
    {
        g_avs_api->avs_prop_delete_key(fi->env, dst_props, "ContentLightLevelMax");
        g_avs_api->avs_prop_delete_key(fi->env, dst_props, "ContentLightLevelAverage");
        g_avs_api->avs_prop_delete_key(fi->env, dst_props, "MasteringDisplayMaxLuminance");
        g_avs_api->avs_prop_delete_key(fi->env, dst_props, "MasteringDisplayMinLuminance");
        g_avs_api->avs_prop_delete_key(fi->env, dst_props, "MasteringDisplayPrimariesX");
        g_avs_api->avs_prop_delete_key(fi->env, dst_props, "MasteringDisplayPrimariesY");
        g_avs_api->avs_prop_delete_key(fi->env, dst_props, "MasteringDisplayWhitePointX");
        g_avs_api->avs_prop_delete_key(fi->env, dst_props, "MasteringDisplayWhitePointY");
    }
    else
    {
        g_avs_api->avs_prop_set_float(fi->env, dst_props, "ContentLightLevelMax", d->dst_pl_csp->hdr.max_cll, 0);
        g_avs_api->avs_prop_set_float(fi->env, dst_props, "ContentLightLevelAverage", d->dst_pl_csp->hdr.max_fall, 0);
        g_avs_api->avs_prop_set_float(fi->env, dst_props, "MasteringDisplayMaxLuminance", d->dst_pl_csp->hdr.max_luma, 0);
        g_avs_api->avs_prop_set_float(fi->env, dst_props, "MasteringDisplayMinLuminance", d->dst_pl_csp->hdr.min_luma, 0);

        const std::array<double, 3> prims_x{d->dst_pl_csp->hdr.prim.red.x, d->dst_pl_csp->hdr.prim.green.x, d->dst_pl_csp->hdr.prim.blue.x};
        const std::array<double, 3> prims_y{d->dst_pl_csp->hdr.prim.red.y, d->dst_pl_csp->hdr.prim.green.y, d->dst_pl_csp->hdr.prim.blue.y};

        g_avs_api->avs_prop_set_float_array(fi->env, dst_props, "MasteringDisplayPrimariesX", prims_x.data(), 3);
        g_avs_api->avs_prop_set_float_array(fi->env, dst_props, "MasteringDisplayPrimariesY", prims_y.data(), 3);
        g_avs_api->avs_prop_set_float(fi->env, dst_props, "MasteringDisplayWhitePointX", d->dst_pl_csp->hdr.prim.white.x, 0);
        g_avs_api->avs_prop_set_float(fi->env, dst_props, "MasteringDisplayWhitePointY", d->dst_pl_csp->hdr.prim.white.y, 0);
    }

    if (g_avs_api->avs_num_components(&fi->vi) > 3)
        g_avs_api->avs_bit_blt(fi->env, g_avs_api->avs_get_write_ptr_p(dst, AVS_PLANAR_A), g_avs_api->avs_get_pitch_p(dst, AVS_PLANAR_A),
            g_avs_api->avs_get_read_ptr_p(src, AVS_PLANAR_A), g_avs_api->avs_get_pitch_p(src, AVS_PLANAR_A),
            g_avs_api->avs_get_row_size_p(src, AVS_PLANAR_A), g_avs_api->avs_get_height_p(src, AVS_PLANAR_A));

    return dst_ptr.release();
}

static void AVSC_CC free_tonemap(AVS_FilterInfo* fi)
{
    tonemap* d{reinterpret_cast<tonemap*>(fi->user_data)};

    if (d->render_params->lut)
        pl_lut_free(const_cast<pl_custom_lut**>(&d->render_params->lut));

    avs_libplacebo_uninit(d->vf);
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
        Clip,
        Src_csp,
        Dst_csp,
        Src_max,
        Src_min,
        Dst_max,
        Dst_min,
        Dynamic_peak_detection,
        Smoothing_period,
        Scene_threshold_low,
        Scene_threshold_high,
        Percentile,
        Black_cutoff,
        Gamut_mapping_mode,
        Tone_mapping_function,
        Tone_constants,
        Metadata,
        Contrast_recovery,
        Contrast_smoothness,
        Visualize_lut,
        Show_clipping,
        Use_dovi,
        Device,
        List_device,
        Cscale,
        Lut,
        Lut_type,
        Dst_prim,
        Dst_trc,
        Dst_sys
    };

    AVS_FilterInfo* fi;
    avs_helpers::avs_clip_ptr clip_ptr{g_avs_api->avs_new_c_filter(env, &fi, avs_array_elt(args, Clip), 1)};
    AVS_Clip* clip{clip_ptr.get()};

    std::unique_ptr<tonemap> params{std::make_unique<tonemap>()};
    const int srcIsRGB{avs_is_rgb(&fi->vi)};

    AVS_Value avs_ver{avs_version(params->msg, "libplacebo_Tonemap", env)};
    if (avs_is_error(avs_ver))
        return avs_ver;

    if (!avs_is_planar(&fi->vi))
        return set_error("libplacebo_Tonemap: clip must be in planar format.", nullptr);
    if (g_avs_api->avs_bits_per_component(&fi->vi) != 16)
        return set_error("libplacebo_Tonemap: bit depth must be 16-bit.", nullptr);
    if (g_avs_api->avs_num_components(&fi->vi) < 3)
        return set_error("libplacebo_Tonemap: the clip must have at least three planes.", nullptr);

    const int device{avs_defined(avs_array_elt(args, Device)) ? avs_as_int(avs_array_elt(args, Device)) : -1};
    const int list_device{avs_defined(avs_array_elt(args, List_device)) ? avs_as_bool(avs_array_elt(args, List_device)) : 0};

    if (list_device || device > -1)
    {
        std::vector<VkPhysicalDevice> devices{};
        VkInstance inst{};

        AVS_Value dev_info{devices_info(clip, fi->env, devices, inst, params->msg, "libplacebo_Tonemap", device, list_device)};
        if (avs_is_error(dev_info) || avs_is_clip(dev_info))
        {
            fi->user_data = params.release();
            fi->free_filter = free_tonemap;

            return dev_info;
        }

        params->vf = avs_libplacebo_init(devices[device], params->msg);

        vkDestroyInstance(inst, nullptr);
    }
    else
    {
        if (device < -1)
            return set_error("libplacebo_Tonemap: device must be greater than or equal to -1.", nullptr);

        params->vf = avs_libplacebo_init(nullptr, params->msg);
    }

    if (params->msg.size())
    {
        params->msg = "libplacebo_Tonemap: " + params->msg;
        return set_error(params->msg.c_str(), nullptr);
    }

    params->src_pl_csp = std::make_unique<pl_color_space>();
    params->src_csp =
        static_cast<supported_colorspace>(avs_defined(avs_array_elt(args, Src_csp)) ? avs_as_int(avs_array_elt(args, Src_csp)) : 1);
    if (params->src_csp == supported_colorspace::CSP_DOVI && srcIsRGB)
        return set_error("libplacebo_Tonemap: Dolby Vision source colorspace must be a YUV clip!", params->vf);

    switch (params->src_csp)
    {
    case supported_colorspace::CSP_SDR:
        *params->src_pl_csp = pl_color_space_bt709;
        break;
    case supported_colorspace::CSP_HDR10:
    case supported_colorspace::CSP_DOVI:
        *params->src_pl_csp = pl_color_space_hdr10;
        break;
    case supported_colorspace::CSP_HLG:
        *params->src_pl_csp = pl_color_space_bt2020_hlg;
        break;
    default:
        return set_error("libplacebo_Tonemap: Invalid source colorspace for tonemapping.", params->vf);
    }

    params->dst_pl_csp = std::make_unique<pl_color_space>();

    const int dst_prim_defined{avs_defined(avs_array_elt(args, Dst_prim))};
    const int dst_trc_defined{avs_defined(avs_array_elt(args, Dst_trc))};
    if (dst_prim_defined && dst_trc_defined)
    {
        const int dst_prim{avs_as_int(avs_array_elt(args, Dst_prim))};
        const int dst_trc{avs_as_int(avs_array_elt(args, Dst_trc))};

        if (dst_prim < 1 || dst_prim > 17)
            return set_error("libplacebo_Tonemap: dst_prim must be between 1 and 17.", params->vf);
        if (dst_trc < 1 || dst_trc > 16)
            return set_error("libplacebo_Tonemap: dst_trc must be between 1 and 16.", params->vf);

        params->dst_pl_csp->primaries = static_cast<pl_color_primaries>(dst_prim);
        params->dst_pl_csp->transfer = static_cast<pl_color_transfer>(dst_trc);
    }
    else if (!dst_prim_defined && !dst_trc_defined)
    {
        params->dst_csp =
            static_cast<supported_colorspace>(avs_defined(avs_array_elt(args, Dst_csp)) ? avs_as_int(avs_array_elt(args, Dst_csp)) : 0);
        switch (params->dst_csp)
        {
        case supported_colorspace::CSP_SDR:
            *params->dst_pl_csp = pl_color_space_bt709;
            break;
        case supported_colorspace::CSP_HDR10:
            *params->dst_pl_csp = pl_color_space_hdr10;
            break;
        case supported_colorspace::CSP_HLG:
            *params->dst_pl_csp = pl_color_space_bt2020_hlg;
            break;
        default:
            return set_error("libplacebo_Tonemap: Invalid target colorspace for tonemapping.", params->vf);
        }
    }
    else
        return set_error("libplacebo_Tonemap: dst_prim/dst_trc must be defined.", params->vf);

    const int lut_defined{avs_defined(avs_array_elt(args, Lut))};

    if (lut_defined)
    {
        params->render_params = std::make_unique<pl_render_params>();

        const char* lut_path{avs_as_string(avs_array_elt(args, Lut))};
        FILE* lut_file{nullptr};

#ifdef _WIN32
        const int required_size{MultiByteToWideChar(CP_UTF8, 0, lut_path, -1, nullptr, 0)};
        std::wstring wbuffer(required_size, 0);
        MultiByteToWideChar(CP_UTF8, 0, lut_path, -1, wbuffer.data(), required_size);
        lut_file = _wfopen(wbuffer.c_str(), L"rb");
        if (!lut_file)
        {
            const int req_size{MultiByteToWideChar(CP_ACP, 0, lut_path, -1, nullptr, 0)};
            std::wstring wbuffer(req_size, 0);
            MultiByteToWideChar(CP_ACP, 0, lut_path, -1, wbuffer.data(), req_size);
            lut_file = _wfopen(wbuffer.c_str(), L"rb");
        }
#else
        lut_file = std::fopen(lut_path, "rb");
#endif
        if (!lut_file)
        {
            params->msg = "libplacebo_Tonemap: error opening file " + std::string(lut_path) + " (" + std::strerror(errno) + ")";
            return set_error(params->msg.c_str(), params->vf);
        }
        if (std::fseek(lut_file, 0, SEEK_END))
        {
            std::fclose(lut_file);
            params->msg =
                "libplacebo_Tonemap: error seeking to the end of file " + std::string(lut_path) + " (" + std::strerror(errno) + ")";
            return set_error(params->msg.c_str(), params->vf);
        }
        const long lut_size{std::ftell(lut_file)};
        if (lut_size == -1)
        {
            std::fclose(lut_file);
            params->msg =
                "libplacebo_Tonemap: error determining the size of file " + std::string(lut_path) + " (" + std::strerror(errno) + ")";
            return set_error(params->msg.c_str(), params->vf);
        }

        std::rewind(lut_file);

        std::string bdata;
        bdata.resize(lut_size);
        std::fread(bdata.data(), 1, lut_size, lut_file);
        bdata[lut_size] = '\0';

        std::fclose(lut_file);

        params->render_params->lut = pl_lut_parse_cube(nullptr, bdata.c_str(), bdata.size());
        if (!params->render_params->lut)
            return set_error("libplacebo_Tonemap: failed lut parsing.", params->vf);

        const int lut_type{(avs_defined(avs_array_elt(args, Lut_type))) ? avs_as_int(avs_array_elt(args, Lut_type)) : 3};
        if (lut_type < 1 || lut_type > 3)
        {
            pl_lut_free(const_cast<pl_custom_lut**>(&params->render_params->lut));
            return set_error("libplacebo_Tonemap: lut_type must be between 1 and 3.", params->vf);
        }

        params->render_params->lut_type = static_cast<pl_lut_type>(lut_type);
    }
    else
    {
        if (params->src_csp == supported_colorspace::CSP_DOVI)
            params->dovi_meta = std::make_unique<pl_dovi_metadata>();

        params->colorMapParams = std::make_unique<pl_color_map_params>(pl_color_map_default_params);

        // Tone mapping function
        params->colorMapParams->tone_mapping_function = pl_find_tone_map_function(
            avs_defined(avs_array_elt(args, Tone_mapping_function)) ? avs_as_string(avs_array_elt(args, Tone_mapping_function)) : "bt2390");
        if (!params->colorMapParams->tone_mapping_function)
            return set_error("libplacebo_Tonemap: wrong tone_mapping_function.", params->vf);

        if (avs_defined(avs_array_elt(args, Tone_constants)))
        {
            constexpr std::array<std::string_view, 11> constants_list{"knee_adaptation", "knee_minimum", "knee_maximum", "knee_default",
                "knee_offset", "slope_tuning", "slope_offset", "spline_contrast", "reinhard_contrast", "linear_knee", "exposure"};

            constexpr std::array<std::pair<std::string_view, std::tuple<float pl_tone_map_constants::*, float, float>>, 11> constants_array{
                std::make_pair(constants_list[0], std::make_tuple(&pl_tone_map_constants::knee_adaptation, 0.0f, 1.0f)),
                std::make_pair(constants_list[1], std::make_tuple(&pl_tone_map_constants::knee_minimum, 0.0f, 0.5f)),
                std::make_pair(constants_list[2], std::make_tuple(&pl_tone_map_constants::knee_maximum, 0.5f, 1.0f)),
                std::make_pair(constants_list[3], std::make_tuple(&pl_tone_map_constants::knee_default, 0.0f, 1.0f)),
                std::make_pair(constants_list[4], std::make_tuple(&pl_tone_map_constants::knee_offset, 0.5f, 2.0f)),
                std::make_pair(constants_list[5], std::make_tuple(&pl_tone_map_constants::slope_tuning, 0.0f, 10.0f)),
                std::make_pair(constants_list[6], std::make_tuple(&pl_tone_map_constants::slope_offset, 0.0f, 1.0f)),
                std::make_pair(constants_list[7], std::make_tuple(&pl_tone_map_constants::spline_contrast, 0.0f, 1.5f)),
                std::make_pair(constants_list[8], std::make_tuple(&pl_tone_map_constants::reinhard_contrast, 0.0f, 1.0f)),
                std::make_pair(constants_list[9], std::make_tuple(&pl_tone_map_constants::linear_knee, 0.0f, 1.0f)),
                std::make_pair(constants_list[10], std::make_tuple(&pl_tone_map_constants::exposure, 0.0f, 10.0f))};

            constexpr auto constants_map{
                Map<std::string_view, std::tuple<float pl_tone_map_constants::*, float, float>, 11>{{constants_array}}};

            const int constants_size{avs_array_size(avs_array_elt(args, Tone_constants))};
            if (constants_size > 11)
                return set_error("libplacebo_Tonemap: tone_constants must be equal to or less than 11.", params->vf);

            for (int i{0}; i < constants_size; ++i)
            {
                std::string constants{avs_as_string(*(avs_as_array(avs_array_elt(args, Tone_constants)) + i))};
                transform(constants.begin(), constants.end(), constants.begin(), [](unsigned char c) { return std::tolower(c); });

                std::regex reg("(\\w+)=(\\d+(?:\\.\\d+)?)");
                std::smatch match;
                if (!std::regex_match(constants.cbegin(), constants.cend(), match, reg))
                    return set_error("libplacebo_Tonemap: regex failed parsing tone_constants.", params->vf);
                if (std::find(std::begin(constants_list), std::end(constants_list), match[1].str()) == std::end(constants_list))
                {
                    params->msg = "libplacebo_Tonemap: wrong tone_constant " + match[1].str() + ".";
                    return set_error(params->msg.c_str(), params->vf);
                }

                const float constant_value{std::stof(match[2].str())};
                if (constant_value < std::get<1>(constants_map.at(match[1].str())) ||
                    constant_value > std::get<2>(constants_map.at(match[1].str())))
                {
                    params->msg = "libplacebo_Tonemap: " + match[1].str() + " must be between " +
                                  std::to_string(std::get<1>(constants_map.at(match[1].str()))) + ".." +
                                  std::to_string(std::get<2>(constants_map.at(match[1].str()))) + ".";
                    return set_error(params->msg.c_str(), params->vf);
                }

                params->colorMapParams->tone_constants.*std::get<0>(constants_map.at(match[1].str())) = constant_value;
            }
        }
        if (avs_defined(avs_array_elt(args, Gamut_mapping_mode)))
        {
            params->colorMapParams->gamut_mapping = pl_find_gamut_map_function(avs_as_string(avs_array_elt(args, Gamut_mapping_mode)));
            if (!params->colorMapParams->gamut_mapping)
                return set_error("libplacebo_Tonemap: wrong gamut_mapping_mode.", params->vf);
        }
        if (avs_defined(avs_array_elt(args, Metadata)))
            params->colorMapParams->metadata = static_cast<pl_hdr_metadata_type>(avs_as_int(avs_array_elt(args, Metadata)));
        if (avs_defined(avs_array_elt(args, Visualize_lut)))
            params->colorMapParams->visualize_lut = avs_as_bool(avs_array_elt(args, Visualize_lut));
        if (avs_defined(avs_array_elt(args, Show_clipping)))
            params->colorMapParams->show_clipping = avs_as_bool(avs_array_elt(args, Show_clipping));

        params->colorMapParams->contrast_recovery =
            (avs_defined(avs_array_elt(args, Contrast_recovery))) ? avs_as_float(avs_array_elt(args, Contrast_recovery)) : 0.30f;
        params->colorMapParams->contrast_smoothness =
            (avs_defined(avs_array_elt(args, Contrast_smoothness))) ? avs_as_float(avs_array_elt(args, Contrast_smoothness)) : 3.5f;

        if (params->colorMapParams->contrast_recovery < 0.0f)
            return set_error("libplacebo_Tonemap: contrast_recovery must be equal to or greater than 0.0f.", params->vf);
        if (params->colorMapParams->contrast_smoothness < 0.0f)
            return set_error("libplacebo_Tonemap: contrast_smoothness must be equal to or greater than 0.0f.", params->vf);

        params->peakDetectParams = std::make_unique<pl_peak_detect_params>(pl_peak_detect_default_params);
        if (avs_defined(avs_array_elt(args, Smoothing_period)))
            params->peakDetectParams->smoothing_period = avs_as_float(avs_array_elt(args, Smoothing_period));
        if (avs_defined(avs_array_elt(args, Scene_threshold_low)))
            params->peakDetectParams->scene_threshold_low = avs_as_float(avs_array_elt(args, Scene_threshold_low));
        if (avs_defined(avs_array_elt(args, Scene_threshold_high)))
            params->peakDetectParams->scene_threshold_high = avs_as_float(avs_array_elt(args, Scene_threshold_high));
        if (avs_defined(avs_array_elt(args, Percentile)))
            params->peakDetectParams->percentile = avs_as_float(avs_array_elt(args, Percentile));
        if (avs_defined(avs_array_elt(args, Black_cutoff)))
            params->peakDetectParams->black_cutoff = avs_as_float(avs_array_elt(args, Black_cutoff));

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

        const int peak_detection{
            avs_defined(avs_array_elt(args, Dynamic_peak_detection)) ? avs_as_bool(avs_array_elt(args, Dynamic_peak_detection)) : 1};
        params->use_dovi = avs_defined(avs_array_elt(args, Use_dovi)) ? avs_as_bool(avs_array_elt(args, Use_dovi))
                                                                      : params->src_csp == supported_colorspace::CSP_DOVI;

        params->render_params = std::make_unique<pl_render_params>(pl_render_default_params);
        params->render_params->color_map_params = params->colorMapParams.get();
        params->render_params->peak_detect_params = (peak_detection) ? params->peakDetectParams.get() : nullptr;
        params->render_params->sigmoid_params = &pl_sigmoid_default_params;
        params->render_params->dither_params = &pl_dither_default_params;
        params->render_params->cone_params = nullptr;
        params->render_params->color_adjustment = nullptr;
        params->render_params->deband_params = nullptr;
    }

    const pl_filter_config* cscaler{pl_find_filter_config(
        (avs_defined(avs_array_elt(args, Cscale))) ? avs_as_string(avs_array_elt(args, Cscale)) : "spline36", PL_FILTER_UPSCALING)};
    if (!cscaler)
    {
        if (lut_defined)
            pl_lut_free(const_cast<pl_custom_lut**>(&params->render_params->lut));

        return set_error("libplacebo_Tonemap: not a valid cscale.", params->vf);
    }

    params->render_params->plane_upscaler = cscaler;

    if (srcIsRGB)
        params->is_subsampled = 0;
    else
        params->is_subsampled = g_avs_api->avs_get_plane_width_subsampling(&fi->vi, AVS_PLANAR_U) |
                                g_avs_api->avs_get_plane_height_subsampling(&fi->vi, AVS_PLANAR_U);

    params->src_repr = std::make_unique<pl_color_repr>();
    params->src_repr->bits.sample_depth = 16;
    params->src_repr->bits.color_depth = 16;
    params->src_repr->bits.bit_shift = 0;

    params->dst_repr = std::make_unique<pl_color_repr>();
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
            const int dst_sys{avs_as_int(avs_array_elt(args, Dst_sys))};
            if (dst_sys < 1 || dst_sys > 9)
            {
                if (lut_defined)
                    pl_lut_free(const_cast<pl_custom_lut**>(&params->render_params->lut));

                return set_error("libplacebo_Tonemap: dst_sys must be between 1 and 9.", params->vf);
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
            case PL_COLOR_TRC_ST428:
                params->dst_repr->sys = PL_COLOR_SYSTEM_BT_709;
                break;
            case PL_COLOR_TRC_PQ:
            case PL_COLOR_TRC_HLG:
            case PL_COLOR_TRC_V_LOG:
            case PL_COLOR_TRC_S_LOG1:
            case PL_COLOR_TRC_S_LOG2:
                params->dst_repr->sys = PL_COLOR_SYSTEM_BT_2020_NC;
                break;
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

    AVS_Value v;
    g_avs_api->avs_set_to_clip(&v, clip);

    fi->user_data = params.release();
    fi->get_frame = tonemap_get_frame;
    fi->set_cache_hints = tonemap_set_cache_hints;
    fi->free_filter = free_tonemap;

    return v;
}
