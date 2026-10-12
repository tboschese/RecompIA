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
    val version: String,
) {
    companion object {
        fun load(context: Context): GameConfig {
            val json = context.assets.open("game.json").bufferedReader().use { JSONObject(it.readText()) }
            val files = json.getJSONArray("files")
            val variables = json.optJSONObject("variables")
            return GameConfig(
                title = json.getString("title"),
                console = json.getString("console"),
                core = json.getString("core"),
                files = List(files.length()) { files.getString(it) },
                main = json.getString("main"),
                shader = json.optString("shader", VideoFilter.ORIGINAL.id),
                variables = buildMap {
                    variables?.keys()?.forEach { key -> put(key, variables.getString(key)) }
                },
                version = json.getString("version"),
            )
        }
    }
}
