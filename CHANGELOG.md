##### 1.2.0:
    Resample/Shader: added trc ST428.
    Tonemap: added parameters percentile, metadata, visualize_lut, show_clipping.
    Tonemap: added tone_mapping_function st2094_40, st2094_10.
    Shader/Tonemap: improved speed. (based on https://github.com/Lypheo/vs-placebo/commit/09075cf2a3768b7c87903bb23640916b0b3b68cc)
    Tonemap: added support for libdovi 3. (based on https://github.com/Lypheo/vs-placebo/commit/f65161b7dd167b60e7af4670a692c6df3c40de6e)
    Removed libp2p dependency.
    Tonemap: fixed wrong levels when output is SDR.
    Tonemap: remove HDR frame props when output is SDR.
    Tonemap: added support for libplacebo v5.264.0. (based on https://github.com/Lypheo/vs-placebo/commit/4a42255c880572d75c8b50b69b784a67fd93e241)
    Shader: removed shader_param limit.

##### 1.1.5:
    libplacebo_Tonemap: fixed `dst_min`.

##### 1.1.4:
    libplacebo_Tonemap: fixed `src_csp` < 3.

##### 1.1.3:
    libplacebo_Resample: added `src_width` and `src_height` parameters.
    libplacebo_Deband: added `grain_neutral` parameter.

##### 1.1.2:
    libplacebo_Tonemap: properly handle video w/o `DolbyVisionRPU`.

##### 1.1.1:
    libplacebo_Resample: fixed chroma location for YUV444.

##### 1.1.0:
    libplacebo_Deband: replaced parameter `grain` with `grainY` and `grainC`.
    Fixed undefined behavior when upstream throw runtime error.
    libplacebo_Tonemap: throw error when src_csp=3 and there is no frame property `DolbyVisionRPU`.
    libplacebo_Tonemap: fixed black point for any Dolby Vision to PQ conversion.

##### 1.0.1:
    libplacebo_Shader: added shader_param.

##### 1.0.0:
    Initial release. (port of the vs-placebo 1.4.2)
