package com.rom2apk.player

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.PointF
import android.graphics.RectF
import android.graphics.Typeface
import android.view.HapticFeedbackConstants
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import com.swordfish.libretrodroid.GLRetroView
import kotlin.math.abs
import kotlin.math.hypot
import kotlin.math.min
import kotlin.math.sign

/**
 * Multi-touch on-screen gamepad. Every touch event recomputes the full input state from all
 * active pointers and only the differences are sent to the emulator, so sliding a finger
 * between buttons works as expected.
 */
@SuppressLint("ViewConstructor")
class VirtualPadView(
    context: Context,
    private val layout: PadLayout,
    private val listener: Listener,
) : View(context) {

    interface Listener {
        fun onKey(keyCode: Int, pressed: Boolean)
        fun onDirection(source: Int, x: Float, y: Float)
        fun onMenu()
    }

    private enum class DirStyle { DPAD, STICK, CBUTTONS }

    private class Directional(val source: Int, val style: DirStyle) {
        var cx = 0f
        var cy = 0f
        var radius = 0f

        fun captures(x: Float, y: Float) = hypot(x - cx, y - cy) <= radius * 1.35f

        fun vectorFor(x: Float, y: Float): PointF {
            val dx = x - cx
            val dy = y - cy
            val dist = hypot(dx, dy)
            if (style == DirStyle.STICK) {
                val scale = if (dist > radius) radius / dist else 1f
                return PointF(dx * scale / radius, dy * scale / radius)
            }
            if (dist < radius * 0.2f) return PointF(0f, 0f)
            // 8-way: an axis is active when the angle is within 67.5° of it.
            val threshold = dist * 0.38f
            return PointF(
                if (abs(dx) > threshold) sign(dx) else 0f,
                if (abs(dy) > threshold) sign(dy) else 0f,
            )
        }
    }

    private class Button(val label: String, val keyCode: Int, val color: Int, val round: Boolean) {
        val bounds = RectF()

        fun hit(x: Float, y: Float): Boolean {
            val padX = bounds.width() * 0.15f
            val padY = bounds.height() * 0.15f
            return x >= bounds.left - padX && x <= bounds.right + padX &&
                y >= bounds.top - padY && y <= bounds.bottom + padY
        }
    }

    var controlsVisible = true
        set(value) {
            field = value
            if (!value) releaseAll()
            invalidate()
        }

    /** Multiplier applied to every control size. */
    var controlScale = 1f
        set(value) {
            field = value
            releaseAll()
            layoutControls(width, height)
            invalidate()
        }

    /** Multiplier applied to every control alpha. */
    var opacity = 1f
        set(value) {
            field = value
            invalidate()
        }

    var haptics = true

    private val leftDir = Directional(
        if (layout.left == LeftControl.STICK) GLRetroView.MOTION_SOURCE_ANALOG_LEFT else GLRetroView.MOTION_SOURCE_DPAD,
        if (layout.left == LeftControl.STICK) DirStyle.STICK else DirStyle.DPAD,
    )
    private val cDir = if (layout.cButtons) Directional(GLRetroView.MOTION_SOURCE_ANALOG_RIGHT, DirStyle.CBUTTONS) else null
    private val directionals = listOfNotNull(leftDir, cDir)

    private val faceButtons = layout.face.map { Button(it.label, it.keyCode, it.color, round = true) }
    private val shoulderButtons = layout.shoulders.map { Button(it.label, it.keyCode, 0, round = false) }
    private val selectButton = if (layout.hasSelect) Button("SELECT", KeyEvent.KEYCODE_BUTTON_SELECT, 0, false) else null
    private val startButton = if (layout.hasStart) Button("START", KeyEvent.KEYCODE_BUTTON_START, 0, false) else null
    private val buttons = faceButtons + shoulderButtons + listOfNotNull(selectButton, startButton)

    /**
     * Pairs of neighbouring face buttons. Touching the gap between them presses both, so
     * combos like Y+B (run + jump) work with a single thumb.
     */
    private val facePairs = faceButtons.indices.flatMap { i ->
        (i + 1 until faceButtons.size).map { j -> faceButtons[i] to faceButtons[j] }
    }.filter { (a, b) ->
        val specA = layout.face[faceButtons.indexOf(a)]
        val specB = layout.face[faceButtons.indexOf(b)]
        hypot(specA.dx - specB.dx, specA.dy - specB.dy) < 1.1f
    }

    private val menuBounds = RectF()

    private val captures = HashMap<Int, Directional?>()
    private var pressedKeys = emptySet<Int>()
    private val dirValues = HashMap<Directional, PointF>()

    private val fill = Paint(Paint.ANTI_ALIAS_FLAG)
    private val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        color = Color.WHITE
    }
    private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE
        textAlign = Paint.Align.CENTER
        typeface = Typeface.DEFAULT_BOLD
    }

    init {
        isHapticFeedbackEnabled = true
    }

    private companion object {
        const val NEUTRAL = 0xFF1A1A1A.toInt()
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) = layoutControls(w, h)

    private fun layoutControls(w: Int, h: Int) {
        if (w == 0 || h == 0) return
        val base = min(w, h).toFloat()
        val unit = base * controlScale
        val margin = base * 0.05f
        val sideMargin = margin + base * 0.04f
        val r = unit * 0.2f
        val clusterY = h - margin - r

        leftDir.cx = sideMargin + r
        leftDir.cy = clusterY
        leftDir.radius = r

        val rightCx = w - sideMargin - r
        val buttonRadius = r * 0.34f
        layout.face.zip(faceButtons).forEach { (spec, button) ->
            val bx = rightCx + spec.dx * r
            val by = clusterY + spec.dy * r
            button.bounds.set(bx - buttonRadius, by - buttonRadius, bx + buttonRadius, by + buttonRadius)
        }

        cDir?.apply {
            cx = rightCx - r * 0.1f
            cy = clusterY - r * 1.9f
            radius = r * 0.55f
        }

        val shoulderWidth = r * 1.4f
        val shoulderHeight = unit * 0.09f
        val shoulderGap = unit * 0.025f
        layout.shoulders.zip(shoulderButtons).forEach { (spec, button) ->
            val top = margin + spec.row * (shoulderHeight + shoulderGap)
            val left = if (spec.side == Side.LEFT) sideMargin else w - sideMargin - shoulderWidth
            button.bounds.set(left, top, left + shoulderWidth, top + shoulderHeight)
        }

        val pillWidth = r * 0.85f
        val pillHeight = unit * 0.075f
        val pillTop = h - margin - pillHeight
        val gap = unit * 0.03f
        selectButton?.bounds?.set(w / 2f - gap / 2 - pillWidth, pillTop, w / 2f - gap / 2, pillTop + pillHeight)
        startButton?.bounds?.set(w / 2f + gap / 2, pillTop, w / 2f + gap / 2 + pillWidth, pillTop + pillHeight)
        if (selectButton == null) startButton?.bounds?.offset(-(pillWidth + gap) / 2, 0f)

        val menuRadius = base * 0.045f
        menuBounds.set(w / 2f - menuRadius, margin, w / 2f + menuRadius, margin + 2 * menuRadius)

        stroke.strokeWidth = base * 0.006f
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(event: MotionEvent): Boolean {
        var liftedIndex = -1
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val index = event.actionIndex
                val x = event.getX(index)
                val y = event.getY(index)
                if (menuBounds.contains(x, y)) {
                    if (haptics) performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP)
                    listener.onMenu()
                    return true
                }
                captures[event.getPointerId(index)] =
                    if (controlsVisible) directionals.firstOrNull { it.captures(x, y) } else null
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
                liftedIndex = event.actionIndex
                captures.remove(event.getPointerId(liftedIndex))
            }
            MotionEvent.ACTION_CANCEL -> {
                releaseAll()
                return true
            }
        }
        if (!controlsVisible) return true

        val newKeys = HashSet<Int>()
        val newDirs = HashMap<Directional, PointF>()
        for (i in 0 until event.pointerCount) {
            if (i == liftedIndex) continue
            val id = event.getPointerId(i)
            if (!captures.containsKey(id)) continue
            val x = event.getX(i)
            val y = event.getY(i)
            val captured = captures[id]
            if (captured != null) {
                newDirs[captured] = captured.vectorFor(x, y)
            } else {
                newKeys += buttonsAt(x, y)
            }
        }
        applyState(newKeys, newDirs)
        return true
    }

    private fun buttonsAt(x: Float, y: Float): List<Int> {
        for ((a, b) in facePairs) {
            val midX = (a.bounds.centerX() + b.bounds.centerX()) / 2
            val midY = (a.bounds.centerY() + b.bounds.centerY()) / 2
            val buttonRadius = a.bounds.width() / 2
            val halfDistance = hypot(a.bounds.centerX() - b.bounds.centerX(), a.bounds.centerY() - b.bounds.centerY()) / 2
            // The zone must leave at least the inner 60% of each button for single presses.
            val zone = min(buttonRadius * 0.5f, halfDistance - buttonRadius * 0.6f)
            if (hypot(x - midX, y - midY) < zone) return listOf(a.keyCode, b.keyCode)
        }
        faceButtons
            .map { it to hypot(x - it.bounds.centerX(), y - it.bounds.centerY()) }
            .filter { (button, dist) -> dist < button.bounds.width() * 0.62f }
            .minByOrNull { it.second }
            ?.let { return listOf(it.first.keyCode) }
        return (buttons - faceButtons.toSet()).filter { it.hit(x, y) }.take(1).map { it.keyCode }
    }

    private fun applyState(newKeys: Set<Int>, newDirs: Map<Directional, PointF>) {
        var changed = false
        (pressedKeys - newKeys).forEach { listener.onKey(it, false); changed = true }
        val pressed = newKeys - pressedKeys
        pressed.forEach { listener.onKey(it, true); changed = true }
        if (pressed.isNotEmpty() && haptics) performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP)
        pressedKeys = newKeys

        for (dir in directionals) {
            val value = newDirs[dir] ?: PointF(0f, 0f)
            val old = dirValues[dir] ?: PointF(0f, 0f)
            if (value != old) {
                if (haptics && dir.style != DirStyle.STICK && (value.x != 0f || value.y != 0f)) {
                    performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK)
                }
                listener.onDirection(dir.source, value.x, value.y)
                dirValues[dir] = value
                changed = true
            }
        }
        if (changed) invalidate()
    }

    fun releaseAll() {
        captures.clear()
        applyState(emptySet(), emptyMap())
    }

    override fun onDraw(canvas: Canvas) {
        drawMenuButton(canvas)
        if (!controlsVisible) return

        directionals.forEach { drawDirectional(canvas, it) }
        buttons.forEach { drawButton(canvas, it, it.keyCode in pressedKeys) }
    }

    private fun alpha(value: Int) = (value * opacity).toInt().coerceIn(0, 255)

    // Neutral controls use a dark fill with a light outline so they stay visible on both
    // dark and bright game screens.
    private fun idleFill(color: Int = Color.WHITE) = fill.apply {
        this.color = if (color == Color.WHITE) NEUTRAL else color
        alpha = alpha(if (color == Color.WHITE) 120 else 150)
    }

    private fun pressedFill(color: Int = Color.WHITE) = fill.apply {
        this.color = color
        alpha = alpha(200).coerceAtLeast(140)
    }

    private fun drawMenuButton(canvas: Canvas) {
        canvas.drawOval(menuBounds, idleFill())
        stroke.alpha = alpha(120)
        canvas.drawOval(menuBounds, stroke)
        val cx = menuBounds.centerX()
        val cy = menuBounds.centerY()
        val half = menuBounds.width() * 0.25f
        stroke.alpha = alpha(200)
        for (offset in listOf(-0.5f, 0f, 0.5f)) {
            canvas.drawLine(cx - half, cy + offset * half, cx + half, cy + offset * half, stroke)
        }
    }

    private fun drawButton(canvas: Canvas, button: Button, pressed: Boolean) {
        val baseColor = if (button.color != 0) button.color else Color.WHITE
        val paint = if (pressed) pressedFill(baseColor) else idleFill(baseColor)
        val b = button.bounds
        if (button.round) {
            canvas.drawOval(b, paint)
        } else {
            canvas.drawRoundRect(b, b.height() / 2, b.height() / 2, paint)
        }
        stroke.alpha = if (pressed) 230 else alpha(120)
        if (button.round) canvas.drawOval(b, stroke) else canvas.drawRoundRect(b, b.height() / 2, b.height() / 2, stroke)

        text.textSize = if (button.round) b.height() * 0.45f else b.height() * 0.42f
        text.alpha = if (pressed) 255 else alpha(200)
        canvas.drawText(button.label, b.centerX(), b.centerY() - (text.descent() + text.ascent()) / 2, text)
    }

    private fun drawDirectional(canvas: Canvas, dir: Directional) {
        val value = dirValues[dir] ?: PointF(0f, 0f)
        when (dir.style) {
            DirStyle.STICK -> {
                canvas.drawCircle(dir.cx, dir.cy, dir.radius, idleFill())
                stroke.alpha = alpha(120)
                canvas.drawCircle(dir.cx, dir.cy, dir.radius, stroke)
                val knobPressed = value.x != 0f || value.y != 0f
                canvas.drawCircle(
                    dir.cx + value.x * dir.radius * 0.6f,
                    dir.cy + value.y * dir.radius * 0.6f,
                    dir.radius * 0.42f,
                    if (knobPressed) pressedFill() else idleFill().apply { alpha = alpha(110) },
                )
            }
            DirStyle.DPAD -> {
                val arm = dir.radius * 0.95f
                val thickness = dir.radius * 0.34f
                val horizontal = RectF(dir.cx - arm, dir.cy - thickness, dir.cx + arm, dir.cy + thickness)
                val vertical = RectF(dir.cx - thickness, dir.cy - arm, dir.cx + thickness, dir.cy + arm)
                val corner = thickness * 0.4f
                val cross = Path().apply {
                    addRoundRect(horizontal, corner, corner, Path.Direction.CW)
                    op(Path().apply { addRoundRect(vertical, corner, corner, Path.Direction.CW) }, Path.Op.UNION)
                }
                canvas.drawPath(cross, idleFill())
                stroke.alpha = alpha(120)
                canvas.drawPath(cross, stroke)
                drawArrows(canvas, dir, arm, thickness)
                val pressedPaint = pressedFill()
                if (value.x != 0f) {
                    val left = if (value.x < 0) dir.cx - arm else dir.cx + thickness
                    canvas.drawRoundRect(RectF(left, dir.cy - thickness, left + arm - thickness, dir.cy + thickness), corner, corner, pressedPaint)
                }
                if (value.y != 0f) {
                    val top = if (value.y < 0) dir.cy - arm else dir.cy + thickness
                    canvas.drawRoundRect(RectF(dir.cx - thickness, top, dir.cx + thickness, top + arm - thickness), corner, corner, pressedPaint)
                }
            }
            DirStyle.CBUTTONS -> {
                val small = dir.radius * 0.36f
                val offset = dir.radius * 0.64f
                val yellow = 0xFFFDD835.toInt()
                listOf(
                    Triple(0f, -1f, "▲"), Triple(0f, 1f, "▼"), Triple(-1f, 0f, "◀"), Triple(1f, 0f, "▶"),
                ).forEach { (dx, dy, arrow) ->
                    val active = (dx != 0f && value.x == dx) || (dy != 0f && value.y == dy)
                    val bx = dir.cx + dx * offset
                    val by = dir.cy + dy * offset
                    canvas.drawCircle(bx, by, small, if (active) pressedFill(yellow) else idleFill(yellow))
                    text.textSize = small * 0.9f
                    text.alpha = alpha(220)
                    canvas.drawText(arrow, bx, by - (text.descent() + text.ascent()) / 2, text)
                }
                text.textSize = small * 0.8f
                canvas.drawText("C", dir.cx, dir.cy - (text.descent() + text.ascent()) / 2, text)
            }
        }
    }

    private fun drawArrows(canvas: Canvas, dir: Directional, arm: Float, thickness: Float) {
        val tip = arm * 0.82f
        val size = thickness * 0.5f
        fill.color = Color.WHITE
        fill.alpha = alpha(150)
        for ((dx, dy) in listOf(0f to -1f, 0f to 1f, -1f to 0f, 1f to 0f)) {
            val tipX = dir.cx + dx * tip
            val tipY = dir.cy + dy * tip
            val baseX = tipX - dx * size
            val baseY = tipY - dy * size
            val arrow = Path().apply {
                moveTo(tipX, tipY)
                lineTo(baseX + dy * size, baseY + dx * size)
                lineTo(baseX - dy * size, baseY - dx * size)
                close()
            }
            canvas.drawPath(arrow, fill)
        }
    }
}
