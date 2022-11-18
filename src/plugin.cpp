#include "avs_libplacebo.h"

static const char* AVSC_CC avisynth_c_plugin_init(AVS_ScriptEnvironment* env)
{
    avs_add_function(env, "libplacebo_Deband", "c[iterations]i[threshold]f[radius]f[grainY]f[grainC]f[dither]i[lut_size]i[temporal]b[planes]i*[device]i[list_device]b[grain_neutral]f*", create_deband, 0);
    avs_add_function(env, "libplacebo_Resample", "cii[filter]s[radius]f[clamp]f[taper]f[blur]f[param1]f[param2]f[sx]f[sy]f[antiring]f[lut_entries]i[cutoff]f[sigmoidize]b[linearize]b[sigmoid_center]f[sigmoid_slope]f[trc]i[cplace]i[device]i[list_device]b[src_width]f[src_height]f", create_resample, 0);
    avs_add_function(env, "libplacebo_Shader", "cs[width]i[height]i[chroma_loc]i[matrix]i[trc]i[filter]s[radius]f[clamp]f[taper]f[blur]f[param1]f[param2]f[antiring]f[lut_entries]i[cutoff]f[sigmoidize]b[linearize]b[sigmoid_center]f[sigmoid_slope]f[shader_param]s[device]i[list_device]b", create_shader, 0);
    avs_add_function(env, "libplacebo_Tonemap", "c[src_csp]i[dst_csp]i[src_max]f[src_min]f[dst_max]f[dst_min]f[dynamic_peak_detection]b[smoothing_period]f[scene_threshold_low]f[scene_threshold_high]f[intent]i[gamut_mode]i[tone_mapping_function]i[tone_mapping_mode]i[tone_mapping_param]f[tone_mapping_crosstalk]f[use_dovi]b[device]i[list_device]b", create_tonemap, 0);

    return "avslibplacebo";
}
