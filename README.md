## Description

An AviSynth+ plugin interface to [libplacebo](https://code.videolan.org/videolan/libplacebo) - a reusable library for GPU-accelerated image/video processing primitives and shaders.

This is [a port of the VapourSynth plugin vs-placebo](https://github.com/Lypheo/vs-placebo).

### Requirements:

- Vulkan device

- AviSynth+ r3688 (can be downloaded from [here](https://gitlab.com/uvz/AviSynthPlus-Builds) until official release is uploaded) or later

- Microsoft VisualC++ Redistributable Package 2022 (can be downloaded from [here](https://github.com/abbodi1406/vcredist/releases))

### Filters

[Debanding](#debanding)<br>
[Resampling](#resampling)<br>
[Shader](#shader)<br>
[Tone mapping](#tone-mapping)

### Debanding

#### Usage:

```
libplacebo_Deband(clip input, int "iterations", float "threshold", float "radius", float "grainY", float "grainC", int "dither", int "lut_size", bool "temporal", int[] "planes", int "device", bool "list_device", float[] "grain_neutral")
```

#### Parameters:

- input<br>
    A clip to process.<br>
    It must be in 8, 16 or 32-bit planar format.

- iterations<br>
    The number of debanding steps to perform per sample.<br>
    Each step reduces a bit more banding, but takes time to compute.<br>
    Note that the strength of each step falls off very quickly, so high numbers (>4) are practically useless.<br>
    Must be greater than or equal to 0.<br>
    Default: 1.

- threshold<br>
    The debanding filter's cut-off threshold.<br>
    Higher numbers increase the debanding strength dramatically, but progressively diminish image details.<br>
    Must be greater than or equal to 0.0.<br>
    Default: 4.0.

- radius<br>
    The debanding filter's initial radius.<br>
    The radius increases linearly for each iteration.<br>
    A higher radius will find more gradients, but a lower radius will smooth more aggressively.<br>
    Must be radius must be greater than or equal to 0.0.<br>
    Default: 16.0.

- grainY, grainC<br>
    Add some extra noise respectively to the luma and chroma plane.<br>
    This significantly helps cover up remaining quantization artifacts.<br>
    Higher numbers add more noise.<br>
    Note: When debanding HDR sources, even a small amount of grain can result in a very big change to the brightness level.<br>
    It's recommended to either scale this value down or disable it entirely for HDR.<br>
    Must be greater than or equal to 0.0.<br>
    When the clip is RGB, grainC doesn't have effect.<br>
    Default: grainY = 6.0; grainC = grainY.

- dither<br>
    It's valid only for 8-bit clips.<br>
    0: Disabled.

    1: PL_DITHER_BLUE_NOISE<br>
    Dither with blue noise.<br>
    Very high quality, but requires the use of a LUT.<br>
    Warning: Computing a blue noise texture with a large size can be very slow, however this only needs to be performed once. Even so, using this with a `lut_size` greater than `6` is generally ill-advised.

    2: PL_DITHER_ORDERED_LUT<br>
    Dither with an ordered (bayer) dither matrix, using a LUT.<br>
    Low quality, and since this also uses a LUT, there's generally no advantage to picking this instead of `PL_DITHER_BLUE_NOISE`.<br>
    It's mainly there for testing.

    3: PL_DITHER_ORDERED_FIXED<br>
    The same as `PL_DITHER_ORDERED_LUT`, but uses fixed function math instead of a LUT.<br>
    This is faster, but only supports a fixed dither matrix size of 16x16 (equal to a `lut_size` of 4).

    4: PL_DITHER_WHITE_NOISE<br>
    Dither with white noise.<br>
    This does not require a LUT and is fairly cheap to compute.<br>
    Unlike the other modes it doesn't show any repeating patterns either spatially or temporally, but the downside is that this is visually fairly jarring due to the presence of low frequencies in the noise spectrum.

    Default: 1.

- lut_size<br>
    For the dither methods which require the use of a LUT.<br>
    This controls the size of the LUT (base 2).<br>
    Must be less than or equal to 8.<br>
    Default: 6 (64x64).

- temporal<br>
    Enables temporal dithering.<br>
    his reduces the persistence of dithering artifacts by perturbing the dithering matrix per frame.<br>
    Warning: This can cause nasty aliasing artifacts on some LCD screens.<br>
    Default: False.

- planes<br>
    Planes to process.<br>
    1: Return garbage.<br>
    2: Copy plane.<br>
    3: Process plane. Always process planes when the clip is RGB.<br>
    Format is [y, u, v].<br>
    Default: [3, 2, 2].

- device<br>
    Sets target Vulkan device.<br>
    Use list_device to get the index of the available devices.<br>
    By default the default device is selected.

- list_device<br>
    Whether to draw the devices list on the frame.<br>
    Default: False.

- grain_neutral<br>
    "Neutral" grain value for each channel being debanded.<br>
    Grain application will be modulated to avoid disturbing colors close to this value.<br>
    Set this to a value corresponding to black in the relevant colorspace.<br>
    Must be greater than 0.0<br>
    Default: [0, 0, 0].

[Back to filters](#filters)

### Resampling

#### Usage:

```
libplacebo_Resample(clip input, int width, int height, string "filter", float "radius", float "clamp", float "taper", float "blur", float "param1", float "param2", float "sx", float "sy", float "antiring", bool "sigmoidize", bool "linearize", float "sigmoid_center", float "sigmoid_slope", int "trc", int "cplace", int "device", bool "list_device", float "src_width", float "src_height")
```

#### Parameters:

- input<br>
    A clip to process.<br>
    It must be in 8, 16 or 32-bit planar format.

- width<br>
    The width of the output.

- height<br>
    The height of the output.

- filter
    The used filter function.

    * `spline16` (2 taps)
    * `spline36` (3 taps)
    * `spline64` (4 taps)
    * `nearest` (AKA box)
    * `bilinear` (AKA triangle) (resizable)
    * `gaussian` (resizable)

    Sinc family (all configured to 3 taps):
    * `sinc` (unwindowed) (resizable)
    * `lanczos` (sinc-sinc) (resizable)
    * `ginseng` (sinc-jinc) (resizable)
    * `ewa_jinc` (unwindowed) (resizable)
    * `ewa_lanczos` (jinc-jinc) (resizable)
    * `ewa_lanczossharp` (jinc-jinc) (resizable)
    * `ewa_lanczos4sharpest` (jinc-jinc) (resizable)
    * `ewa_ginseng` (jinc-sinc) (resizable)
    * `ewa_hann` (jinc-hann) (resizable)
    * `ewa_hanning` (ewa_hann alias)

    Spline family:
    * `bicubic`
    * `triangle` (bicubic alias)
    * `hermite`
    * `catmull_rom`
    * `mitchell`
    * `mitchell_clamp`
    * `robidoux`
    * `robidouxsharp`
    * `ewa_robidoux`
    * `ewa_robidouxsharp`

    Default: `ewa_lanczos`

- radius<br>
    It may be used to adjust the function's radius.<br>
    Defaults to the the radius needed to represent a single filter lobe (tap).<br>
    If the function is not resizable, this doesn't have effect.<br>
    Must be between 0.0..16.0.

- clamp<br>
    Represents a clamping coefficient for negative weights:<br>
    0.0: No clamping.<br>
    1.0: Full clamping, i.e. all negative weights will be clamped to 0.<br>
    Values between 0.0 and 1.0 can be specified to apply only a moderate diminishment of negative weights.<br>
    Higher values would lead to more blur.<br>
    Default: 0.0.

- taper<br>
    Additional taper coefficient.<br>
    This essentially flattens the function's center.<br>
    The values within `[-taper, taper]` will return 1.0, with the actual function being squished into the remainder of `[taper, radius]`.<br>
    Must be between 0.0..1.0.<br>
    Default: 0.0.

- blur<br>
    Additional blur coefficient.<br>
    This effectively stretches the kernel, without changing the effective radius of the filter radius.<br>
    Values significantly below 1.0 may seriously degrade the visual output, and should be used with care.<br>
    Must be between 0.0..100.0.<br>
    Default: 0.0.

- param1, param2<br>
    These may be used to adjust the function.<br>
    Defaults to the function's preferred defaults. if the relevant setting is not tunable, they are ignored entirely.

- sx<br>
    Cropping of the left edge.<br>
    Default: 0.0.

- sy<br>
    Cropping of the top edge.<br>
    Default: 0.0.

- antiring<br>
    Antiringing strength.<br>
    A value of 0.0 disables antiringing, and a value of 1.0 enables full-strength antiringing.<br>
    Only relevant for separated/orthogonal filters.<br>
    Default: 0.0.

- sigmoidize, linearize<br>
    Whether to linearize/sigmoidize before scaling.<br>
    Only relevant for RGB formats.<br>
    When sigmodizing, `linearize` should be `true`<br>
    Default: True.

- sigmoid_center<br>
    The center (bias) of the sigmoid curve.<br>
    Must be between 0.0 and 1.0.<br>
    Default: 0.75.

- sigmoid_slope<br>
    The slope (steepness) of the sigmoid curve.<br>
    Must be between 1.0 and 20.0.<br>
    Default: 6.5.

- trc<br>
    The colorspace's transfer function (gamma / EOTF) to use for linearizing.<br>
    0: UNKNOWN

    Standard dynamic range:<br>
    1: BT_1886 (ITU-R Rec. BT.1886 (CRT emulation + OOTF))<br>
    2: SRGB (IEC 61966-2-4 sRGB (CRT emulation))<br>
    3: LINEAR (Linear light content)<br>
    4: GAMMA18 (Pure power gamma 1.8)<br>
    5: GAMMA20 (Pure power gamma 2.0)<br>
    6: GAMMA22 (Pure power gamma 2.2)<br>
    7: GAMMA24 (Pure power gamma 2.4)<br>
    8: GAMMA26 (Pure power gamma 2.6)<br>
    9: GAMMA28 (Pure power gamma 2.8)<br>
    10: PRO_PHOTO (ProPhoto RGB (ROMM))<br>
    11: ST428 (Digital Cinema Distribution Master (XYZ))

    High dynamic range:<br>
    12: PQ (ITU-R BT.2100 PQ (perceptual quantizer), aka SMPTE ST2048)<br>
    13: HLG (ITU-R BT.2100 HLG (hybrid log-gamma), aka ARIB STD-B67)<br>
    14: V_LOG (Panasonic V-Log (VARICAM))<br>
    15: S_LOG1 (Sony S-Log1)<br>
    16: S_LOG2 (Sony S-Log2)

    Default: 1.

- cplace<br>
    Chroma sample position in YUV formats.<br>
    0: left<br>
    1: center<br>
    2: topleft<br>
    Default: 0.

- device<br>
    Sets target Vulkan device.<br>
    Use list_device to get the index of the available devices.<br>
    By default the default device is selected.

- list_device<br>
    Whether to draw the devices list on the frame.<br>
    Default: False.

- src_width<br>
    Sets the width of the clip before resizing.<br>
    Must be greater than 0.0.<br>
    Default: Source width.

- src_height<br>
    Sets the height of the clip before resizing.<br>
    Must be greater than 0.0.<br>
    Default: Source height.

[Back to filters](#filters)

### Shader

#### Usage:

```
libplacebo_Shader(clip input, string shader, int "width", int "height", int "chroma_loc", int "matrix", int "trc",  string "filter", float "radius", float "clamp", float "taper", float "blur", float "param1", float "param2", float "antiring", bool "sigmoidize", bool "linearize", float "sigmoid_center", float "sigmoid_slope", string "shader_param", int "device", bool "list_device")
```

#### Parameters:

- input<br>
    A clip to process.<br>
    It must be YUV 16-bit planar format.<br>
    The output is YUV444P16. This is necessitated by the fundamental design of libplacebo/mpv’s custom shader feature: the shaders aren’t meant (nor written) to be run by themselves, but to be injected at arbitrary points into a [rendering pipeline](https://github.com/mpv-player/mpv/wiki/Video-output---shader-stage-diagram) with RGB output.<br>
    As such, the user needs to specify the output frame properties, and libplacebo will produce a conforming image, only running the supplied shader if the texture it hooks into is actually rendered. For example, if a shader hooks into the LINEAR texture, it will only be executed when `linearize = true`.

- shader<br>
    Path to the shader file.

- width<br>
    The width of the output.<br>
    Default: Source width.

- height<br>
    The height of the output.<br>
    Default: Source height.

- chroma_loc<br>
    Chroma location to derive chroma shift from.<br>
    0: UNKNOWN<br>
    1: LEFT<br>
    2: CENTER<br>
    3: TOP_LEFT<br>
    4: TOP_CENTER<br>
    5: BOTTOM_LEFT<br>
    6: BOTTOM_CENTER<br>
    Default: 1.

- matrix<br>
    0: UNKNOWN<br>
    1: BT_601 (ITU-R Rec. BT.601 (SD))<br>
    2: BT_709 (ITU-R Rec. BT.709 (HD))<br>
    3: SMPTE_240M (SMPTE-240M)<br>
    4: BT_2020_NC (ITU-R Rec. BT.2020 (non-constant luminance))<br>
    5: BT_2020_C (ITU-R Rec. BT.2020 (constant luminance))<br>
    6: BT_2100_PQ (ITU-R Rec. BT.2100 ICtCp PQ variant)<br>
    7: BT_2100_HLG (ITU-R Rec. BT.2100 ICtCp HLG variant)<br>
    8: YCGCO (YCgCo (derived from RGB))
    Default: 2.

- trc<br>
    The colorspace's transfer function (gamma / EOTF) to use for linearizing.<br>
    0: UNKNOWN

    Standard dynamic range:<br>
    1: BT_1886 (ITU-R Rec. BT.1886 (CRT emulation + OOTF))<br>
    2: SRGB (IEC 61966-2-4 sRGB (CRT emulation))<br>
    3: LINEAR (Linear light content)<br>
    4: GAMMA18 (Pure power gamma 1.8)<br>
    5: GAMMA20 (Pure power gamma 2.0)<br>
    6: GAMMA22 (Pure power gamma 2.2)<br>
    7: GAMMA24 (Pure power gamma 2.4)<br>
    8: GAMMA26 (Pure power gamma 2.6)<br>
    9: GAMMA28 (Pure power gamma 2.8)<br>
    10: PRO_PHOTO (ProPhoto RGB (ROMM))<br>
    11: ST428 (Digital Cinema Distribution Master (XYZ))

    High dynamic range:<br>
    12: PQ (ITU-R BT.2100 PQ (perceptual quantizer), aka SMPTE ST2048)<br>
    13: HLG (ITU-R BT.2100 HLG (hybrid log-gamma), aka ARIB STD-B67)<br>
    14: V_LOG (Panasonic V-Log (VARICAM))<br>
    15: S_LOG1 (Sony S-Log1)<br>
    16: S_LOG2 (Sony S-Log2)

    Default: 1.

- filter
    The used filter function.

    * `spline16` (2 taps)
    * `spline36` (3 taps)
    * `spline64` (4 taps)
    * `nearest` (AKA box)
    * `bilinear` (AKA triangle) (resizable)
    * `gaussian` (resizable)

    Sinc family (all configured to 3 taps):
    * `sinc` (unwindowed) (resizable)
    * `lanczos` (sinc-sinc) (resizable)
    * `ginseng` (sinc-jinc) (resizable)
    * `ewa_jinc` (unwindowed) (resizable)
    * `ewa_lanczos` (jinc-jinc) (resizable)
    * `ewa_lanczossharp` (jinc-jinc) (resizable)
    * `ewa_lanczos4sharpest` (jinc-jinc) (resizable)
    * `ewa_ginseng` (jinc-sinc) (resizable)
    * `ewa_hann` (jinc-hann) (resizable)
    * `ewa_hanning` (ewa_hann alias)

    Spline family:
    * `bicubic`
    * `triangle` (bicubic alias)
    * `hermite`
    * `catmull_rom`
    * `mitchell`
    * `mitchell_clamp`
    * `robidoux`
    * `robidouxsharp`
    * `ewa_robidoux`
    * `ewa_robidouxsharp`

    Default: `ewa_lanczos`

- radius<br>
    It may be used to adjust the function's radius.<br>
    Defaults to the the radius needed to represent a single filter lobe (tap).<br>
    If the function is not resizable, this doesn't have effect.<br>
    Must be between 0.0..16.0.

- clamp<br>
    Represents a clamping coefficient for negative weights:<br>
    0.0: No clamping.<br>
    1.0: Full clamping, i.e. all negative weights will be clamped to 0.<br>
    Values between 0.0 and 1.0 can be specified to apply only a moderate diminishment of negative weights.<br>
    Higher values would lead to more blur.<br>
    Default: 0.0.

- taper<br>
    Additional taper coefficient.<br>
    This essentially flattens the function's center.<br>
    The values within `[-taper, taper]` will return 1.0, with the actual function being squished into the remainder of `[taper, radius]`.<br>
    Must be between 0.0..1.0.<br>
    Default: 0.0.

- blur<br>
    Additional blur coefficient.<br>
    This effectively stretches the kernel, without changing the effective radius of the filter radius.<br>
    Values significantly below 1.0 may seriously degrade the visual output, and should be used with care.<br>
    Must be between 0.0..100.0.<br>
    Default: 0.0.

- param1, param2<br>
    These may be used to adjust the function.<br>
    Defaults to the function's preferred defaults. if the relevant setting is not tunable, they are ignored entirely.

- antiring<br>
    Antiringing strength.<br>
    A value of 0.0 disables antiringing, and a value of 1.0 enables full-strength antiringing.<br>
    Only relevant for separated/orthogonal filters.<br>
    Default: 0.0.

- sigmoidize, linearize<br>
    Whether to linearize/sigmoidize before scaling.<br>
    Only relevant for RGB formats.<br>
    When sigmodizing, `linearize` should be `true`<br>
    Default: True.

- sigmoid_center<br>
    The center (bias) of the sigmoid curve.<br>
    Must be between 0.0 and 1.0.<br>
    Default: 0.75.

- sigmoid_slope<br>
    The slope (steepness) of the sigmoid curve.<br>
    Must be between 1.0 and 20.0.<br>
    Default: 6.5.

- shader_param<br>
    This changes shader's parameter set by `#define XXXX YYYY` on the fly.<br>
    Format is: `param=value`.<br>
    The parameter is case sensitive and must be the same as in the shader file.<br>
    If more than one parameter is specified, the parameters must be separated by space.

    Usage example: if the shader has the following parameters:
    * #define INTENSITY_SIGMA 0.1 //Intensity window size, higher is stronger denoise, must be a positive real number
    * #define SPATIAL_SIGMA 1.0 //Spatial window size, higher is stronger denoise, must be a positive real number

    `shader_param="INTENSITY_SIGMA=0.15 SPATIAL_SIGMA=1.1"`

- device<br>
    Sets target Vulkan device.<br>
    Use list_device to get the index of the available devices.<br>
    By default the default device is selected.

- list_device<br>
    Whether to draw the devices list on the frame.<br>
    Default: False.

[Back to filters](#filters)

### Tone mapping

#### Usage:

```
libplacebo_Tonemap(clip input, int "src_csp", float "dst_csp", float "src_max", float "src_min", float "dst_max", float "dst_min", bool "dynamic_peak_detection", float "smoothing_period", float "scene_threshold_low", float "scene_threshold_high", float "percentile", float "black_cutoff", string "gamut_mapping_mode", string "tone_mapping_function", string[] "tone_constants", int "metadata", float "contrast_recovery", float "contrast_smoothness", bool "visualize_lut", bool "show_clipping", bool "use_dovi", int "device", bool "list_device", string "cscale", string "lut", int "lut_type", int "dst_prim", int "dst_trc", int "dst_sys")
```

#### Parameters:

- input<br>
    A clip to process.<br>
    It must be 16-bit planar format. (min. 3 planes)<br>
    The output is YUV444P16 if the input is YUV.

- src_csp, dst_csp<br>
    Respectively source and output color space.<br>
    0: SDR<br>
    1: HDR10<br>
    2: HLG<br>
    3: DOVI<br>
    Default: src_csp = 1; dst_csp = 0.

    For example, to map from [BT.2020, PQ] (HDR) to traditional [BT.709, BT.1886] (SDR), pass `src_csp=1, dst_csp=0`.

- src_max, src_min, dst_max, dst_min<br>
    Source max/min and output max/min in nits (cd/m^2).<br>
    The source values can be derived from props if available.<br>
    Default: max = 1000 (HDR)/203 (SDR); min = 0.005 (HDR)/0.2023 (SDR)

- dynamic_peak_detection<br>
    Enables computation of signal stats to optimize HDR tonemapping quality.<br>
    Default: True.

- smoothing_period<br>
    Smoothing coefficient for the detected values.<br>
    This controls the time parameter (tau) of an IIR low pass filter. In other words, it represent the cutoff period (= 1 / cutoff frequency) in frames. Frequencies below this length will be suppressed.<br>
    This helps block out annoying "sparkling" or "flickering" due to small variations in frame-to-frame brightness.<br>
    Default: 20.0.

- scene_threshold_low, scene_threshold_high<br>
    In order to avoid reacting sluggishly on scene changes as a result of the low-pass filter, we disable it when the difference between the current frame brightness and the average frame brightness exceeds a given threshold difference.<br>
    But rather than a single hard cutoff, which would lead to weird discontinuities on fades, we gradually disable it over a small window of brightness ranges. These parameters control the lower and upper bounds of this window, in dB.<br>
    To disable this logic entirely, set either one to a negative value.<br>
    Default: scene_threshold_low = 1.0; scene_threshold_high = 3.0

- percentile<br>
    Which percentile of the input image brightness histogram to consider as the true peak of the scene.<br>
    If this is set to 100 (or 0), the brightest pixel is measured. Otherwise, the top of the frequency distribution is progressively cut off.<br>
    Setting this too low will cause clipping of very bright details, but can improve the dynamic brightness range of scenes with very bright isolated highlights.<br>
    The default of 99.995% is very conservative and should cause no major issues in typical content.

- black_cutoff<br>
    Black cutoff strength.<br>
    To prevent unnatural pixel shimmer and excessive darkness in mostly black scenes, as well as avoid black bars from affecting the content, (smoothly) cut off any value below this (PQ%) threshold.<br>
    Setting this to 0.0 (or a negative value) disables this functionality.<br>
    Default: 1.0 (1% PQ).

- gamut_mapping_mode<br>
    Specifies the algorithm used for reducing the gamut of images for the target display, after any tone mapping is done.<br>

    * `clip`: Hard-clip to the gamut (per-channel). Very low quality, but free.

    * `perceptual`: Performs a perceptually balanced gamut mapping using a soft knee function to roll-off clipped regions, and a hue shifting function to preserve saturation.

    * `softclip`: Performs a perceptually balanced gamut mapping using a soft knee function to roll-off clipped regions, and a hue shifting function to preserve saturation.

    * `relative`: Performs relative colorimetric clipping, while maintaining an exponential relationship between brightness and chromaticity.

    * `saturation`: Performs simple RGB->RGB saturation mapping. The input R/G/B channels are mapped directly onto the output R/G/B channels. Will never clip, but will distort all hues and/or result in a faded look.

    * `absolute`: Performs absolute colorimetric clipping. Like `relative`, but does not adapt the white point.

    * `desaturate`: Performs constant-luminance colorimetric clipping, desaturing colors towards white until they're in-range.

    * `darken`: Uniformly darkens the input slightly to prevent clipping on blown-out highlights, then clamps colorimetrically to the input gamut boundary, biased slightly to preserve chromaticity over luminance.

    * `highlight`: Performs no gamut mapping, but simply highlights out-of-gamut pixels.

    * `linear`: Linearly/uniformly desaturates the image in order to bring the entire image into the target gamut.

    Default: `perceptual`.

- tone_mapping_function

    * `clip`: Performs no tone-mapping, just clips out-of-range colors.<br>
        Retains perfect color accuracy for in-range colors but completely destroys out-of-range information.<br>
        Does not perform any black point adaptation.

    * `st2094-40`: EETF from SMPTE ST 2094-40 Annex B, which uses the provided OOTF based on Bezier curves to perform tone-mapping.<br>
        The OOTF used is adjusted based on the ratio between the targeted and actual display peak luminances.<br>
        In the absence of HDR10+ metadata, falls back to a simple constant bezier curve.<br>

    * `st2094-10`: EETF from SMPTE ST 2094-10 Annex B.2, which takes into account the input signal average luminance in addition to the maximum/minimum.<br>
        Note: This does *not* currently include the subjective gain/offset/gamma controls defined in Annex B.3.

    * `bt2390`: EETF from the ITU-R Report BT.2390, a hermite spline roll-off with linear segment.<br>

    * `bt2446a`: EETF from ITU-R Report BT.2446, method A.<br>
        Can be used for both forward and inverse tone mapping.

    * `spline`: Simple spline consisting of two polynomials, joined by a single pivot point.<br>
        Simple spline consisting of two polynomials, joined by a single pivot point, which is tuned based on the source scene average brightness (taking into account HDR10+ metadata if available).<br>
        This function can be used for both forward and inverse tone mapping.

    * `reinhard`: Very simple non-linear curve.<br>
        Named after Erik Reinhard.<br>

    * `mobius`: Generalization of the `reinhard` tone mapping algorithm to support an additional linear slope near black.<br>
        The name is derived from its function shape `(ax+b)/(cx+d)`, which is known as a Möbius transformation.<br>
        This function is considered legacy/low-quality, and should not be used.

    * `hable`: Piece-wise, filmic tone-mapping algorithm developed by John Hable for use in Uncharted 2, inspired by a similar tone-mapping algorithm used by Kodak.<br>
        Popularized by its use in video games with HDR rendering.<br>
        Preserves both dark and bright details very well, but comes with the drawback of changing the average brightness quite significantly.<br>
        This is sort of similar to `reinhard` with `reinhard_contrast=0.24`.<br>
        This function is considered legacy/low-quality, and should not be used.

    * `gamma`: Fits a gamma (power) function to transfer between the source and target color spaces, effectively resulting in a perceptual hard-knee joining two roughly linear sections.<br>
        This preserves details at all scales fairly accurately, but can result in an image with a muted or dull appearance.<br>
        This function is considered legacy/low-quality and should not be used.

    * `linear`: Linearly stretches the input range to the output range, in PQ space.<br>
        This will preserve all details accurately, but results in a significantly different average brightness.<br>
        Can be used for inverse tone-mapping in addition to regular tone-mapping.<br>

    * `linearlight`: Like `linear`, but in linear light (instead of PQ).<br>
        Works well for small range adjustments but may cause severe darkening when downconverting from e.g. 10k nits to SDR.

    Default: `bt2390`.

- tone_constants<br>
    Tone mapping constants for tuning `tone_mapping_function`.<br>
    Format is `tone_constants=["option=xxx", "option1=xxx", "option2=xxx"]`. For example, `tone_constants=["exposure=0.25"]`.

    * `knee_adaptation`: Configures the knee point, as a ratio between the source average and target average (in PQ space).<br>
        An adaptation of 1.0 always adapts the source scene average brightness to the (scaled) target average, while a value of 0.0 never modifies scene brightness.<br>
        Must be between 0.0..1.0.<br>
        Affects all methods that use the ST2094 knee point determination (currently ST2094-40, ST2094-10 and spline).<br>
        Default: 0.4.

    * `knee_minimum`, `knee_maximum`: Configures the knee point minimum and maximum, respectively, as a percentage of the PQ luminance range.<br>
        Provides a hard limit on the knee point chosen by `knee_adaptation`.<br>
        `knee_minimum` must be between 0.0..0.5.<br>
        `knee_maximum` must be between 0.5..1.0.<br>
        Default: `knee_minimum` 0.1; `knee_maximum` 0.8.

    * `knee_default`: Default knee point to use in the absence of source scene average metadata.<br>
        Normally, this is ignored in favor of picking the knee point as the (relative) source scene average brightness level.<br>
        Must be between `knee_minimum` and `knee_maximum`.<br>
        Default: 0.4.

    * `knee_offset`: Knee point offset (for BT.2390 only).<br>
        Note that a value of 0.5 is the spec-defined default behavior, which differs from the libplacebo default of 1.0.<br>
        Must be between 0.5..2.0.

    * `slope_tuning`, `slope_offset`: For the single-pivot polynomial (spline) function, this controls the coefficients used to tune the slope of the curve.<br>
        This tuning is designed to make the slope closer to 1.0 when the difference in peaks is low, and closer to linear when the difference between peaks is high.<br>
        `slope_tuning` must be between 0.0..10.0.<br>
        `slope_offset` must be between 0.0..1.0.<br>
        Default: `slope_tuning` 1.5; `slope_offset` 0.2.

    * `spline_contrast`: Contrast setting for the spline function.<br>
        Higher values make the curve steeper (closer to `clip`), preserving midtones at the cost of losing shadow/highlight details, while lower values make the curve shallowed (closer to `linear`), preserving highlights at the cost of losing midtone contrast.<br>
        Values above 1.0 are possible, resulting in an output with more contrast than the input.<br>
        Must be between 0.0..1.5.<br>
        Default: 0.5.

    * `reinhard_contrast`: For the reinhard function, this specifies the local contrast coefficient at the display peak.<br>
        Essentially, a value of 0.5 implies that the reference white will be about half as bright as when clipping. (0,1).
        Must be between 0.0..1.0.<br>
        Default: 0.5.

    * `linear_knee`: For legacy functions (`mobius`, `gamma`) which operate on linear light, this directly sets the corresponding knee point.<br>
        Must be between 0.0..1.0<br>
        Default: 0.3.

    * `exposure`: For linear methods (`linear`, `linearlight`), this controls the linear exposure/gain applied to the image.<br>
        Must be between 0.0..10.0.<br>
        Default: 1.0.

- metadata<br>
    Data source to use when tone-mapping.<br>
    Setting this to a specific value allows overriding the default metadata preference logic.<br>
    0: ANY<br>
    1: NONE<br>
    2: HDR10 (HDR10 static mastering display metadata)<br>
    3: HDR10PLUS (HDR10+ dynamic metadata)<br>
    4: CIE_Y (CIE Y derived dynamic luminance metadata)

- contrast_recovery<br>
    Contrast recovery strength.<br>
    If set to a value above 0.0, the source image will be divided into high-frequency and low-frequency components, and a portion of the high-frequency image is added back onto the tone-mapped output.<br>
    May cause excessive ringing artifacts for some HDR sources, but can improve the subjective sharpness and detail left over in the image after tone-mapping.<br>
    Must be equal to or greater than 0.0.<br>
    Default: 0.3.

- contrast_smoothness<br>
    Contrast recovery lowpass kernel size.<br>
    Increasing or decreasing this will affect the visual appearance substantially.<br>
    Must be equal to or greater than 0.0.<br>
    Default: 3.5.

- visualize_lut<br>
    Visualize the tone-mapping curve / LUT. (PQ-PQ graph)<br>
    Default: False.

- show_clipping<br>
    Graphically highlight hard-clipped pixels during tone-mapping (i.e. pixels that exceed the claimed source luminance range).<br>
    Note that the difference between this and `gamut_mode=1` is that the latter only shows out-of-gamut colors (that are inside the monitor brightness range), while this shows out-of-range colors (regardless of whether or not they're in-gamut).<br>
    Default: False.

- use_dovi<br>
    Whether to use the Dolby Vision RPU for ST2086 metadata.<br>
    Defaults to true when tonemapping from Dolby Vision.

- device<br>
    Sets target Vulkan device.<br>
    Use list_device to get the index of the available devices.<br>
    By default the default device is selected.

- list_device<br>
    Whether to draw the devices list on the frame.<br>
    Default: False.

- cscale<br>
    The scaler for chroma planes.<br>
    This is used when the input is YUV420/YUV422.

    * `spline16` (2 taps)
    * `spline36` (3 taps)
    * `spline64` (4 taps)
    * `nearest` (AKA box)
    * `bilinear` (AKA triangle) (resizable)
    * `gaussian` (resizable)

    Sinc family (all configured to 3 taps):
    * `sinc` (unwindowed) (resizable)
    * `lanczos` (sinc-sinc) (resizable)
    * `ginseng` (sinc-jinc) (resizable)
    * `ewa_jinc` (unwindowed) (resizable)
    * `ewa_lanczos` (jinc-jinc) (resizable)
    * `ewa_lanczossharp` (jinc-jinc) (resizable)
    * `ewa_lanczos4sharpest` (jinc-jinc) (resizable)
    * `ewa_ginseng` (jinc-sinc) (resizable)
    * `ewa_hann` (jinc-hann) (resizable)
    * `ewa_hanning` (ewa_hann alias)

    Spline family:
    * `bicubic`
    * `triangle` (bicubic alias)
    * `hermite`
    * `catmull_rom`
    * `mitchell`
    * `mitchell_clamp`
    * `robidoux`
    * `robidouxsharp`
    * `ewa_robidoux`
    * `ewa_robidouxsharp`

    Default: `spline36`.

- lut<br>
    Path to the color mapping LUT.<br>
    If present, this will be applied as part of the image being rendered.<br>
    `src_csp` and `dst_csp` should be used to indicate the color spaces.<br>
    Default: not specified.

- lut_type<br>
    Controls the interpretation of color values fed to and from the LUT.<br>
    1: native (Applied to raw image contents in its native RGB colorspace (non-linear light), before conversion to the output color space.)<br>
    2: normalized (Applied to the normalized RGB image contents, in linear light, before conversion to the output color space.)<br>
    3: conversion (Fully replaces the conversion from the input color space to the output color space. It overrides options related to tone mapping and output colorimetry (dst_prim, dst_trc etc.))<br>
    Default: 3.

- dst_prim<br>
    Target primaries.<br>
    `dst_trc` must be also specified.<br>
    `dst_csp` has no effect.

    Standard gamut:<br>
    1: BT_601_525 (ITU-R Rec. BT.601 (525-line = NTSC, SMPTE-C))<br>
    2: BT_601_625 (ITU-R Rec. BT.601 (625-line = PAL, SECAM))<br>
    3: BT_709 (ITU-R Rec. BT.709 (HD), also sRGB)<br>
    4: BT_470M (ITU-R Rec. BT.470 M)<br>
    5: EBU_3213 (EBU Tech. 3213-E / JEDEC P22 phosphors)<br>
    Wide gamut:<br>
    6: BT_2020 (ITU-R Rec. BT.2020 (UltraHD))<br>
    7: APPLE (Apple RGB)<br>
    8: ADOBE (Adobe RGB (1998))<br>
    9: PRO_PHOTO (ProPhoto RGB (ROMM))<br>
    10: CIE_1931 (CIE 1931 RGB primaries)<br>
    11: DCI_P3 (DCI-P3 (Digital Cinema))<br>
    12: DISPLAY_P3 (DCI-P3 (Digital Cinema) with D65 white point)<br>
    13: V_GAMUT (Panasonic V-Gamut (VARICAM))<br>
    14: S_GAMUT (Sony S-Gamut)<br>
    15: FILM_C (Traditional film primaries with Illuminant C)<br>
    16: ACES_AP0 (ACES Primaries #0 (ultra wide))<br>
    17: ACES_AP1 (ACES Primaries #1)

    Default: not specified.

- dst_trc<br>
    Target transfer function.<br>
    `dst_prim` must be also specified.<br>
    `dst_csp` has no effect.

    Standard dynamic range:<br>
    1: BT_1886 (ITU-R Rec. BT.1886 (CRT emulation + OOTF))<br>
    2: SRGB (IEC 61966-2-4 sRGB (CRT emulation))<br>
    3: LINEAR (Linear light content)<br>
    4: GAMMA18 (Pure power gamma 1.8)<br>
    5: GAMMA20 (Pure power gamma 2.0)<br>
    6: GAMMA22 (Pure power gamma 2.2)<br>
    7: GAMMA24 (Pure power gamma 2.4)<br>
    8: GAMMA26 (Pure power gamma 2.6)<br>
    9: GAMMA28 (Pure power gamma 2.8)<br>
    10: PRO_PHOTO (ProPhoto RGB (ROMM))<br>
    11: ST428 (Digital Cinema Distribution Master (XYZ))<br>
    High dynamic range:<br>
    12: PQ (ITU-R BT.2100 PQ (perceptual quantizer), aka SMPTE ST2048)<br>
    13: HLG (ITU-R BT.2100 HLG (hybrid log-gamma), aka ARIB STD-B67)<br>
    14: V_LOG (Panasonic V-Log (VARICAM))<br>
    15: S_LOG1 (Sony S-Log1)<br>
    16: S_LOG2 (Sony S-Log2)<br>

    Default: not specified.

- dst_sys<br>
    The underlying color representation.<br>
    This has no effect if both `dst_prim` and `dst_trc` are not specified.<br>
    This has effect only for YUV input.<br>
    1: BT_601 (ITU-R Rec. BT.601 (SD))<br>
    2: BT_709 (ITU-R Rec. BT.709 (HD))<br>
    3: SMPTE_240M (SMPTE-240M)<br>
    4: BT_2020_NC (ITU-R Rec. BT.2020 (non-constant luminance))<br>
    5: BT_2020_C (ITU-R Rec. BT.2020 (constant luminance))<br>
    6: BT_2100_PQ (ITU-R Rec. BT.2100 ICtCp PQ variant)<br>
    7: BT_2100_HLG (ITU-R Rec. BT.2100 ICtCp HLG variant)<br>
    8: DOLBYVISION (Dolby Vision (see pl_dovi_metadata))<br>
    9: YCGCO (YCgCo (derived from RGB))<br>
    Default: not specified.

[Back to filters](#filters)

### Building:

```
Requirements:
    - CMake
    - Ninja
    - Vulkan SDK (https://vulkan.lunarg.com/sdk)
    - Clang-cl (https://github.com/llvm/llvm-project/releases) (Windows)
```

```
Steps:
    Install Vulkan SDk.

    Clone the repo:
        git clone --recurse-submodules --depth 1 --shallow-submodules https://github.com/Asd-g/avslibplacebo

    Set prefix:
        cd avslibplacebo
        set prefix="%cd%\deps" (Windows)
        prefix="$(pwd)/deps" (Linux)

    Build dolby_vision:
        cd dovi_tool/dolby_vision
        cargo install cargo-c
        cargo cinstall --release --prefix %prefix% (Windows)
        cargo cinstall --release --prefix $prefix (Linux)

    Building libplacebo:
        cd ../../libplacebo
        set LIB=%LIB%;C:\VulkanSDK\1.3.268.0\Lib (Windows)
        meson setup build -Dvulkan-registry=C:\VulkanSDK\1.3.283.0\share\vulkan\registry\vk.xml --default-library=static --buildtype=release -Ddemos=false -Dopengl=disabled -Dd3d11=disabled --prefix=%prefix% (Windows)
        meson setup build --default-library=static --buildtype=release -Ddemos=false -Dopengl=disabled -Dd3d11=disabled --prefix=$prefix (Linux)
        ninja -C build
        ninja -C build install

    Building plugin:
        cd ../
        cmake -B build -G Ninja -DCMAKE_PREFIX_PATH="c:\VulkanSDK\1.3.283.0;%prefix%" (Windows)
        cmake -B build -G Ninja -DCMAKE_PREFIX_PATH=$prefix (Linux)
        ninja -C build
```

[Back to top](#description)
