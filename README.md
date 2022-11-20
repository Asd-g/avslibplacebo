## Description

An AviSynth+ plugin interface to [libplacebo](https://code.videolan.org/videolan/libplacebo) - a reusable library for GPU-accelerated image/video processing primitives and shaders.

This is [a port of the VapourSynth plugin vs-placebo](https://github.com/Lypheo/vs-placebo).

### Requirements:

- Vulkan device

- AviSynth+ r3682 (can be downloaded from [here](https://gitlab.com/uvz/AviSynthPlus-Builds) until official release is uploaded) (r3689 recommended) or later

- Microsoft VisualC++ Redistributable Package 2022 (can be downloaded from [here](https://github.com/abbodi1406/vcredist/releases))

### Filters

[Debanding](#debanding)\
[Resampling](#resampling)\
[Shader](#shader)\
[Tone mapping](#tone-mapping)

### Debanding

#### Usage:

```
libplacebo_Deband(clip input, int "iterations", float "threshold", float "radius", float "grainY", float "grainC", int "dither", int "lut_size", bool "temporal", int[] "planes", int "device", bool "list_device", float[] "grain_neutral")
```

#### Parameters:

- input\
    A clip to process.\
    It must be in 8, 16 or 32-bit planar format.

- iterations\
    The number of debanding steps to perform per sample.\
    Each step reduces a bit more banding, but takes time to compute.\
    Note that the strength of each step falls off very quickly, so high numbers (>4) are practically useless.\
    Must be greater than or equal to 0.\
    Default: 1.

- threshold\
    The debanding filter's cut-off threshold.\
    Higher numbers increase the debanding strength dramatically, but progressively diminish image details.\
    Must be greater than or equal to 0.0.\
    Default: 4.0.

- radius\
    The debanding filter's initial radius.\
    The radius increases linearly for each iteration.\
    A higher radius will find more gradients, but a lower radius will smooth more aggressively.\
    Must be radius must be greater than or equal to 0.0.\
    Default: 16.0.

- grainY, grainC\
    Add some extra noise respectively to the luma and chroma plane.\
    This significantly helps cover up remaining quantization artifacts.\
    Higher numbers add more noise.\
    Note: When debanding HDR sources, even a small amount of grain can result in a very big change to the brightness level.\
    It's recommended to either scale this value down or disable it entirely for HDR.\
    Must be greater than or equal to 0.0.\
    When the clip is RGB, grainC doesn't have effect.\
    Default: grainY = 6.0; grainC = grainY.

- dither\
    It's valid only for 8-bit clips.\
    0: Disabled.

    1: PL_DITHER_BLUE_NOISE\
    Dither with blue noise.\
    Very high quality, but requires the use of a LUT.\
    Warning: Computing a blue noise texture with a large size can be very slow, however this only needs to be performed once. Even so, using this with a `lut_size` greater than `6` is generally ill-advised.

    2: PL_DITHER_ORDERED_LUT\
    Dither with an ordered (bayer) dither matrix, using a LUT.\
    Low quality, and since this also uses a LUT, there's generally no advantage to picking this instead of `PL_DITHER_BLUE_NOISE`.\
    It's mainly there for testing.

    3: PL_DITHER_ORDERED_FIXED\
    The same as `PL_DITHER_ORDERED_LUT`, but uses fixed function math instead of a LUT.\
    This is faster, but only supports a fixed dither matrix size of 16x16 (equal to a `lut_size` of 4).

    4: PL_DITHER_WHITE_NOISE\
    Dither with white noise.\
    This does not require a LUT and is fairly cheap to compute.\
    Unlike the other modes it doesn't show any repeating patterns either spatially or temporally, but the downside is that this is visually fairly jarring due to the presence of low frequencies in the noise spectrum.

    Default: 1.

- lut_size\
    For the dither methods which require the use of a LUT.\
    This controls the size of the LUT (base 2).\
    Must be less than or equal to 8.\
    Default: 6 (64x64).

- temporal\
    Enables temporal dithering.\
    his reduces the persistence of dithering artifacts by perturbing the dithering matrix per frame.\
    Warning: This can cause nasty aliasing artifacts on some LCD screens.\
    Default: False.

- planes\
    Planes to process.\
    1: Return garbage.\
    2: Copy plane.\
    3: Process plane. Always process planes when the clip is RGB.\
    Format is [y, u, v].\
    Default: [3, 2, 2].

- device\
    Sets target Vulkan device.\
    Use list_device to get the index of the available devices.\
    By default the default device is selected.

- list_device\
    Whether to draw the devices list on the frame.\
    Default: False.

- grain_neutral\
    "Neutral" grain value for each channel being debanded.\
    Grain application will be modulated to avoid disturbing colors close to this value.\
    Set this to a value corresponding to black in the relevant colorspace.\
    Must be greater than 0.0\
    Default: [0, 0, 0].

[Back to filters](#filters)

### Resampling

#### Usage:

```
libplacebo_Resample(clip input, int width, int height, string "filter", float "radius", float "clamp", float "taper", float "blur", float "param1", float "param2", float "sx", float "sy", float "antiring", float "lut_entries", float "cutoff", bool "sigmoidize", bool "linearize", float "sigmoid_center", float "sigmoid_slope", int "trc", int "cplace", int "device", bool "list_device", float "src_width", float "src_height")
```

#### Parameters:

- input\
    A clip to process.\
    It must be in 8, 16 or 32-bit planar format.

- width\
    The width of the output.

- height\
    The height of the output.

- filter
    The used filter function.
    * spline16 (2 taps)
    * spline36 (3 taps)
    * spline64 (4 taps)
    * nearest (AKA box)
    * bilinear (AKA triangle) (resizable)
    * gaussian (resizable)

    Sinc family (all configured to 3 taps):
    * sinc (unwindowed) (resizable)
    * lanczos (sinc-sinc) (resizable)
    * ginseng (sinc-jinc) (resizable)
    * ewa_jinc (unwindowed) (resizable)
    * ewa_lanczos (jinc-jinc) (resizable)
    * ewa_ginseng (jinc-sinc) (resizable)
    * ewa_hann (jinc-hann) (resizable)

    Spline family:
    * bicubic
    * catmull_rom
    * mitchell
    * robidoux
    * robidouxsharp
    * ewa_robidoux
    * ewa_robidouxsharp

    Default: "ewa_lanczos"

- radius\
    It may be used to adjust the function's radius.\
    Defaults to the the radius needed to represent a single filter lobe (tap).\
    If the function is not resizable, this doesn't have effect.

- clamp\
    Represents a clamping coefficient for negative weights:\
    0.0: No clamping.\
    1.0: Full clamping, i.e. all negative weights will be clamped to 0.\
    Values between 0.0 and 1.0 can be specified to apply only a moderate diminishment of negative weights.\
    Higher values would lead to more blur.\
    Default: 0.0.

- taper\
    Additional taper coefficient.\
    This essentially flattens the function's center.\
    The values within `[-taper, taper]` will return 1.0, with the actual function being squished into the remainder of `[taper, radius]`.\
    Default: 0.0.

- blur\
    Additional blur coefficient.\
    This effectively stretches the kernel, without changing the effective radius of the filter radius.\
    Values significantly below 1.0 may seriously degrade the visual output, and should be used with care.\
    Default: 0.0.

- param1, param2\
    These may be used to adjust the function.\
    Defaults to the function's preferred defaults. if the relevant setting is not tunable, they are ignored entirely.

- sx\
    Cropping of the left edge.\
    Default: 0.0.

- sy\
    Cropping of the top edge.\
    Default: 0.0.

- antiring\
    Antiringing strength.\
    A value of 0.0 disables antiringing, and a value of 1.0 enables full-strength antiringing.\
    Only relevant for separated/orthogonal filters.\
    Default: 0.0.

- lut_entries\
    The precision of the LUT.\
    A value of 64 should be fine for most practical purposes, but higher or lower values may be justified depending on the use case.\
    Must be greater than 0.\
    Default: 64.

- cutoff\
    As a micro-optimization, all samples below this cutoff value will be ignored when updating the cutoff radius.\
    Setting it to a value of 0.0 disables this optimization.\
    Only relevant for polar filters.\
    Default: 0.0.

- sigmoidize, linearize\
    Whether to linearize/sigmoidize before scaling.\
    Only relevant for RGB formats.\
    When sigmodizing, `linearize` should be `true`\
    Default: True.

- sigmoid_center\
    The center (bias) of the sigmoid curve.\
    Must be between 0.0 and 1.0.\
    Default: 0.75.

- sigmoid_slope\
    The slope (steepness) of the sigmoid curve.\
    Must be between 1.0 and 20.0.\
    Default: 6.5.

- trc\
    The colorspace's transfer function (gamma / EOTF) to use for linearizing.\
    0: UNKNOWN

    Standard dynamic range:\
    1: BT_1886 (ITU-R Rec. BT.1886 (CRT emulation + OOTF))\
    2: SRGB (IEC 61966-2-4 sRGB (CRT emulation))\
    3: LINEAR (Linear light content)\
    4: GAMMA18 (Pure power gamma 1.8)\
    5: GAMMA20 (Pure power gamma 2.0)\
    6: GAMMA22 (Pure power gamma 2.2)\
    7: GAMMA24 (Pure power gamma 2.4)\
    8: GAMMA26 (Pure power gamma 2.6)\
    9: GAMMA28 (Pure power gamma 2.8)\
    10: PRO_PHOTO (ProPhoto RGB (ROMM))

    High dynamic range:\
    11: PQ (ITU-R BT.2100 PQ (perceptual quantizer), aka SMPTE ST2048)\
    12: HLG (ITU-R BT.2100 HLG (hybrid log-gamma), aka ARIB STD-B67)\
    13: V_LOG (Panasonic V-Log (VARICAM))\
    14: S_LOG1 (Sony S-Log1)\
    15: S_LOG2 (Sony S-Log2)

    Default: 1.

- cplace\
    Chroma sample position in YUV formats.\
    0: left\
    1: center\
    2: topleft\
    Default: 0.

- device\
    Sets target Vulkan device.\
    Use list_device to get the index of the available devices.\
    By default the default device is selected.

- list_device\
    Whether to draw the devices list on the frame.\
    Default: False.

- src_width\
    Sets the width of the clip before resizing.\
    Must be greater than 0.0.\
    Default: Source width.

- src_height\
    Sets the height of the clip before resizing.\
    Must be greater than 0.0.\
    Default: Source height.

[Back to filters](#filters)

### Shader

#### Usage:

```
libplacebo_Shader(clip input, string shader, int "width", int "height", int "chroma_loc", int "matrix", int "trc",  string "filter", float "radius", float "clamp", float "taper", float "blur", float "param1", float "param2", float "antiring", float "lut_entries", float "cutoff", bool "sigmoidize", bool "linearize", float "sigmoid_center", float "sigmoid_slope", string "shader_param", int "device", bool "list_device")
```

#### Parameters:

- input\
    A clip to process.\
    It must be YUV 16-bit planar format.\
    The output is YUV444P16. This is necessitated by the fundamental design of libplacebo/mpv’s custom shader feature: the shaders aren’t meant (nor written) to be run by themselves, but to be injected at arbitrary points into a [rendering pipeline](https://github.com/mpv-player/mpv/wiki/Video-output---shader-stage-diagram) with RGB output.\
    As such, the user needs to specify the output frame properties, and libplacebo will produce a conforming image, only running the supplied shader if the texture it hooks into is actually rendered. For example, if a shader hooks into the LINEAR texture, it will only be executed when `linearize = true`.

- shader\
    Path to the shader file.

- width\
    The width of the output.\
    Default: Source width.

- height\
    The height of the output.\
    Default: Source height.

- chroma_loc\
    Chroma location to derive chroma shift from.\
    0: UNKNOWN\
    1: LEFT\
    2: CENTER\
    3: TOP_LEFT\
    4: TOP_CENTER\
    5: BOTTOM_LEFT\
    6: BOTTOM_CENTER\
    Default: 1.

- matrix\
    0: UNKNOWN\
    1: BT_601 (ITU-R Rec. BT.601 (SD))\
    2: BT_709 (ITU-R Rec. BT.709 (HD))\
    3: SMPTE_240M (SMPTE-240M)\
    4: BT_2020_NC (ITU-R Rec. BT.2020 (non-constant luminance))\
    5: BT_2020_C (ITU-R Rec. BT.2020 (constant luminance))\
    6: BT_2100_PQ (ITU-R Rec. BT.2100 ICtCp PQ variant)\
    7: BT_2100_HLG (ITU-R Rec. BT.2100 ICtCp HLG variant)\
    8: YCGCO (YCgCo (derived from RGB))
    Default: 2.

- trc\
    The colorspace's transfer function (gamma / EOTF) to use for linearizing.\
    0: UNKNOWN

    Standard dynamic range:\
    1: BT_1886 (ITU-R Rec. BT.1886 (CRT emulation + OOTF))\
    2: SRGB (IEC 61966-2-4 sRGB (CRT emulation))\
    3: LINEAR (Linear light content)\
    4: GAMMA18 (Pure power gamma 1.8)\
    5: GAMMA20 (Pure power gamma 2.0)\
    6: GAMMA22 (Pure power gamma 2.2)\
    7: GAMMA24 (Pure power gamma 2.4)\
    8: GAMMA26 (Pure power gamma 2.6)\
    9: GAMMA28 (Pure power gamma 2.8)\
    10: PRO_PHOTO (ProPhoto RGB (ROMM))

    High dynamic range:\
    11: PQ (ITU-R BT.2100 PQ (perceptual quantizer), aka SMPTE ST2048)\
    12: HLG (ITU-R BT.2100 HLG (hybrid log-gamma), aka ARIB STD-B67)\
    13: V_LOG (Panasonic V-Log (VARICAM))\
    14: S_LOG1 (Sony S-Log1)\
    15: S_LOG2 (Sony S-Log2)

    Default: 1.

- filter
    The used filter function.
    * spline16 (2 taps)
    * spline36 (3 taps)
    * spline64 (4 taps)
    * nearest (AKA box)
    * bilinear (AKA triangle) (resizable)
    * gaussian (resizable)

    Sinc family (all configured to 3 taps):
    * sinc (unwindowed) (resizable)
    * lanczos (sinc-sinc) (resizable)
    * ginseng (sinc-jinc) (resizable)
    * ewa_jinc (unwindowed) (resizable)
    * ewa_lanczos (jinc-jinc) (resizable)
    * ewa_ginseng (jinc-sinc) (resizable)
    * ewa_hann (jinc-hann) (resizable)

    Spline family:
    * bicubic
    * catmull_rom
    * mitchell
    * robidoux
    * robidouxsharp
    * ewa_robidoux
    * ewa_robidouxsharp

    Default: "ewa_lanczos"

- radius\
    It may be used to adjust the function's radius.\
    Defaults to the the radius needed to represent a single filter lobe (tap).\
    If the function is not resizable, this doesn't have effect.

- clamp\
    Represents a clamping coefficient for negative weights:\
    0.0: No clamping.\
    1.0: Full clamping, i.e. all negative weights will be clamped to 0.\
    Values between 0.0 and 1.0 can be specified to apply only a moderate diminishment of negative weights.\
    Higher values would lead to more blur.\
    Default: 0.0.

- taper\
    Additional taper coefficient.\
    This essentially flattens the function's center.\
    The values within `[-taper, taper]` will return 1.0, with the actual function being squished into the remainder of `[taper, radius]`.\
    Default: 0.0.

- blur\
    Additional blur coefficient.\
    This effectively stretches the kernel, without changing the effective radius of the filter radius.\
    Values significantly below 1.0 may seriously degrade the visual output, and should be used with care.\
    Default: 0.0.

- param1, param2\
    These may be used to adjust the function.\
    Defaults to the function's preferred defaults. if the relevant setting is not tunable, they are ignored entirely.

- antiring\
    Antiringing strength.\
    A value of 0.0 disables antiringing, and a value of 1.0 enables full-strength antiringing.\
    Only relevant for separated/orthogonal filters.\
    Default: 0.0.

- lut_entries\
    The precision of the LUT.\
    A value of 64 should be fine for most practical purposes, but higher or lower values may be justified depending on the use case.\
    Must be greater than 0.\
    Default: 64.

- cutoff\
    As a micro-optimization, all samples below this cutoff value will be ignored when updating the cutoff radius.\
    Setting it to a value of 0.0 disables this optimization.\
    Only relevant for polar filters.\
    Default: 0.0.

- sigmoidize, linearize\
    Whether to linearize/sigmoidize before scaling.\
    Only relevant for RGB formats.\
    When sigmodizing, `linearize` should be `true`\
    Default: True.

- sigmoid_center\
    The center (bias) of the sigmoid curve.\
    Must be between 0.0 and 1.0.\
    Default: 0.75.

- sigmoid_slope\
    The slope (steepness) of the sigmoid curve.\
    Must be between 1.0 and 20.0.\
    Default: 6.5.

- shader_param\
    This changes shader's parameter set by `#define XXXX YYYY` on the fly.\
    Format is: `param=value`.\
    It takes up to 4 parameters.\
    The parameter is case sensitive and must be the same as in the shader file.\
    If more than one parameter is specified, the parameters must be separated by space.

    Usage example: if the shader has the following parameters:
    * #define INTENSITY_SIGMA 0.1 //Intensity window size, higher is stronger denoise, must be a positive real number
    * #define SPATIAL_SIGMA 1.0 //Spatial window size, higher is stronger denoise, must be a positive real number

    `shader_param="INTENSITY_SIGMA=0.15 SPATIAL_SIGMA=1.1"`

- device\
    Sets target Vulkan device.\
    Use list_device to get the index of the available devices.\
    By default the default device is selected.

- list_device\
    Whether to draw the devices list on the frame.\
    Default: False.

[Back to filters](#filters)

### Tone mapping

#### Usage:

```
libplacebo_Tonemap(clip input, int "src_csp", float "dst_csp", float "src_max", float "src_min", float "dst_max", float "dst_min", bool "dynamic_peak_detection", float "smoothing_period", float "scene_threshold_low", float "scene_threshold_high", int "intent", int "gamut_mode", int "tone_mapping_function", int "tone_mapping_mode", float "tone_mapping_param", float "tone_mapping_crosstalk", bool "use_dovi", int "device", bool "list_device")
```

#### Parameters:

- input\
    A clip to process.\
    It must be 16-bit planar format. (min. 3 planes)\
    The output is YUV444P16 if the input is YUV.

- src_csp, dst_csp\
    Respectively source and output color space.\
    0: SDR\
    1: HDR10\
    2: HLG\
    3: DOVI\
    Default: src_csp = 1; dst_csp = 0.

    For example, to map from [BT.2020, PQ] (HDR) to traditional [BT.709, BT.1886] (SDR), pass `src_csp=1, dst_csp=0`.

- src_max, src_min, dst_max, dst_min\
    Source max/min and output max/min in nits (cd/m^2).\
    The source values can be derived from props if available.\
    Default: max = 1000 (HDR)/203 (SDR); min = 0.005 (HDR)/0.2023 (SDR)

- dynamic_peak_detection\
    Enables computation of signal stats to optimize HDR tonemapping quality.\
    Default: True.

- smoothing_period\
    Smoothing coefficient for the detected values.\
    This controls the time parameter (tau) of an IIR low pass filter. In other words, it represent the cutoff period (= 1 / cutoff frequency) in frames. Frequencies below this length will be suppressed.\
    This helps block out annoying "sparkling" or "flickering" due to small variations in frame-to-frame brightness.\
    Default: 100.0.

- scene_threshold_low, scene_threshold_high\
    In order to avoid reacting sluggishly on scene changes as a result of the low-pass filter, we disable it when the difference between the current frame brightness and the average frame brightness exceeds a given threshold difference.\
    But rather than a single hard cutoff, which would lead to weird discontinuities on fades, we gradually disable it over a small window of brightness ranges. These parameters control the lower and upper bounds of this window, in dB.\
    To disable this logic entirely, set either one to a negative value.\
    Default: scene_threshold_low = 5.5; scene_threshold_high = 10.0

- intent\
    The rendering intent to use for gamut mapping.\
    0: PERCEPTUAL\
    1: RELATIVE_COLORIMETRIC\
    2: SATURATION\
    3: ABSOLUTE_COLORIMETRIC\
    Default: 1.

- gamut_mode\
    How to handle out-of-gamut colors when changing the content primaries.\
    0: CLIP (Do nothing, simply clip out-of-range colors to the RGB volume)\
    1: WARN (Equal to CLIP but also highlights out-of-gamut colors (by coloring them pink))\
    2: DARKEN (Linearly reduces content brightness to preserves saturated details, followed by clipping the remaining out-of-gamut colors.\
    As the name implies, this makes everything darker, but provides a good balance between preserving details and colors.)\
    3: DESATURATE (Hard-desaturates out-of-gamut colors towards white, while preserving the luminance. Has a tendency to shift colors.)\
    Default: 0.

- tone_mapping_function\
    0: auto (Special tone mapping function that means "automatically pick a good function based on the HDR levels")\
    1: clip (Performs no tone-mapping, just clips out-of-range colors.\
    Retains perfect color accuracy for in-range colors but completely destroys out-of-range information.\
    Does not perform any black point adaptation.)\
    2: bt2390 (EETF from the ITU-R Report BT.2390, a hermite spline roll-off with linear segment.\
    The knee point offset is configurable. Note that this defaults to 1.0, rather than the value of 0.5 from the ITU-R spec.)\
    3: bt2446a (EETF from ITU-R Report BT.2446, method A.\
    Can be used for both forward and inverse tone mapping. Not configurable.)\
    4: spline (Simple spline consisting of two polynomials, joined by a single pivot point.\
    The `tone_mapping_param` gives the pivot point (in PQ space), defaulting to 0.30.\
    Can be used for both forward and inverse tone mapping.)\
    5: reinhard (Simple non-linear, global tone mapping algorithm.\
    Named after Erik Reinhard.\
    The `tone_mapping_param` specifies the local contrast coefficient at the display peak.\
    Essentially, a value of param=0.5 implies that the reference white will be about half as bright as when clipping.\
    Defaults to 0.5, which results in the simplest formulation of this function.)\
    6: mobius (Generalization of the reinhard tone mapping algorithm to support an additional linear slope near black.\
    The tone mapping `tone_mapping_param` indicates the trade-off between the linear section and the non-linear section.\
    Essentially, for param=0.5, every color value below 0.5 will be mapped linearly, with the higher values being non-linearly tone mapped.\
    Values near 1.0 make this curve behave like `clip`, and values near 0.0 make this curve behave like `reinhard`.\
    The default value is 0.3, which provides a good balance between colorimetric accuracy and preserving out-of-gamut details.\
    The name is derived from its function shape (ax+b)/(cx+d), which is known as a Möbius transformation in mathematics.)\
    7: hable (Piece-wise, filmic tone-mapping algorithm developed by John Hable for use in Uncharted 2, inspired by a similar tone-mapping algorithm used by Kodak.\
    Popularized by its use in video games with HDR rendering.\
    Preserves both dark and bright details very well, but comes with the drawback of changing the average brightness quite significantly.\
    This is sort of similar to `reinhard` with `tone_mapping_param` 0.24.)\
    8: gamma (Fits a gamma (power) function to transfer between the source and target color spaces, effectively resulting in a perceptual hard-knee joining two roughly linear sections.\
    This preserves details at all scales fairly accurately, but can result in an image with a muted or dull appearance.\
    The `tone_mapping_param` is used as the cutoff point, defaulting to 0.5.)\
    9: linear (Linearly stretches the input range to the output range, in PQ space.\
    This will preserve all details accurately, but results in a significantly different average brightness.\
    Can be used for inverse tone-mapping in addition to regular tone-mapping.\
    The parameter can be used as an additional linear gain coefficient (defaulting to 1.0).)\
    Default: 0.

- tone_mapping_mode\
    0: AUTO (Picks the best tone-mapping mode based on internal heuristics.)\
    1: RGB (Per-channel tone-mapping in RGB. Guarantees no clipping and heavily desaturates the output, but distorts the colors quite significantly.)\
    2: MAX (Tone-mapping is performed on the brightest component found in the signal.\
    Good at preserving details in highlights, but has a tendency to crush blacks.)\
    3: HYBRID (Tone-map per-channel for highlights and linearly (luma-based) for midtones/shadows, based on a fixed gamma 2.4 coefficient curve.)\
    4: LUMA (Tone-map linearly on the luma component, and adjust (desaturate) the chromaticities to compensate using a simple constant factor.\
    This is essentially the mode used in ITU-R BT.2446 method A.)\
    Default: 0.

- tone_mapping_param\
    The tone-mapping curve parameter.\
    Default: Default for the selected `tone_mapping_function`.

- tone_mapping_crosstalk\
    Extra crosstalk factor to apply before tone-mapping.\
    May help to improve the appearance of very bright, monochromatic highlights.\
    Default: 0.04.

- use_dovi\
    Whether to use the Dolby Vision RPU for ST2086 metadata.\
    Defaults to true when tonemapping from Dolby Vision.

- device\
    Sets target Vulkan device.\
    Use list_device to get the index of the available devices.\
    By default the default device is selected.

- list_device\
    Whether to draw the devices list on the frame.\
    Default: False.

[Back to filters](#filters)

### Building:

- Windows
    ```
    Requirements:
        - Clang-cl (https://github.com/llvm/llvm-project/releases)
        - Vulkan SDK (https://vulkan.lunarg.com/sdk/home#windows)
        - libp2p (https://github.com/sekrit-twc/libp2p)
        - dolby_vision C-lib (https://github.com/quietvoid/dovi_tool/blob/main/dolby_vision/README.md)
        - libplacebo (https://code.videolan.org/videolan/libplacebo)
    ```
    ```
    Steps:
        Install Vulkan SDk.
        Build libp2p.
        Build dolby_vision.
        Building libplacebo (apply this patch https://gist.github.com/Asd-g/f7dc6f88f9a48c3442ebc996f45bc5ce):
            set LIB=%LIB%;C:\VulkanSDK\1.3.224.1\Lib
            meson build -Dvulkan-registry=C:\VulkanSDK\1.3.224.1\share\vulkan\registry\vk.xml --default-library=static --buildtype=release -Ddemos=false -Dopengl=disabled -Dd3d11=disabled
        Use solution files to build avs_libplacebo.
    ```

- Linux
    ```
    Requirements:
        - Vulkan lib
        - libp2p (https://github.com/sekrit-twc/libp2p)
        - dolby_vision C-lib (https://github.com/quietvoid/dovi_tool/blob/main/dolby_vision/README.md)
        - libplacebo (https://code.videolan.org/videolan/libplacebo)
        - AviSynth lib
    ```

[Back to top](#description)
