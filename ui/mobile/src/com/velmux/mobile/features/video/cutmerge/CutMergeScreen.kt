package com.velmux.mobile.features.video.cutmerge

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.velmux.mobile.Strings
import com.velmux.mobile.components.Section
import com.velmux.mobile.engine.EngineClient
import com.velmux.mobile.engine.FieldChange
import com.velmux.mobile.engine.Fields
import com.velmux.mobile.engine.SessionSnapshot
import com.velmux.mobile.features.workflow.OptionFieldRow
import com.velmux.mobile.features.workflow.WorkflowScreenHost
import com.velmux.mobile.features.workflow.editors.ChoiceRow
import com.velmux.mobile.features.workflow.editors.MergeRulesEditor
import com.velmux.mobile.features.workflow.editors.RemovedRangesEditor
import com.velmux.mobile.features.workflow.editors.SplitRulesEditor
import com.velmux.mobile.features.workflow.editors.TimeRangeEditor
import com.velmux.mobile.features.workflow.editors.mergeAudioChange
import com.velmux.mobile.features.workflow.mergeLongerDuration
import com.velmux.mobile.features.workflow.outputNameOf
import com.velmux.mobile.features.workflow.payloadOf
import com.velmux.mobile.features.workflow.removedRangesJson
import com.velmux.mobile.features.workflow.toJson
import com.velmux.mobile.features.workflow.timelineMode
import com.velmux.mobile.features.workflow.toRemovedRanges
import com.velmux.mobile.features.workflow.toJson
import com.velmux.mobile.features.workflow.toSplitRules
import com.velmux.mobile.features.workflow.toUsRange
import com.velmux.mobile.features.workflow.toSplitRules
import com.velmux.mobile.features.workflow.toUsRange
import com.velmux.mobile.navigation.Navigator

/**
 * Cut & Merge (mandate §27): the four focused timeline operations —
 * Trim, Cut, Split, Merge — over shared timeline infrastructure. The
 * mode selector switches the session payload (the core replaces it with
 * mode defaults); each mode composes its specific editor.
 */
@Composable
fun CutMergeScreen(
    client: EngineClient,
    navigator: Navigator,
    sessionId: String?,
    isVideo: Boolean,
    modifier: Modifier = Modifier,
) {
    WorkflowScreenHost(
        operation = if (isVideo) "video-cutmerge" else "audio-cutmerge",
        client = client,
        navigator = navigator,
        sessionId = sessionId,
        submitLabel = Strings.CUTMERGE_ACTION,
        modifier = modifier,
    ) { snapshot, controller ->
        Section(title = Strings.TIMELINE_MODE_SECTION) {
            listOf(
                "trim" to Strings.TIMELINE_MODE_TRIM,
                "cut" to Strings.TIMELINE_MODE_CUT,
                "split" to Strings.TIMELINE_MODE_SPLIT,
                "merge" to Strings.TIMELINE_MODE_MERGE,
            ).forEach { (modeId, label) ->
                ChoiceRow(
                    label = label,
                    selected = snapshot.timelineMode == modeId,
                    onClick = {
                        controller.select(
                            snapshot,
                            FieldChange(field = Fields.TIMELINE_MODE, valueId = modeId),
                        )
                    },
                )
            }
        }

        when (snapshot.timelineMode) {
            "trim" -> TrimEditor(snapshot = snapshot, controller = controller)
            "cut" -> CutEditor(snapshot = snapshot, controller = controller)
            "split" -> SplitEditor(snapshot = snapshot, controller = controller)
            "merge" -> MergeEditor(
                snapshot = snapshot,
                controller = controller,
                isVideo = isVideo,
            )
        }

        Section(title = Strings.OUTPUT_SECTION) {
            com.velmux.mobile.features.workflow.editors.LabeledInputRow(
                label = Strings.OUTPUT_NAME,
                acceptedValue = snapshot.outputNameOf() ?: "",
                placeholder = Strings.TRIMMED_DEFAULT_NAME,
                keyboardType = androidx.compose.ui.text.input.KeyboardType.Text,
                onApply = { text ->
                    controller.select(
                        snapshot,
                        FieldChange(
                            field = Fields.OUTPUT_NAME,
                            valueId = "custom",
                            payload = com.velmux.mobile.features.workflow.outputNameJson(text),
                        ),
                    )
                },
            )
        }
    }
}

@Composable
private fun TrimEditor(snapshot: SessionSnapshot, controller: com.velmux.mobile.features.workflow.WorkflowController) {
    Section(title = Strings.RANGE_SECTION) {
        TimeRangeEditor(
            range = snapshot.payloadOf(Fields.TIMELINE_RANGES).toUsRange(),
            onApply = { range ->
                controller.select(
                    snapshot,
                    FieldChange(
                        field = Fields.TIMELINE_RANGES,
                        valueId = "custom",
                        payload = range.toJson(),
                    ),
                )
            },
        )
    }
}

@Composable
private fun CutEditor(snapshot: SessionSnapshot, controller: com.velmux.mobile.features.workflow.WorkflowController) {
    Section(title = Strings.REMOVED_SECTION) {
        val removed = snapshot.payloadOf(Fields.TIMELINE_RANGES).toRemovedRanges()
        RemovedRangesEditor(
            ranges = removed,
            onApply = { ranges ->
                controller.select(
                    snapshot,
                    FieldChange(
                        field = Fields.TIMELINE_RANGES,
                        valueId = "custom",
                        payload = removedRangesJson(ranges),
                    ),
                )
            },
        )
    }
}

@Composable
private fun SplitEditor(snapshot: SessionSnapshot, controller: com.velmux.mobile.features.workflow.WorkflowController) {
    Section(title = Strings.SPLIT_SECTION) {
        SplitRulesEditor(
            rules = snapshot.payloadOf(Fields.SPLIT_RULES).toSplitRules(),
            onApply = { rules ->
                controller.select(
                    snapshot,
                    FieldChange(
                        field = Fields.SPLIT_RULES,
                        valueId = rules.mode.wire,
                        payload = rules.toJson(),
                    ),
                )
            },
        )
    }
}

@Composable
private fun MergeEditor(
    snapshot: SessionSnapshot,
    controller: com.velmux.mobile.features.workflow.WorkflowController,
    isVideo: Boolean,
) {
    if (isVideo) {
        Section(title = Strings.MERGE_SECTION) {
            MergeRulesEditor(
                layout = snapshot.valueFor(Fields.MERGE_LAYOUT)?.valueId ?: "sequential",
                audioSelection = snapshot.valueFor(Fields.MERGE_AUDIO)?.valueId ?: "first",
                longerDuration = snapshot.payloadOf(Fields.MERGE_AUDIO).mergeLongerDuration(),
                isVideo = true,
                onSelect = { controller.select(snapshot, it) },
                onLongerDuration = { longer ->
                    controller.select(
                        snapshot,
                        mergeAudioChange(
                            snapshot.valueFor(Fields.MERGE_AUDIO)?.valueId ?: "first",
                            longer,
                        ),
                    )
                },
            )
        }
    }
    // The source order is the pick order; the scaffold renders the source
    // cards in order (mandate §27.4: the user selects and reorders files;
    // re-picking happens from Home).
    if (snapshot.sources.size > 1) {
        Column(modifier = Modifier.padding(horizontal = 16.dp)) {
            Spacer(Modifier.height(4.dp))
        }
    }
}
