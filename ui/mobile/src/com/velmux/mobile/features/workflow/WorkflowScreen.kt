package com.velmux.mobile.features.workflow

import com.velmux.mobile.fmt
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import com.velmux.mobile.engine.EngineClient
import com.velmux.mobile.features.audio.converter.AudioConverterScreen
import com.velmux.mobile.features.audio.cutmerge.AudioCutMergeScreen
import com.velmux.mobile.features.video.converter.VideoConverterScreen
import com.velmux.mobile.features.video.crop.CropScreen
import com.velmux.mobile.features.video.cutmerge.CutMergeScreen
import com.velmux.mobile.features.video.extractaudio.ExtractAudioScreen
import com.velmux.mobile.features.video.extractframes.ExtractFramesScreen
import com.velmux.mobile.features.video.extractsubtitles.ExtractSubtitlesScreen
import com.velmux.mobile.navigation.Navigator

/**
 * Routes a workflow destination to its configuration screen — the eight
 * primary workflows (mandate §9) each have a focused screen composed
 * from the shared components.
 */
@Composable
fun WorkflowScreen(
    operation: String,
    client: EngineClient,
    navigator: Navigator,
    sessionId: String?,
    modifier: Modifier = Modifier,
) {
    when (operation) {
        "video-convert" -> VideoConverterScreen(
            client = client,
            navigator = navigator,
            sessionId = sessionId,
            modifier = modifier,
        )
        "video-crop" -> CropScreen(
            client = client,
            navigator = navigator,
            sessionId = sessionId,
            modifier = modifier,
        )
        "video-cutmerge" -> CutMergeScreen(
            client = client,
            navigator = navigator,
            sessionId = sessionId,
            isVideo = true,
            modifier = modifier,
        )
        "audio-cutmerge" -> AudioCutMergeScreen(
            client = client,
            navigator = navigator,
            sessionId = sessionId,
            modifier = modifier,
        )
        "extract-audio" -> ExtractAudioScreen(
            client = client,
            navigator = navigator,
            sessionId = sessionId,
            modifier = modifier,
        )
        "extract-frames" -> ExtractFramesScreen(
            client = client,
            navigator = navigator,
            sessionId = sessionId,
            modifier = modifier,
        )
        "extract-subtitles" -> ExtractSubtitlesScreen(
            client = client,
            navigator = navigator,
            sessionId = sessionId,
            modifier = modifier,
        )
        "audio-convert" -> AudioConverterScreen(
            client = client,
            navigator = navigator,
            sessionId = sessionId,
            modifier = modifier,
        )
        else -> UnknownWorkflowScreen(operation = operation)
    }
}

/** Honest fallback: an operation the UI does not know (future engine). */
@Composable
private fun UnknownWorkflowScreen(operation: String) {
    com.velmux.mobile.components.EmptyState(
        title = com.velmux.mobile.Strings.WORKFLOW_UNKNOWN_TITLE,
        message = com.velmux.mobile.Strings.WORKFLOW_UNKNOWN_BODY.fmt(operation),
    )
}
