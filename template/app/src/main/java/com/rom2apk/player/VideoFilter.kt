package com.rom2apk.player

import com.swordfish.libretrodroid.ShaderConfig

enum class VideoFilter(val id: String, val label: String, val shader: ShaderConfig) {
    HD("hd", "HD (upscaling suavizado)", ShaderConfig.CUT2()),
    SHARP("sharp", "Pixels nítidos", ShaderConfig.Sharp),
    CRT("crt", "TV de tubo (CRT)", ShaderConfig.CRT),
    LCD("lcd", "Tela LCD", ShaderConfig.LCD),
    ORIGINAL("original", "Original (bilinear)", ShaderConfig.Default);

    companion object {
        fun fromId(id: String?): VideoFilter = entries.firstOrNull { it.id == id } ?: ORIGINAL
    }
}
