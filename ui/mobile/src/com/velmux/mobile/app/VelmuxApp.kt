package com.velmux.mobile.app

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Icon
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.SnackbarResult
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
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
import com.velmux.mobile.design.VelmuxIcons
import com.velmux.mobile.design.VelmuxTheme
import com.velmux.mobile.engine.EngineClient
import com.velmux.mobile.features.home.HomeScreen
import com.velmux.mobile.features.jobs.JobsScreen
import com.velmux.mobile.features.settings.SettingsController
import com.velmux.mobile.features.settings.SettingsScreen
import com.velmux.mobile.features.workflow.WorkflowPlaceholderScreen
import com.velmux.mobile.navigation.Destination
import com.velmux.mobile.navigation.Navigator
import com.velmux.mobile.platform.AppSettings
import com.velmux.mobile.platform.SettingsStore
import com.velmux.mobile.platform.ThemeMode

/**
 * The root composable (mandate §36): owns the engine client lifecycle
 * (start/stop, listener registration that keeps the 250 ms poll loop
 * alive while the UI exists), the typed navigation, the theme and the ONE
 * coherent error surface (a single snackbar banner driven by
 * EngineClient.banner — never alert spam).
 */
@Composable
fun VelmuxApp(
    client: EngineClient,
    modifier: Modifier = Modifier,
) {
    val navigator = remember { Navigator() }
    val scope = rememberCoroutineScope()
    val settings = remember { SettingsController(SettingsStore(), scope) }
    val appSettings by settings.settings.collectAsState()
    val snackbarHostState = remember { SnackbarHostState() }

    // Client lifecycle: start once with the UI scope; the UI keeps one
    // listener attached (poll loop alive while the app is visible).
    DisposableEffect(client) {
        client.start(scope)
        val listener = client.addListener()
        onDispose {
            listener.dispose()
            client.stop()
        }
    }

    // The single coherent error banner.
    LaunchedEffect(client) {
        client.banner.collect { error ->
            if (error != null) {
                val result = snackbarHostState.showSnackbar(
                    message = "${error.code}: ${error.message}",
                    actionLabel = Strings.BANNER_DISMISS,
                    withDismissAction = true,
                )
                if (result == SnackbarResult.ActionPerformed) {
                    client.dismissBanner()
                }
            }
        }
    }

    VelmuxTheme(themeMode = appSettings.themeMode) {
        val current = navigator.stack.collectAsState().value.last()

        Scaffold(
            modifier = modifier.fillMaxSize(),
            snackbarHost = { SnackbarHost(snackbarHostState) },
            bottomBar = {
                if (current in Destination.topLevel) {
                    BottomBar(current, navigator)
                }
            },
        ) { padding ->
            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(
                        top = padding.calculateTopPadding(),
                        bottom = padding.calculateBottomPadding(),
                    ),
            ) {
                when (current) {
                    Destination.Home -> HomeScreen(client = client, navigator = navigator)
                    Destination.Jobs -> JobsScreen(client = client, navigator = navigator)
                    Destination.Settings -> SettingsScreen(
                        client = client,
                        controller = settings,
                    )
                    is Destination.Workflow -> WorkflowPlaceholderScreen(
                        client = client,
                        navigator = navigator,
                        operation = current.operation,
                        sessionId = current.sessionId,
                    )
                }
            }
        }
    }
}

@Composable
private fun BottomBar(current: Destination, navigator: Navigator) {
    NavigationBar {
        Destination.topLevel.forEach { destination ->
            val (label, icon) = when (destination) {
                Destination.Home -> Strings.NAV_HOME to VelmuxIcons.Home
                Destination.Jobs -> Strings.NAV_JOBS to VelmuxIcons.Jobs
                Destination.Settings -> Strings.NAV_SETTINGS to VelmuxIcons.Settings
                else -> return@forEach
            }
            NavigationBarItem(
                selected = destination == current,
                onClick = { navigator.open(destination) },
                icon = { Icon(imageVector = icon, contentDescription = label) },
                label = { Text(text = label) },
            )
        }
    }
}
