package com.rom2apk.player

import android.content.Context
import android.hardware.input.InputManager
import android.os.Bundle
import android.os.SystemClock
import android.view.Gravity
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.widget.FrameLayout
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.lifecycle.lifecycleScope
import com.swordfish.libretrodroid.GLRetroView
import com.swordfish.libretrodroid.GLRetroViewData
import com.swordfish.libretrodroid.LibretroDroid
import com.swordfish.libretrodroid.Variable
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.text.DateFormat
import java.util.Date
import kotlin.math.abs
import kotlin.math.sign

class GameActivity : AppCompatActivity(), VirtualPadView.Listener, InputManager.InputDeviceListener {

    private enum class TouchMode(val label: String) { AUTO("Automático"), ALWAYS("Sempre"), NEVER("Nunca") }

    private enum class PadSize(val label: String, val scale: Float) {
        SMALL("Pequeno", 0.85f), MEDIUM("Médio", 1f), LARGE("Grande", 1.15f), HUGE("Enorme", 1.3f)
    }

    private enum class PadOpacity(val label: String, val factor: Float) {
        LOW("Discreta", 0.55f), MEDIUM("Normal", 1f), HIGH("Forte", 1.5f)
    }

    private enum class ButtonMapping(val label: String) {
        POSITION("Pela posição (Xbox, PlayStation, genéricos)"),
        LABEL("Pelo nome (controles com layout Nintendo)"),
    }

    private lateinit var config: GameConfig
    private lateinit var container: FrameLayout
    private var retroView: GLRetroView? = null
    private var pad: VirtualPadView? = null
    private var gameLoaded = false
    private var menuOpen = false

    private val prefs by lazy { getSharedPreferences("player", Context.MODE_PRIVATE) }
    private val savesDir by lazy { File(filesDir, "saves").apply { mkdirs() } }
    private val sramFile get() = File(savesDir, "game.srm")
    private val resumeFile get() = File(savesDir, "resume-${config.version}.state")
    private val inputManager by lazy { getSystemService(InputManager::class.java) }
    private val padLayout by lazy { PadLayouts.forConsole(config.console) }

    private var filter: VideoFilter
        get() = VideoFilter.fromId(prefs.getString("filter", null) ?: config.shader)
        set(value) = prefs.edit().putString("filter", value.id).apply()

    private var touchMode: TouchMode
        get() = enumPref("touch_mode", TouchMode.AUTO)
        set(value) = prefs.edit().putString("touch_mode", value.name).apply()

    private var padSize: PadSize
        get() = enumPref("pad_size", PadSize.MEDIUM)
        set(value) = prefs.edit().putString("pad_size", value.name).apply()

    private var padOpacity: PadOpacity
        get() = enumPref("pad_opacity", PadOpacity.MEDIUM)
        set(value) = prefs.edit().putString("pad_opacity", value.name).apply()

    private var buttonMapping: ButtonMapping
        get() = enumPref("button_mapping", ButtonMapping.POSITION)
        set(value) = prefs.edit().putString("button_mapping", value.name).apply()

    private var haptics: Boolean
        get() = prefs.getBoolean("haptics", true)
        set(value) = prefs.edit().putBoolean("haptics", value).apply()

    private var autoResume: Boolean
        get() = prefs.getBoolean("auto_resume", true)
        set(value) = prefs.edit().putBoolean("auto_resume", value).apply()

    private var fastForward = false

    private inline fun <reified T : Enum<T>> enumPref(key: String, default: T): T =
        runCatching { enumValueOf<T>(prefs.getString(key, null) ?: default.name) }.getOrDefault(default)

    private val triggerState = HashMap<Int, Pair<Boolean, Boolean>>()
    private val pressTimes = HashMap<Int, Long>()
    private val pendingReleases = HashMap<Int, Runnable>()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enterImmersiveMode()

        config = GameConfig.load(this)
        title = config.title
        container = FrameLayout(this)
        setContentView(container)

        val loading = TextView(this).apply {
            text = "Preparando ${config.title}…"
            textSize = 18f
            setTextColor(0xFFFFFFFF.toInt())
            gravity = Gravity.CENTER
        }
        container.addView(loading, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))

        lifecycleScope.launch {
            val gameFile = try {
                withContext(Dispatchers.IO) { extractGameFiles() }
            } catch (e: Exception) {
                showFatalError("Não foi possível preparar o jogo: ${e.message}")
                return@launch
            }
            container.removeView(loading)
            startEmulator(gameFile)
        }
    }

    private fun extractGameFiles(): File {
        val gameDir = File(filesDir, "game")
        val marker = File(gameDir, ".version")
        if (!marker.exists() || marker.readText() != config.version) {
            gameDir.deleteRecursively()
            gameDir.mkdirs()
            config.files.forEach { name -> copyAsset("game/$name", File(gameDir, name)) }
            marker.writeText(config.version)
        }

        val systemDir = File(filesDir, "system").apply { mkdirs() }
        assets.list("system")?.forEach { name ->
            val target = File(systemDir, name)
            if (!target.exists()) copyAsset("system/$name", target)
        }
        return File(gameDir, config.main)
    }

    private fun copyAsset(assetPath: String, target: File) {
        val partial = File(target.parentFile, target.name + ".part")
        assets.open(assetPath).use { input ->
            partial.outputStream().use { output -> input.copyTo(output, 1 shl 20) }
        }
        partial.renameTo(target)
    }

    private fun startEmulator(gameFile: File) {
        val data = GLRetroViewData(this).apply {
            coreFilePath = config.core
            gameFilePath = gameFile.absolutePath
            systemDirectory = File(filesDir, "system").absolutePath
            savesDirectory = savesDir.absolutePath
            variables = config.variablesFor(filter).map { Variable(it.key, it.value) }.toTypedArray()
            saveRAMState = sramFile.takeIf { it.exists() }?.readBytes()
            shader = filter.shader(this@GameActivity)
            rumbleEventsEnabled = false
            preferLowLatencyAudio = true
        }

        val view = GLRetroView(this, data).apply {
            isFocusable = false
            isFocusableInTouchMode = false
        }
        retroView = view
        container.addView(view, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
        lifecycle.addObserver(view)

        val padView = VirtualPadView(this, padLayout, this)
        pad = padView
        applyPadSettings()
        container.addView(padView, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
        updatePadVisibility()

        lifecycleScope.launch {
            view.getGLRetroEvents().collect { event ->
                if (event is GLRetroView.GLRetroEvents.SurfaceCreated) gameLoaded = true
            }
        }
        lifecycleScope.launch {
            view.getGLRetroEvents().first { it is GLRetroView.GLRetroEvents.FrameRendered }
            resumeWhereLeftOff(view)
        }
        lifecycleScope.launch {
            view.getGLRetroErrors().collect { showFatalError(describeError(it)) }
        }
    }

    private suspend fun resumeWhereLeftOff(view: GLRetroView) {
        savesDir.listFiles { file -> file.name.startsWith("resume-") && file != resumeFile }?.forEach { it.delete() }
        val file = resumeFile
        if (!autoResume || !file.exists()) return
        val ok = withContext(Dispatchers.IO) {
            runCatching { view.unserializeState(file.readBytes(), true) }.getOrDefault(false)
        }
        if (ok) toast("Continuando de onde você parou")
    }

    private fun describeError(code: Int): String = when (code) {
        GLRetroView.ERROR_LOAD_LIBRARY -> "Não foi possível carregar o emulador (${config.core})."
        GLRetroView.ERROR_LOAD_GAME -> "O emulador não conseguiu abrir a ROM. Verifique se o arquivo está íntegro."
        GLRetroView.ERROR_GL_NOT_COMPATIBLE -> "Este aparelho não suporta os gráficos exigidos por este console."
        else -> "Erro inesperado no emulador (código $code)."
    }

    private fun showFatalError(message: String) {
        AlertDialog.Builder(this)
            .setTitle(config.title)
            .setMessage(message)
            .setCancelable(false)
            .setPositiveButton("Fechar") { _, _ -> finish() }
            .show()
    }

    override fun onResume() {
        super.onResume()
        enterImmersiveMode()
        inputManager.registerInputDeviceListener(this, null)
        updatePadVisibility()
        // Lifecycle observers resume the emulator after this method returns.
        if (menuOpen) container.post { if (menuOpen) pauseEmulation() }
    }

    override fun onPause() {
        inputManager.unregisterInputDeviceListener(this)
        pad?.releaseAll()
        super.onPause()
        saveSram()
    }

    private fun saveSram() {
        val view = retroView ?: return
        if (!gameLoaded) return
        runCatching { sramFile.writeBytes(view.serializeSRAM(false)) }
        if (autoResume) runCatching { resumeFile.writeBytes(view.serializeState(false)) }
    }

    private fun enterImmersiveMode() {
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowInsetsControllerCompat(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
    }

    // region Virtual pad

    override fun onKey(keyCode: Int, pressed: Boolean) {
        val view = retroView ?: return
        if (pressed) {
            pendingReleases.remove(keyCode)?.let { container.removeCallbacks(it) }
            pressTimes[keyCode] = SystemClock.uptimeMillis()
            view.sendKeyEvent(KeyEvent.ACTION_DOWN, keyCode)
            return
        }
        // The core samples input once per frame, so a press shorter than a frame would be lost.
        val held = SystemClock.uptimeMillis() - (pressTimes.remove(keyCode) ?: 0L)
        val release = Runnable {
            pendingReleases.remove(keyCode)
            view.sendKeyEvent(KeyEvent.ACTION_UP, keyCode)
        }
        if (held >= MIN_PRESS_MS) {
            release.run()
        } else {
            pendingReleases[keyCode] = release
            container.postDelayed(release, MIN_PRESS_MS - held)
        }
    }

    override fun onDirection(source: Int, x: Float, y: Float) {
        val view = retroView ?: return
        val id = -(source + 1)
        pendingReleases.remove(id)?.let { container.removeCallbacks(it) }
        if (x != 0f || y != 0f) {
            pressTimes.getOrPut(id) { SystemClock.uptimeMillis() }
            view.sendMotionEvent(source, x, y)
            return
        }
        val held = SystemClock.uptimeMillis() - (pressTimes.remove(id) ?: 0L)
        val release = Runnable {
            pendingReleases.remove(id)
            view.sendMotionEvent(source, 0f, 0f)
        }
        if (held >= MIN_PRESS_MS) {
            release.run()
        } else {
            pendingReleases[id] = release
            container.postDelayed(release, MIN_PRESS_MS - held)
        }
    }

    override fun onMenu() = showMenu()

    private fun updatePadVisibility() {
        pad?.controlsVisible = when (touchMode) {
            TouchMode.ALWAYS -> true
            TouchMode.NEVER -> false
            TouchMode.AUTO -> !hasPhysicalGamepad()
        }
    }

    // endregion

    // region Physical gamepads (Bluetooth / USB)

    private fun hasPhysicalGamepad(): Boolean = InputDevice.getDeviceIds().any { id ->
        val device = InputDevice.getDevice(id) ?: return@any false
        !device.isVirtual && (device.supportsSource(InputDevice.SOURCE_GAMEPAD) || device.supportsSource(InputDevice.SOURCE_JOYSTICK))
    }

    override fun onInputDeviceAdded(deviceId: Int) = updatePadVisibility()
    override fun onInputDeviceRemoved(deviceId: Int) = updatePadVisibility()
    override fun onInputDeviceChanged(deviceId: Int) = updatePadVisibility()

    private fun portOf(device: InputDevice?): Int = ((device?.controllerNumber ?: 1) - 1).coerceAtLeast(0)

    private fun isGamepadEvent(event: KeyEvent): Boolean =
        event.isFromSource(InputDevice.SOURCE_GAMEPAD) ||
            event.isFromSource(InputDevice.SOURCE_JOYSTICK) ||
            event.isFromSource(InputDevice.SOURCE_DPAD)

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val view = retroView ?: return super.dispatchKeyEvent(event)
        when (event.keyCode) {
            KeyEvent.KEYCODE_BACK, KeyEvent.KEYCODE_BUTTON_MODE -> {
                if (event.action == KeyEvent.ACTION_UP) showMenu()
                return true
            }
        }
        if (event.keyCode in GAMEPAD_KEYS && isGamepadEvent(event)) {
            if (event.repeatCount == 0) {
                val keyCode = if (buttonMapping == ButtonMapping.POSITION) toRetroPad(event.keyCode) else event.keyCode
                view.sendKeyEvent(event.action, keyCode, portOf(event.device))
            }
            return true
        }
        return super.dispatchKeyEvent(event)
    }

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        val view = retroView
        if (view == null || !event.isFromSource(InputDevice.SOURCE_JOYSTICK) || event.action != MotionEvent.ACTION_MOVE) {
            return super.dispatchGenericMotionEvent(event)
        }
        val port = portOf(event.device)
        val stickX = event.getAxisValue(MotionEvent.AXIS_X)
        val stickY = event.getAxisValue(MotionEvent.AXIS_Y)
        var hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X)
        var hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y)

        if (padLayout.left == LeftControl.DPAD) {
            // Consoles without analog sticks: let the left stick drive the d-pad too.
            if (hatX == 0f && abs(stickX) > 0.5f) hatX = sign(stickX)
            if (hatY == 0f && abs(stickY) > 0.5f) hatY = sign(stickY)
        }

        view.sendMotionEvent(GLRetroView.MOTION_SOURCE_DPAD, hatX, hatY, port)
        view.sendMotionEvent(GLRetroView.MOTION_SOURCE_ANALOG_LEFT, stickX, stickY, port)
        view.sendMotionEvent(
            GLRetroView.MOTION_SOURCE_ANALOG_RIGHT,
            event.getAxisValue(MotionEvent.AXIS_Z),
            event.getAxisValue(MotionEvent.AXIS_RZ),
            port,
        )

        val l2 = maxOf(event.getAxisValue(MotionEvent.AXIS_LTRIGGER), event.getAxisValue(MotionEvent.AXIS_BRAKE)) > 0.5f
        val r2 = maxOf(event.getAxisValue(MotionEvent.AXIS_RTRIGGER), event.getAxisValue(MotionEvent.AXIS_GAS)) > 0.5f
        val (oldL2, oldR2) = triggerState[port] ?: (false to false)
        if (l2 != oldL2) view.sendKeyEvent(if (l2) KeyEvent.ACTION_DOWN else KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BUTTON_L2, port)
        if (r2 != oldR2) view.sendKeyEvent(if (r2) KeyEvent.ACTION_DOWN else KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BUTTON_R2, port)
        triggerState[port] = l2 to r2
        return true
    }

    // endregion

    // region In-game menu

    private fun pauseEmulation() {
        val view = retroView ?: return
        LibretroDroid.pause()
        view.onPause()
    }

    private fun resumeEmulation() {
        val view = retroView ?: return
        view.onResume()
        LibretroDroid.resume()
    }

    private fun showMenu() {
        if (menuOpen || retroView == null) return
        menuOpen = true
        pad?.releaseAll()
        pauseEmulation()

        val options = listOf(
            "Continuar" to {},
            "Salvar estado…" to { showSlotPicker(save = true) },
            "Carregar estado…" to { showSlotPicker(save = false) },
            "Filtro de imagem: ${filter.label}" to { showFilterPicker() },
            "Controles…" to { showControlsMenu() },
            (if (fastForward) "Avançar rápido: ligado" else "Avançar rápido: desligado") to { toggleFastForward() },
            "Reiniciar jogo" to { retroView?.reset(false); Unit },
            "Sair" to { saveSram(); finish() },
        )
        AlertDialog.Builder(this)
            .setTitle(config.title)
            .setItems(options.map { it.first }.toTypedArray()) { _, which -> options[which].second() }
            .setOnDismissListener { closeMenu() }
            .show()
    }

    private fun closeMenu() {
        // Sub-dialogs reopen the menu flow; only resume when nothing else is showing.
        container.post {
            if (menuOpen && openSubDialogs == 0) {
                menuOpen = false
                enterImmersiveMode()
                if (!isFinishing) resumeEmulation()
            }
        }
    }

    private var openSubDialogs = 0

    private fun showSubDialog(builder: AlertDialog.Builder) {
        openSubDialogs++
        builder.setOnDismissListener {
            openSubDialogs--
            closeMenu()
        }.show()
    }

    private fun stateFile(slot: Int) = File(savesDir, "slot$slot.state")

    private fun showSlotPicker(save: Boolean) {
        val labels = (1..SLOTS).map { slot ->
            val file = stateFile(slot)
            val date = if (file.exists()) DateFormat.getDateTimeInstance().format(Date(file.lastModified())) else "vazio"
            "Slot $slot — $date"
        }
        showSubDialog(
            AlertDialog.Builder(this)
                .setTitle(if (save) "Salvar estado" else "Carregar estado")
                .setItems(labels.toTypedArray()) { _, which ->
                    val slot = which + 1
                    if (save) saveState(slot) else loadState(slot)
                },
        )
    }

    private fun saveState(slot: Int) {
        val view = retroView ?: return
        val ok = runCatching { stateFile(slot).writeBytes(view.serializeState(false)) }.isSuccess
        toast(if (ok) "Estado salvo no slot $slot" else "Não foi possível salvar o estado")
    }

    private fun loadState(slot: Int) {
        val view = retroView ?: return
        val file = stateFile(slot)
        if (!file.exists()) {
            toast("O slot $slot está vazio")
            return
        }
        val ok = runCatching { view.unserializeState(file.readBytes(), false) }.getOrDefault(false)
        toast(if (ok) "Estado carregado" else "Não foi possível carregar o estado")
    }

    private fun showFilterPicker() {
        val filters = VideoFilter.availableFor(config.console)
        showSubDialog(
            AlertDialog.Builder(this)
                .setTitle("Filtro de imagem")
                .setSingleChoiceItems(filters.map { it.label }.toTypedArray(), filters.indexOf(filter)) { dialog, which ->
                    val previous = filter
                    filter = filters[which]
                    retroView?.apply {
                        shader = filter.shader(this@GameActivity)
                        if (previous.nativeResolution != filter.nativeResolution) {
                            val changed = config.variablesFor(filter).filterKeys { it in config.nativeVariables }
                            updateVariables(*changed.map { Variable(it.key, it.value) }.toTypedArray())
                        }
                    }
                    dialog.dismiss()
                },
        )
    }

    private fun toggleFastForward() {
        fastForward = !fastForward
        retroView?.frameSpeed = if (fastForward) FAST_FORWARD_SPEED else 1
    }

    private fun applyPadSettings() {
        pad?.apply {
            controlScale = padSize.scale
            opacity = padOpacity.factor
            haptics = this@GameActivity.haptics
        }
    }

    private fun showControlsMenu() {
        val options = listOf(
            "Mostrar na tela: ${touchMode.label}" to {
                showChoice("Controles na tela", TouchMode.entries, touchMode, { it.label }) {
                    touchMode = it
                    updatePadVisibility()
                }
            },
            "Tamanho: ${padSize.label}" to {
                showChoice("Tamanho dos controles", PadSize.entries, padSize, { it.label }) {
                    padSize = it
                    applyPadSettings()
                }
            },
            "Visibilidade: ${padOpacity.label}" to {
                showChoice("Visibilidade dos controles", PadOpacity.entries, padOpacity, { it.label }) {
                    padOpacity = it
                    applyPadSettings()
                }
            },
            (if (haptics) "Vibração: ligada" else "Vibração: desligada") to {
                haptics = !haptics
                applyPadSettings()
            },
            "Controle físico: ${if (buttonMapping == ButtonMapping.POSITION) "pela posição" else "pelo nome"}" to {
                showChoice("Mapear botões do controle físico", ButtonMapping.entries, buttonMapping, { it.label }) {
                    buttonMapping = it
                }
            },
            (if (autoResume) "Continuar de onde parou: sim" else "Continuar de onde parou: não") to {
                autoResume = !autoResume
                if (!autoResume) resumeFile.delete()
            },
        )
        showSubDialog(
            AlertDialog.Builder(this)
                .setTitle("Controles")
                .setItems(options.map { it.first }.toTypedArray()) { _, which -> options[which].second() },
        )
    }

    private fun <T> showChoice(title: String, values: List<T>, current: T, label: (T) -> String, onPick: (T) -> Unit) {
        showSubDialog(
            AlertDialog.Builder(this)
                .setTitle(title)
                .setSingleChoiceItems(values.map(label).toTypedArray(), values.indexOf(current)) { dialog, which ->
                    onPick(values[which])
                    dialog.dismiss()
                },
        )
    }

    private fun toast(message: String) = Toast.makeText(this, message, Toast.LENGTH_SHORT).show()

    // endregion

    companion object {
        private const val SLOTS = 3
        private const val MIN_PRESS_MS = 50L
        private const val FAST_FORWARD_SPEED = 3

        private val GAMEPAD_KEYS = setOf(
            KeyEvent.KEYCODE_DPAD_UP, KeyEvent.KEYCODE_DPAD_DOWN, KeyEvent.KEYCODE_DPAD_LEFT, KeyEvent.KEYCODE_DPAD_RIGHT,
            KeyEvent.KEYCODE_DPAD_UP_LEFT, KeyEvent.KEYCODE_DPAD_UP_RIGHT, KeyEvent.KEYCODE_DPAD_DOWN_LEFT, KeyEvent.KEYCODE_DPAD_DOWN_RIGHT,
            KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_BUTTON_X, KeyEvent.KEYCODE_BUTTON_Y,
            KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_R1, KeyEvent.KEYCODE_BUTTON_L2, KeyEvent.KEYCODE_BUTTON_R2,
            KeyEvent.KEYCODE_BUTTON_THUMBL, KeyEvent.KEYCODE_BUTTON_THUMBR,
            KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_BUTTON_SELECT,
        )

        /** Android reports the Xbox layout (A = south); the RetroPad uses the SNES layout (B = south). */
        private fun toRetroPad(keyCode: Int): Int = when (keyCode) {
            KeyEvent.KEYCODE_BUTTON_A -> KeyEvent.KEYCODE_BUTTON_B
            KeyEvent.KEYCODE_BUTTON_B -> KeyEvent.KEYCODE_BUTTON_A
            KeyEvent.KEYCODE_BUTTON_X -> KeyEvent.KEYCODE_BUTTON_Y
            KeyEvent.KEYCODE_BUTTON_Y -> KeyEvent.KEYCODE_BUTTON_X
            else -> keyCode
        }
    }
}
