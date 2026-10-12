package com.rom2apk.player

import android.view.KeyEvent.KEYCODE_BUTTON_A
import android.view.KeyEvent.KEYCODE_BUTTON_B
import android.view.KeyEvent.KEYCODE_BUTTON_L1
import android.view.KeyEvent.KEYCODE_BUTTON_L2
import android.view.KeyEvent.KEYCODE_BUTTON_R1
import android.view.KeyEvent.KEYCODE_BUTTON_R2
import android.view.KeyEvent.KEYCODE_BUTTON_X
import android.view.KeyEvent.KEYCODE_BUTTON_Y

enum class LeftControl { DPAD, STICK }

enum class Side { LEFT, RIGHT }

/** Offsets are relative to the right cluster center, in units of the cluster radius. */
data class FaceButton(val label: String, val keyCode: Int, val dx: Float, val dy: Float, val color: Int = 0)

data class ShoulderButton(val label: String, val keyCode: Int, val side: Side, val row: Int)

/**
 * Key codes follow LibretroDroid's RetroPad convention (KEYCODE_BUTTON_A = RetroPad A,
 * the east face button on a SNES pad), not the Android/Xbox physical layout.
 */
data class PadLayout(
    val left: LeftControl,
    val face: List<FaceButton>,
    val shoulders: List<ShoulderButton> = emptyList(),
    val hasSelect: Boolean = true,
    val hasStart: Boolean = true,
    val cButtons: Boolean = false,
)

object PadLayouts {
    private const val RED = 0xFFE53935.toInt()
    private const val GREEN = 0xFF43A047.toInt()
    private const val BLUE = 0xFF1E88E5.toInt()
    private const val YELLOW = 0xFFFDD835.toInt()
    private const val PINK = 0xFFEC407A.toInt()

    private val twoButtons = listOf(
        FaceButton("B", KEYCODE_BUTTON_B, -0.48f, 0.22f),
        FaceButton("A", KEYCODE_BUTTON_A, 0.48f, -0.22f),
    )

    private val nes = PadLayout(LeftControl.DPAD, twoButtons)

    private val gb = PadLayout(LeftControl.DPAD, twoButtons)

    private val gba = PadLayout(
        LeftControl.DPAD,
        twoButtons,
        shoulders = listOf(
            ShoulderButton("L", KEYCODE_BUTTON_L1, Side.LEFT, 0),
            ShoulderButton("R", KEYCODE_BUTTON_R1, Side.RIGHT, 0),
        ),
    )

    private val snes = PadLayout(
        LeftControl.DPAD,
        listOf(
            FaceButton("X", KEYCODE_BUTTON_X, 0f, -0.6f, BLUE),
            FaceButton("Y", KEYCODE_BUTTON_Y, -0.6f, 0f, GREEN),
            FaceButton("A", KEYCODE_BUTTON_A, 0.6f, 0f, RED),
            FaceButton("B", KEYCODE_BUTTON_B, 0f, 0.6f, YELLOW),
        ),
        shoulders = listOf(
            ShoulderButton("L", KEYCODE_BUTTON_L1, Side.LEFT, 0),
            ShoulderButton("R", KEYCODE_BUTTON_R1, Side.RIGHT, 0),
        ),
    )

    private val megaDrive = PadLayout(
        LeftControl.DPAD,
        listOf(
            FaceButton("A", KEYCODE_BUTTON_Y, -0.62f, 0.3f),
            FaceButton("B", KEYCODE_BUTTON_B, 0f, 0.05f),
            FaceButton("C", KEYCODE_BUTTON_A, 0.62f, -0.2f),
        ),
        hasSelect = false,
    )

    private val masterSystem = PadLayout(
        LeftControl.DPAD,
        listOf(
            FaceButton("1", KEYCODE_BUTTON_B, -0.48f, 0.22f),
            FaceButton("2", KEYCODE_BUTTON_A, 0.48f, -0.22f),
        ),
        hasSelect = false,
    )

    private val psx = PadLayout(
        LeftControl.DPAD,
        listOf(
            FaceButton("△", KEYCODE_BUTTON_X, 0f, -0.6f, GREEN),
            FaceButton("□", KEYCODE_BUTTON_Y, -0.6f, 0f, PINK),
            FaceButton("○", KEYCODE_BUTTON_A, 0.6f, 0f, RED),
            FaceButton("✕", KEYCODE_BUTTON_B, 0f, 0.6f, BLUE),
        ),
        shoulders = listOf(
            ShoulderButton("L1", KEYCODE_BUTTON_L1, Side.LEFT, 0),
            ShoulderButton("L2", KEYCODE_BUTTON_L2, Side.LEFT, 1),
            ShoulderButton("R1", KEYCODE_BUTTON_R1, Side.RIGHT, 0),
            ShoulderButton("R2", KEYCODE_BUTTON_R2, Side.RIGHT, 1),
        ),
    )

    private val n64 = PadLayout(
        LeftControl.STICK,
        listOf(
            FaceButton("A", KEYCODE_BUTTON_B, 0.1f, 0.45f, BLUE),
            FaceButton("B", KEYCODE_BUTTON_Y, -0.55f, 0.05f, GREEN),
        ),
        shoulders = listOf(
            ShoulderButton("L", KEYCODE_BUTTON_L1, Side.LEFT, 0),
            ShoulderButton("Z", KEYCODE_BUTTON_L2, Side.LEFT, 1),
            ShoulderButton("R", KEYCODE_BUTTON_R1, Side.RIGHT, 0),
        ),
        hasSelect = false,
        cButtons = true,
    )

    fun forConsole(console: String): PadLayout = when (console) {
        "nes" -> nes
        "snes" -> snes
        "gb", "gbc" -> gb
        "gba" -> gba
        "md" -> megaDrive
        "sms", "gg" -> masterSystem
        "psx" -> psx
        "n64" -> n64
        else -> snes
    }
}
