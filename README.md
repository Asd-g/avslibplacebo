## Description

An AviSynth+ plugin interface to libplacebo - a reusable library for GPU-accelerated image/video processing primitives and shaders.

This is [a port of the VapourSynth plugin vs-placebo](https://github.com/Lypheo/vs-placebo).

### Requirements:

- AviSynth+ 3.6 or later

- Microsoft VisualC++ Redistributable Package 2022 (can be downloaded from [here](https://github.com/abbodi1406/vcredist/releases))

### Usage:

```
libplDeband(clip input, int "iterations", float "threshold", float "radius", float "grain", int "dither", int "lut_size", bool "temporal", int[] "planes", int "device", bool "list_device")
```

### Parameters:

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
