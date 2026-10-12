package com.rom2apk.player

import android.content.Context
import org.json.JSONObject

/** Contents of `assets/game.json`, written by the rom2apk CLI at build time. */
data class GameConfig(
    val title: String,
    val console: String,
    val core: String,
    val files: List<String>,
    val main: String,
    val shader: String,
    val variables: Map<String, String>,
    /** Overrides for [variables] while a filter that needs native resolution (CRT) is active. */
    val nativeVariables: Map<String, String>,
    val version: String,
) {
    fun variablesFor(filter: VideoFilter): Map<String, String> =
        if (filter.nativeResolution) variables + nativeVariables else variables

    companion object {
        fun load(context: Context): GameConfig {
            val json = context.assets.open("game.json").bufferedReader().use { JSONObject(it.readText()) }
            val files = json.getJSONArray("files")
            return GameConfig(
                title = json.getString("title"),
                console = json.getString("console"),
                core = json.getString("core"),
                files = List(files.length()) { files.getString(it) },
                main = json.getString("main"),
                shader = json.optString("shader", VideoFilter.ORIGINAL.id),
                variables = json.stringMap("variables"),
                nativeVariables = json.stringMap("native_variables"),
                version = json.getString("version"),
            )
        }

        private fun JSONObject.stringMap(name: String): Map<String, String> {
            val obj = optJSONObject(name) ?: return emptyMap()
            return buildMap { obj.keys().forEach { key -> put(key, obj.getString(key)) } }
        }
    }
}
