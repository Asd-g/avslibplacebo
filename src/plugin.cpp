#include <iostream>
#include <span>

#include "avs_libplacebo.h"

const char* AVSC_CC avisynth_c_plugin_init(AVS_ScriptEnvironment* env)
{
    static constexpr int REQUIRED_INTERFACE_VERSION{ 9 };
    static constexpr int REQUIRED_BUGFIX_VERSION{ 2 };
    static constexpr std::string_view required_functions_storage[]{
        "avs_pool_free",           // avs loader helper functions
        "avs_release_clip",        // avs loader helper functions
        "avs_release_value",       // avs loader helper functions
        "avs_release_video_frame", // avs loader helper functions
        "avs_take_clip",           // avs loader helper functions
        "avs_add_function",
        "avs_new_c_filter",
        "avs_new_video_frame_p",
        "avs_set_to_clip",
        "avs_get_frame",
        "avs_get_row_size_p",
        "avs_get_height_p",
        "avs_get_pitch_p",
        "avs_get_video_info",
        "avs_get_read_ptr_p",
        "avs_get_write_ptr_p",
        "avs_num_components",
        "avs_bit_blt",
        "avs_bits_per_component",
        "avs_prop_set_int",
        "avs_get_frame_props_rw",
        "avs_is_420",
        "avs_is_422",
        "avs_get_plane_width_subsampling",
        "avs_get_plane_height_subsampling",
        "avs_component_size",
        "avs_get_frame_props_ro",
        "avs_prop_get_int",
        "avs_prop_get_float",
        "avs_prop_get_float_array",
        "avs_prop_num_elements",
        "avs_prop_get_data",
        "avs_prop_get_data_size",
        "avs_prop_set_float",
        "avs_prop_set_float_array",
        "avs_prop_delete_key",
        "avs_check_version",
        "avs_get_env_property"
    };
    static constexpr std::span<const std::string_view> required_functions{ required_functions_storage };

    if (!avisynth_c_api_loader::get_api(env, REQUIRED_INTERFACE_VERSION, REQUIRED_BUGFIX_VERSION, required_functions)) {
        std::cerr << avisynth_c_api_loader::get_last_error() << std::endl;
        return avisynth_c_api_loader::get_last_error();
    }


    g_avs_api->avs_add_function(env, "libplacebo_Deband",
        "c"
        "[iterations]i"
        "[threshold]f"
        "[radius]f"
        "[grainY]f"
        "[grainC]f"
        "[dither]i"
        "[lut_size]i"
        "[temporal]b"
        "[planes]i*"
        "[device]i"
        "[list_device]b"
        "[grain_neutral]f*",
        create_deband, 0);

    g_avs_api->avs_add_function(env, "libplacebo_Resample",
        "c"
        "i"
        "i"
        "[filter]s"
        "[radius]f"
        "[clamp]f"
        "[taper]f"
        "[blur]f"
        "[param1]f"
        "[param2]f"
        "[sx]f"
        "[sy]f"
        "[antiring]f"
        "[sigmoidize]b"
        "[linearize]b"
        "[sigmoid_center]f"
        "[sigmoid_slope]f"
        "[trc]i"
        "[cplace]i"
        "[device]i"
        "[list_device]b"
        "[src_width]f"
        "[src_height]f",
        create_resample, 0);

    g_avs_api->avs_add_function(env, "libplacebo_Shader",
        "c"
        "s"
        "[width]i"
        "[height]i"
        "[chroma_loc]i"
        "[matrix]i"
        "[trc]i"
        "[filter]s"
        "[radius]f"
        "[clamp]f"
        "[taper]f"
        "[blur]f"
        "[param1]f"
        "[param2]f"
        "[antiring]f"
        "[sigmoidize]b"
        "[linearize]b"
        "[sigmoid_center]f"
        "[sigmoid_slope]f"
        "[shader_param]s"
        "[device]i"
        "[list_device]b",
        create_shader, 0);

    g_avs_api->avs_add_function(env, "libplacebo_Tonemap",
        "c"
        "[src_csp]i"
        "[dst_csp]i"
        "[src_max]f"
        "[src_min]f"
        "[dst_max]f"
        "[dst_min]f"
        "[dynamic_peak_detection]b"
        "[smoothing_period]f"
        "[scene_threshold_low]f"
        "[scene_threshold_high]f"
        "[percentile]f"
        "[black_cutoff]f"
        "[gamut_mapping_mode]s"
        "[tone_mapping_function]s"
        "[tone_constants]s*"
        "[metadata]i"
        "[contrast_recovery]f"
        "[contrast_smoothness]f"
        "[visualize_lut]b"
        "[show_clipping]b"
        "[use_dovi]b"
        "[device]i"
        "[list_device]b"
        "[cscale]s"
        "[lut]s"
        "[lut_type]i"
        "[dst_prim]i"
        "[dst_trc]i"
        "[dst_sys]i",
        create_tonemap, 0);

    return "avslibplacebo";
}
