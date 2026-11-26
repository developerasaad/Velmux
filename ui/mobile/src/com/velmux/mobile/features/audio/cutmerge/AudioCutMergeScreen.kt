package com.velmux.mobile.features.audio.cutmerge

import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import com.velmux.mobile.engine.EngineClient
import com.velmux.mobile.features.video.cutmerge.CutMergeScreen
import com.velmux.mobile.navigation.Navigator

/** Audio Cut & Merge (mandate §27.6): the shared timeline screen. */
@Composable
fun AudioCutMergeScreen(
    client: EngineClient,
    navigator: Navigator,
    sessionId: String?,
    modifier: Modifier = Modifier,
) {
    CutMergeScreen(
        client = client,
        navigator = navigator,
        sessionId = sessionId,
        isVideo = false,
        modifier = modifier,
    )
}
