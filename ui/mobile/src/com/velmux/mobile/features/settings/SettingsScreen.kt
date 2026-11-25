package com.velmux.mobile.features.settings

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.velmux.mobile.Strings
import com.velmux.mobile.components.LabeledValue
import com.velmux.mobile.components.OptionRow
import com.velmux.mobile.components.Section
import com.velmux.mobile.components.SelectionOption
import com.velmux.mobile.components.SelectionSheet
import com.velmux.mobile.engine.EngineClient
import com.velmux.mobile.engine.EngineStatus
import com.velmux.mobile.platform.AppSettings
import com.velmux.mobile.platform.SettingsStore
import com.velmux.mobile.platform.ThemeMode
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** Goal presets the engine documents (compressionPresetName, types.cpp). */
val GoalPresetOptions: List<SelectionOption> = listOf(
    SelectionOption("balanced", "Balanced", "Quality and size traded evenly"),
    SelectionOption("high-quality", "High quality", "Best fidelity, larger files"),
    SelectionOption("smaller-file", "Smaller file", "Strongest size reduction"),
    SelectionOption("target-size", "Target size", "Hit an explicit size goal"),
)

/** Output containers the engine documents (containerName, types.cpp). */
val OutputContainerOptions: List<SelectionOption> = listOf(
    "mp4", "mkv", "webm", "mov", "avi", "m4v", "mpegts", "3gp",
    "m4a", "ogg", "mp3", "wav", "flac", "asf",
).map { container ->
    SelectionOption(container, container.uppercase(), null)
}

/**
 * Settings controller: persists [AppSettings] through the platform
 * [SettingsStore]; appearance applies immediately through [settings].
 */
class SettingsController(
    private val store: SettingsStore,
    private val scope: CoroutineScope,
) {
    private val _settings = MutableStateFlow(AppSettings())
    val settings: StateFlow<AppSettings> = _settings

    init {
        scope.launch {
            val loaded = withContext(Dispatchers.IO) { store.load() }
            if (loaded != null) {
                _settings.value = loaded
            }
        }
    }

    fun setThemeMode(mode: ThemeMode) = update { it.copy(themeMode = mode) }

    fun setGoalPreset(preset: String) = update { it.copy(defaultGoalPreset = preset) }

    fun setOutputContainer(container: String) =
        update { it.copy(defaultOutputContainer = container) }

    fun setNotifyCompletion(enabled: Boolean) =
        update { it.copy(notifyOnCompletion = enabled) }

    fun setNotifyFailure(enabled: Boolean) = update { it.copy(notifyOnFailure = enabled) }

    private fun update(block: (AppSettings) -> AppSettings) {
        _settings.value = block(_settings.value)
        scope.launch {
            withContext(Dispatchers.IO) { store.save(_settings.value) }
        }
    }
}

/**
 * Settings: appearance (light/dark/system), conversion defaults (goal
 * preset, output container), storage (managed outputs with cleanup),
 * notifications toggles, read-only diagnostics (engine version, ABI check,
 * backends) and about.
 */
@Composable
fun SettingsScreen(
    client: EngineClient,
    controller: SettingsController,
    modifier: Modifier = Modifier,
) {
    val settings by controller.settings.collectAsState()
    val engineInfo by client.engineInfo.collectAsState()
    val engineStatus by client.engineStatus.collectAsState()
    val jobs by client.jobs.collectAsState()
    val scope = rememberCoroutineScope()

    var sheet by remember { mutableStateOf<String?>(null) }

    Column(
        modifier = modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState()),
    ) {
        Column(modifier = Modifier.padding(horizontal = 16.dp)) {
            Spacer(Modifier.height(24.dp))
            Text(
                text = Strings.SETTINGS_TITLE,
                style = MaterialTheme.typography.headlineSmall,
            )
        }

        Section(title = Strings.SETTINGS_APPEARANCE) {
            OptionRow(
                label = Strings.SETTINGS_THEME,
                value = themeLabel(settings.themeMode),
            ) { sheet = SHEET_THEME }
        }

        Section(title = Strings.SETTINGS_CONVERSION) {
            OptionRow(
                label = Strings.SETTINGS_GOAL_PRESET,
                value = settings.defaultGoalPreset,
            ) { sheet = SHEET_GOAL }
            OptionRow(
                label = Strings.SETTINGS_OUTPUT_CONTAINER,
                value = settings.defaultOutputContainer,
            ) { sheet = SHEET_CONTAINER }
        }

        Section(title = Strings.SETTINGS_STORAGE) {
            val managed = jobs.filter { it.state in com.velmux.mobile.engine.JobStates.TERMINAL }
            if (managed.isEmpty()) {
                Text(
                    text = Strings.SETTINGS_MANAGED_OUTPUTS_EMPTY,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            } else {
                Text(
                    text = Strings.SETTINGS_MANAGED_OUTPUTS_COUNT.format(managed.size),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Spacer(Modifier.height(8.dp))
                Row(horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                    LabeledValue(
                        label = "Output files",
                        value = managed.sumOf { it.outputs.size + if (it.outputPath.isNotEmpty()) 1 else 0 }.toString(),
                    )
                    LabeledValue(
                        label = "Total size",
                        value = Strings.formatBytes(managed.sumOf { it.outputSizeBytes }),
                    )
                }
                Spacer(Modifier.height(8.dp))
                // Cleanup: delete managed outputs + records of ALL terminal jobs.
                val cleaned = remember(managed) {
                    {
                        managed.forEach { job ->
                            scope.launch { client.deleteManagedOutput(job) }
                        }
                    }
                }
                com.velmux.mobile.components.ActionChip(
                    label = Strings.SETTINGS_CLEANUP,
                    icon = com.velmux.mobile.design.VelmuxIcons.Delete,
                    destructive = true,
                    onClick = cleaned,
                )
            }
        }

        Section(title = Strings.SETTINGS_NOTIFICATIONS) {
            com.velmux.mobile.components.OptionRow(
                label = Strings.SETTINGS_NOTIFY_COMPLETION,
                value = if (settings.notifyOnCompletion) "On" else "Off",
            ) { controller.setNotifyCompletion(!settings.notifyOnCompletion) }
            com.velmux.mobile.components.OptionRow(
                label = Strings.SETTINGS_NOTIFY_FAILURE,
                value = if (settings.notifyOnFailure) "On" else "Off",
            ) { controller.setNotifyFailure(!settings.notifyOnFailure) }
            Text(
                text = Strings.SETTINGS_NOTIFICATIONS_NOTE,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
            )
        }

        Section(title = Strings.SETTINGS_DIAGNOSTICS) {
            val info = engineInfo
            com.velmux.mobile.components.LabeledValue(
                label = Strings.SETTINGS_ENGINE_VERSION,
                value = info?.engineVersion ?: Strings.SIZE_UNKNOWN,
                modifier = Modifier.padding(vertical = 4.dp),
            )
            com.velmux.mobile.components.LabeledValue(
                label = Strings.SETTINGS_ABI_VERSION,
                value = when (val status = engineStatus) {
                    is EngineStatus.AbiMismatch ->
                        "${status.found} — ${Strings.SETTINGS_ABI_MISMATCH}"
                    is EngineStatus.Ready, is EngineStatus.NotStarted ->
                        "${info?.abiVersion ?: Abi.VERSION} — ${Strings.SETTINGS_ABI_OK}"
                    is EngineStatus.Unavailable -> Strings.SIZE_UNKNOWN
                },
                modifier = Modifier.padding(vertical = 4.dp),
            )
            Spacer(Modifier.height(8.dp))
            Text(
                text = Strings.SETTINGS_BACKENDS,
                style = MaterialTheme.typography.labelMedium,
            )
            val backends = info?.backends.orEmpty()
            if (backends.isEmpty()) {
                Text(
                    text = Strings.SETTINGS_BACKENDS_NONE,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            } else {
                backends.forEach { backend ->
                    Text(
                        text = backend,
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        }

        Section(title = Strings.SETTINGS_ABOUT) {
            com.velmux.mobile.components.LabeledValue(
                label = Strings.SETTINGS_APP_VERSION,
                value = "1.0.0",
                modifier = Modifier.padding(vertical = 4.dp),
            )
            com.velmux.mobile.components.LabeledValue(
                label = Strings.SETTINGS_REPOSITORY,
                value = Strings.SETTINGS_REPOSITORY_URL,
                modifier = Modifier.padding(vertical = 4.dp),
            )
            Spacer(Modifier.height(8.dp))
            Text(
                text = Strings.SETTINGS_LICENSES,
                style = MaterialTheme.typography.labelMedium,
            )
            Text(
                text = Strings.SETTINGS_LICENSES_TEXT,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(vertical = 4.dp),
            )
        }
        Spacer(Modifier.height(24.dp))
    }

    when (sheet) {
        SHEET_THEME -> SelectionSheet(
            title = Strings.SETTINGS_THEME,
            options = listOf(
                SelectionOption("system", Strings.SETTINGS_THEME_SYSTEM),
                SelectionOption("light", Strings.SETTINGS_THEME_LIGHT),
                SelectionOption("dark", Strings.SETTINGS_THEME_DARK),
            ),
            selectedId = settings.themeMode.sheetId,
            onDismiss = { sheet = null },
            onSelect = { option ->
                controller.setThemeMode(
                    when (option.id) {
                        "light" -> ThemeMode.LIGHT
                        "dark" -> ThemeMode.DARK
                        else -> ThemeMode.SYSTEM
                    }
                )
                sheet = null
            },
        )
        SHEET_GOAL -> SelectionSheet(
            title = Strings.SETTINGS_GOAL_PRESET,
            options = GoalPresetOptions,
            selectedId = settings.defaultGoalPreset,
            onDismiss = { sheet = null },
            onSelect = { option ->
                controller.setGoalPreset(option.id)
                sheet = null
            },
        )
        SHEET_CONTAINER -> SelectionSheet(
            title = Strings.SETTINGS_OUTPUT_CONTAINER,
            options = OutputContainerOptions,
            selectedId = settings.defaultOutputContainer,
            onDismiss = { sheet = null },
            onSelect = { option ->
                controller.setOutputContainer(option.id)
                sheet = null
            },
        )
    }
}

private const val SHEET_THEME = "theme"
private const val SHEET_GOAL = "goal"
private const val SHEET_CONTAINER = "container"

private val ThemeMode.sheetId: String
    get() = when (this) {
        ThemeMode.SYSTEM -> "system"
        ThemeMode.LIGHT -> "light"
        ThemeMode.DARK -> "dark"
    }

private fun themeLabel(mode: ThemeMode): String = when (mode) {
    ThemeMode.SYSTEM -> Strings.SETTINGS_THEME_SYSTEM
    ThemeMode.LIGHT -> Strings.SETTINGS_THEME_LIGHT
    ThemeMode.DARK -> Strings.SETTINGS_THEME_DARK
}

private val Abi get() = com.velmux.mobile.engine.Abi
