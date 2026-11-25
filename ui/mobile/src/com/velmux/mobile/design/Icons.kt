package com.velmux.mobile.design

import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.materialIcon
import androidx.compose.material.icons.rounded.CheckCircle
import androidx.compose.material.icons.rounded.Close
import androidx.compose.material.icons.rounded.Delete
import androidx.compose.material.icons.rounded.Home
import androidx.compose.material.icons.rounded.Info
import androidx.compose.material.icons.rounded.KeyboardArrowDown
import androidx.compose.material.icons.rounded.KeyboardArrowUp
import androidx.compose.material.icons.rounded.List
import androidx.compose.material.icons.rounded.PlayArrow
import androidx.compose.material.icons.rounded.Refresh
import androidx.compose.material.icons.rounded.Search
import androidx.compose.material.icons.rounded.Settings
import androidx.compose.material.icons.rounded.Share
import androidx.compose.material.icons.rounded.Warning
import androidx.compose.ui.graphics.vector.ImageVector

/**
 * Material vector icons ONLY (design rule: never emoji). Standard icons
 * come from the material-icons core set (re-exported here as one place to
 * import from); the Velmux-specific glyphs (workflow tiles, pause, stop,
 * open) are hand-drawn Material-style 24dp vector paths.
 */
object VelmuxIcons {

    // -- standard core icons ---------------------------------------------------

    val Home: ImageVector get() = Icons.Rounded.Home
    val Jobs: ImageVector get() = Icons.Rounded.List
    val Settings: ImageVector get() = Icons.Rounded.Settings
    val Search: ImageVector get() = Icons.Rounded.Search
    val Share: ImageVector get() = Icons.Rounded.Share
    val Delete: ImageVector get() = Icons.Rounded.Delete
    val Refresh: ImageVector get() = Icons.Rounded.Refresh
    val Close: ImageVector get() = Icons.Rounded.Close
    val Play: ImageVector get() = Icons.Rounded.PlayArrow
    val Check: ImageVector get() = Icons.Rounded.CheckCircle
    val Warning: ImageVector get() = Icons.Rounded.Warning
    val Info: ImageVector get() = Icons.Rounded.Info
    val Expand: ImageVector get() = Icons.Rounded.KeyboardArrowDown
    val Collapse: ImageVector get() = Icons.Rounded.KeyboardArrowUp

    // -- workflow tiles -------------------------------------------------------

    /** video-convert: a media file with a play arrow. */
    val VideoConvert: ImageVector by icon("Velmux.VideoConvert") {
        path {
            moveTo(4.0f, 5.0f)
            horizontalLineToRelative(16.0f)
            verticalLineToRelative(14.0f)
            horizontalLineToRelative(-16.0f)
            close()
            moveTo(10.0f, 9.5f)
            lineToRelative(5.0f, 2.5f)
            lineToRelative(-5.0f, 2.5f)
            close()
        }
    }

    /** video-crop (material crop glyph). */
    val Crop: ImageVector by icon("Velmux.Crop") {
        path {
            moveTo(17.0f, 15.0f)
            horizontalLineToRelative(2.0f)
            verticalLineTo(7.0f)
            curveToRelative(0.0f, -1.1f, -0.9f, -2.0f, -2.0f, -2.0f)
            horizontalLineTo(9.0f)
            verticalLineToRelative(2.0f)
            horizontalLineToRelative(8.0f)
            verticalLineTo(15.0f)
            close()
            moveTo(7.0f, 17.0f)
            verticalLineTo(1.0f)
            horizontalLineTo(5.0f)
            verticalLineToRelative(4.0f)
            horizontalLineTo(1.0f)
            verticalLineToRelative(2.0f)
            horizontalLineToRelative(4.0f)
            verticalLineToRelative(10.0f)
            curveToRelative(0.0f, 1.1f, 0.9f, 2.0f, 2.0f, 2.0f)
            horizontalLineToRelative(10.0f)
            verticalLineToRelative(4.0f)
            horizontalLineToRelative(2.0f)
            verticalLineToRelative(-4.0f)
            horizontalLineToRelative(4.0f)
            verticalLineToRelative(-2.0f)
            horizontalLineTo(7.0f)
            close()
        }
    }

    /** cutmerge (video/audio): two segments around a cut point. */
    val CutMerge: ImageVector by icon("Velmux.CutMerge") {
        path {
            moveTo(3.0f, 7.0f)
            horizontalLineToRelative(6.0f)
            verticalLineToRelative(2.5f)
            horizontalLineToRelative(-3.5f)
            verticalLineToRelative(5.0f)
            horizontalLineToRelative(3.5f)
            verticalLineToRelative(2.5f)
            horizontalLineToRelative(-6.0f)
            close()
            moveTo(15.0f, 7.0f)
            horizontalLineToRelative(6.0f)
            verticalLineToRelative(10.0f)
            horizontalLineToRelative(-6.0f)
            verticalLineToRelative(-2.5f)
            horizontalLineToRelative(3.5f)
            verticalLineToRelative(-5.0f)
            horizontalLineToRelative(-3.5f)
            close()
            moveTo(12.0f, 9.0f)
            lineToRelative(3.0f, 3.0f)
            lineToRelative(-3.0f, 3.0f)
            lineToRelative(-3.0f, -3.0f)
            close()
        }
    }

    /** extract-frames: a filmstrip grid. */
    val Frames: ImageVector by icon("Velmux.Frames") {
        path {
            moveTo(3.0f, 5.0f)
            horizontalLineToRelative(18.0f)
            verticalLineToRelative(14.0f)
            horizontalLineToRelative(-18.0f)
            close()
            moveTo(5.0f, 7.0f)
            horizontalLineToRelative(4.0f)
            verticalLineToRelative(4.0f)
            horizontalLineToRelative(-4.0f)
            close()
            moveTo(10.0f, 7.0f)
            horizontalLineToRelative(4.0f)
            verticalLineToRelative(4.0f)
            horizontalLineToRelative(-4.0f)
            close()
            moveTo(15.0f, 7.0f)
            horizontalLineToRelative(4.0f)
            verticalLineToRelative(4.0f)
            horizontalLineToRelative(-4.0f)
            close()
            moveTo(5.0f, 13.0f)
            horizontalLineToRelative(4.0f)
            verticalLineToRelative(4.0f)
            horizontalLineToRelative(-4.0f)
            close()
            moveTo(10.0f, 13.0f)
            horizontalLineToRelative(9.0f)
            verticalLineToRelative(4.0f)
            horizontalLineToRelative(-9.0f)
            close()
        }
    }

    /** extract-subtitles: a card with caption lines. */
    val Subtitles: ImageVector by icon("Velmux.Subtitles") {
        path {
            moveTo(3.0f, 5.0f)
            horizontalLineToRelative(18.0f)
            verticalLineToRelative(14.0f)
            horizontalLineToRelative(-18.0f)
            close()
            moveTo(6.0f, 11.0f)
            horizontalLineToRelative(5.0f)
            verticalLineToRelative(2.0f)
            horizontalLineToRelative(-5.0f)
            close()
            moveTo(13.0f, 11.0f)
            horizontalLineToRelative(5.0f)
            verticalLineToRelative(2.0f)
            horizontalLineToRelative(-5.0f)
            close()
            moveTo(6.0f, 15.0f)
            horizontalLineToRelative(9.0f)
            verticalLineToRelative(2.0f)
            horizontalLineToRelative(-9.0f)
            close()
        }
    }

    /** extract-audio / audio-convert / audio-cutmerge (music note). */
    val Audio: ImageVector by icon("Velmux.Audio") {
        path {
            moveTo(12.0f, 3.0f)
            verticalLineToRelative(10.55f)
            curveToRelative(-0.59f, -0.34f, -1.27f, -0.55f, -2.0f, -0.55f)
            curveToRelative(-2.21f, 0.0f, -4.0f, 1.79f, -4.0f, 4.0f)
            reflectiveCurveToRelative(1.79f, 4.0f, 4.0f, 4.0f)
            reflectiveCurveToRelative(4.0f, -1.79f, 4.0f, -4.0f)
            verticalLineTo(7.0f)
            horizontalLineToRelative(4.0f)
            verticalLineTo(3.0f)
            close()
        }
    }

    // -- controls ---------------------------------------------------------------

    /** Pause job (not in the material-icons core set). */
    val Pause: ImageVector by icon("Velmux.Pause") {
        path {
            moveTo(7.0f, 5.0f)
            horizontalLineToRelative(3.5f)
            verticalLineToRelative(14.0f)
            horizontalLineToRelative(-3.5f)
            close()
            moveTo(13.5f, 5.0f)
            horizontalLineToRelative(3.5f)
            verticalLineToRelative(14.0f)
            horizontalLineToRelative(-3.5f)
            close()
        }
    }

    /** Stop (used for the canceling state). */
    val Stop: ImageVector by icon("Velmux.Stop") {
        path {
            moveTo(7.0f, 7.0f)
            horizontalLineToRelative(10.0f)
            verticalLineToRelative(10.0f)
            horizontalLineToRelative(-10.0f)
            close()
        }
    }

    /** Open a produced output externally (material open-in-new glyph). */
    val Open: ImageVector by icon("Velmux.Open") {
        path {
            moveTo(19.0f, 19.0f)
            horizontalLineTo(5.0f)
            verticalLineTo(5.0f)
            horizontalLineToRelative(7.0f)
            verticalLineTo(3.0f)
            horizontalLineTo(5.0f)
            curveToRelative(-1.11f, 0.0f, -2.0f, 0.9f, -2.0f, 2.0f)
            verticalLineToRelative(14.0f)
            curveToRelative(0.0f, 1.1f, 0.89f, 2.0f, 2.0f, 2.0f)
            horizontalLineToRelative(14.0f)
            curveToRelative(1.1f, 0.0f, 2.0f, -0.9f, 2.0f, -2.0f)
            verticalLineToRelative(-7.0f)
            horizontalLineToRelative(-2.0f)
            verticalLineTo(19.0f)
            close()
            moveTo(14.0f, 3.0f)
            verticalLineToRelative(2.0f)
            horizontalLineToRelative(3.59f)
            lineToRelative(-9.83f, 9.83f)
            lineToRelative(1.41f, 1.41f)
            lineTo(19.0f, 6.41f)
            verticalLineTo(10.0f)
            horizontalLineToRelative(2.0f)
            verticalLineTo(3.0f)
            close()
        }
    }

    private fun icon(
        name: String,
        block: androidx.compose.ui.graphics.vector.ImageVector.Builder.() -> Unit,
    ): Lazy<ImageVector> = lazy { materialIcon(name = name, block = block) }
}
