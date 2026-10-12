package com.rom2apk.player

import android.content.Context
import com.swordfish.libretrodroid.ShaderConfig

enum class VideoFilter(val id: String, val label: String, val nativeResolution: Boolean = false) {
    HD("hd", "HD — xBRZ (contornos lisos e nítidos)"),
    SMOOTH("smooth", "HD suave (CUT3)"),
    CRT("crt", "TV de tubo (CRT)", nativeResolution = true),
    CRT_ARCADE("crt-arcade", "Monitor arcade (CRT curvo)", nativeResolution = true),
    SHARP("sharp", "Pixel perfeito"),
    LCD("lcd", "Tela LCD portátil"),
    ORIGINAL("original", "Original (bilinear)");

    fun shader(context: Context): ShaderConfig = when (this) {
        HD -> ShaderConfig.Custom(
            passes = listOf(
                RetroArchShader.pass(context, "xbrz-freescale-pass0.glsl"),
                RetroArchShader.pass(
                    context,
                    "xbrz-freescale-pass1.glsl",
                    aliases = mapOf(
                        "PassPrev2Texture" to "OrigTexture",
                        "PassPrev2TextureSize" to "OrigInputSize",
                    ),
                ),
            ),
        )
        SMOOTH -> ShaderConfig.CUT3()
        CRT -> ShaderConfig.Custom(listOf(RetroArchShader.pass(context, "crt-easymode.glsl")))
        CRT_ARCADE -> ShaderConfig.Custom(listOf(RetroArchShader.pass(context, "crt-lottes.glsl")))
        SHARP -> ShaderConfig.Sharp
        LCD -> ShaderConfig.LCD
        ORIGINAL -> ShaderConfig.Default
    }

    companion object {
        fun fromId(id: String?): VideoFilter = entries.firstOrNull { it.id == id } ?: ORIGINAL

        fun availableFor(console: String): List<VideoFilter> = when (console) {
            "gb", "gba", "gg" -> entries
            else -> entries.filter { it != LCD }
        }
    }
}

/** Adapts single-file RetroArch GLSL shaders (`#if defined(VERTEX)` style) to LibretroDroid. */
private object RetroArchShader {
    private val cache = HashMap<String, String>()

    fun pass(
        context: Context,
        file: String,
        scale: Float = 1f,
        aliases: Map<String, String> = emptyMap(),
    ): ShaderConfig.Custom.Pass {
        val source = synchronized(cache) {
            cache.getOrPut(file) {
                context.assets.open("shaders/$file").bufferedReader().use { it.readText() }
            }
        }
        val defines = aliases.entries.joinToString("") { (name, value) -> "#define $name $value\n" }
        return ShaderConfig.Custom.Pass(
            vertex = "#define VERTEX\n#define VertexCoord vPosition\n#define TexCoord vCoordinate\n$defines$source",
            fragment = "#define FRAGMENT\n$defines$source",
            scale = scale,
        )
    }
}
