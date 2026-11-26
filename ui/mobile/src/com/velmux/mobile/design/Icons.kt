package com.velmux.mobile.design

import androidx.compose.material.icons.Icons
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
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.addPathNodes
import androidx.compose.ui.unit.dp

/**
 * Material vector icons ONLY (design rule: never emoji). Standard icons
 * come from the material-icons core set (re-exported here as one place to
 * import from); the Velmux-specific glyphs (workflow tiles, pause, stop,
 * open) are hand-drawn Material-style 24dp vector paths declared through
 * the stable addPathNodes API.
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
    val VideoConvert: ImageVector by icon(
        "Velmux.VideoConvert",
        "M4,5h16v14h-16z M10,9.5l5,2.5l-5,2.5z",
    )

    /** video-crop (material crop glyph). */
    val Crop: ImageVector by icon(
        "Velmux.Crop",
        "M17,15h2V7c0,-1.1 -0.9,-2 -2,-2H9v2h8v8z " +
            "M7,17V1H5v4H1v2h4v10c0,1.1 0.9,2 2,2h10v4h2v-4h4v-2H7z",
    )

    /** cutmerge (video/audio): two segments around a cut point. */
    val CutMerge: ImageVector by icon(
        "Velmux.CutMerge",
        "M3,7h6v2.5h-3.5v5h3.5v2.5h-6z " +
            "M15,7h6v10h-6v-2.5h3.5v-5h-3.5z " +
            "M12,9l3,3l-3,3l-3,-3z",
    )

    /** extract-frames: a contact sheet of frames. */
    val Frames: ImageVector by icon(
        "Velmux.Frames",
        "M3,5h18v14h-18z " +
            "M5,7h4v4h-4z M10,7h4v4h-4z M15,7h4v4h-4z " +
            "M5,13h4v4h-4z M10,13h9v4h-9z",
    )

    /** extract-subtitles: a card with caption lines. */
    val Subtitles: ImageVector by icon(
        "Velmux.Subtitles",
        "M3,5h18v14h-18z " +
            "M6,11h5v2h-5z M13,11h5v2h-5z M6,15h9v2h-9z",
    )

    /** extract-audio / audio-convert / audio-cutmerge (music note). */
    val Audio: ImageVector by icon(
        "Velmux.Audio",
        "M12,3v10.55c-0.59,-0.34 -1.27,-0.55 -2,-0.55c-2.21,0 -4,1.79 -4,4" +
            "s1.79,4 4,4s4,-1.79 4,-4V7h4V3z",
    )

    // -- controls ---------------------------------------------------------------

    /** Pause job (not in the material-icons core set). */
    val Pause: ImageVector by icon(
        "Velmux.Pause",
        "M7,5h3.5v14h-3.5z M13.5,5h3.5v14h-3.5z",
    )

    /** Stop (used for the canceling state). */
    val Stop: ImageVector by icon("Velmux.Stop", "M7,7h10v10h-10z")

    /** Open a produced output externally (material open-in-new glyph). */
    val Open: ImageVector by icon(
        "Velmux.Open",
        "M19,19H5V5h7V3H5c-1.11,0 -2,0.9 -2,2v14c0,1.1 0.89,2 2,2h14" +
            "c1.1,0 2,-0.9 2,-2v-7h-2V19z " +
            "M14,3v2h3.59l-9.83,9.83l1.41,1.41L19,6.41V10h2V3z",
    )

    private fun icon(name: String, pathData: String): Lazy<ImageVector> =
        lazy {
            ImageVector.Builder(
                name = name,
                defaultWidth = 24.dp,
                defaultHeight = 24.dp,
                viewportWidth = 24f,
                viewportHeight = 24f,
            ).addPath(
                pathData = addPathNodes(pathData),
                fill = SolidColor(Color.Black),
            ).build()
        }
}
