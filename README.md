## Description

An AviSynth+ plugin interface to [libplacebo](https://code.videolan.org/videolan/libplacebo) - a reusable library for GPU-accelerated image/video processing primitives and shaders.

This is [a port of the VapourSynth plugin vs-placebo](https://github.com/Lypheo/vs-placebo).

### Requirements:

- AviSynth+ 3.6 or later

- Microsoft VisualC++ Redistributable Package 2022 (can be downloaded from [here](https://github.com/abbodi1406/vcredist/releases))

### Filters

[Debanding](#debanding)\
[Resampling](#resampling)\

### Debanding

#### Usage:

```
libplacebo_Deband(clip input, int "iterations", float "threshold", float "radius", float "grain", int "dither", int "lut_size", bool "temporal", int[] "planes", int "device", bool "list_device")
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

- grain\
    Add some extra noise to the image.\
    This significantly helps cover up remaining quantization artifacts.\
    Higher numbers add more noise.\
    Note: When debanding HDR sources, even a small amount of grain can result in a very big change to the brightness level.\
    It's recommended to either scale this value down or disable it entirely for HDR.\
    Must be greater than or equal to 0.0.\
    Default: 6.0.

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

[Back to filters](#filters)

### Resampling

#### Usage:

```
libplacebo_Resample(clip input, int width, int height, string "filter", float "radius", float "clamp", float "taper", float "blur", float "param1", float "param2", float "sx", float "sy", float "antiring", float "lut_entries", float "cutoff", bool "sigmoidize", bool "linearize", float "sigmoid_center", float "sigmoid_slope", int "trc", int "cplace", int "device", bool "list_device")
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
    * box (AKA nearest)
    * triangle (AKA bilinear)
    * gaussian

    Sinc family (all configured to 3 taps):
    * sinc (unwindowed)
    * lanczos (sinc-sinc)
    * ginseng (sinc-jinc)
    * ewa_jinc (unwindowed)
    * ewa_lanczos (jinc-jinc)
    * ewa_ginseng (jinc-sinc)
    * ewa_hann (jinc-hann)
    * haasnsoft (blurred ewa_hann)

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
    Represents a clamping coefficient for negative weights:
    0.0: No clamping.\
    1.0: Full clamping, i.e. all negative weights will be clamped to 0.\
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
    The colorspace's transfer function (gamma / EOTF) to use for linearizing:
    0. UNKNOWN

    Standard dynamic range:
    1. BT_1886 (ITU-R Rec. BT.1886 (CRT emulation + OOTF))
    2. SRGB (IEC 61966-2-4 sRGB (CRT emulation))
    3. LINEAR (Linear light content)
    4. GAMMA18 (Pure power gamma 1.8)
    5. GAMMA20 (Pure power gamma 2.0)
    6. GAMMA22 (Pure power gamma 2.2)
    7. GAMMA24 (Pure power gamma 2.4)
    8. GAMMA26 (Pure power gamma 2.6)
    9. GAMMA28 (Pure power gamma 2.8)
    10. PRO_PHOTO (ProPhoto RGB (ROMM))

    High dynamic range:
    11. PQ (ITU-R BT.2100 PQ (perceptual quantizer), aka SMPTE ST2048)
    12. HLG (ITU-R BT.2100 HLG (hybrid log-gamma), aka ARIB STD-B67)
    13. V_LOG (Panasonic V-Log (VARICAM))
    14. S_LOG1 (Sony S-Log1)
    15. S_LOG2 (Sony S-Log2)

    Default: 1.

- cplace\
    Chroma sample position in YUV formats:
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

[Back to filters](#filters)
