#include "avs_libplacebo.h"

const char* AVSC_CC avisynth_c_plugin_init(AVS_ScriptEnvironment* env)
{
    avs_add_function(env, "libplacebo_Deband", "c[iterations]i[threshold]f[radius]f[grain]f[dither]i[lut_size]i[temporal]b[planes]i*[device]i[list_device]b", create_deband, 0);
    avs_add_function(env, "libplacebo_Resample", "cii[filter]s[radius]f[clamp]f[taper]f[blur]f[param1]f[param2]f[sx]f[sy]f[antiring]f[lut_entries]i[cutoff]f[sigmoidize]b[linearize]b[sigmoid_center]f[sigmoid_slope]f[trc]i[cplace]i[device]i[list_device]b", create_resample, 0);

    return "avslibplacebo";
}
