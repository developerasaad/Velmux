import React, {useCallback, useState} from 'react';
import {Alert} from 'react-native';

import {Button} from './Button';
import type {JobSnapshot} from '../native/NativeVelmux';
import * as engineClient from '../native/engineClient';
import {saveOutput, shareOutput} from '../native/fileAccess';
import {useJobsStore} from '../state/jobsStore';

/**
 * Output-file actions for completed jobs (History).
 *
 * Output location: the engine reports the absolute path of the produced
 * file in every job snapshot (`outputPath`); outputs live in the app's
 * private storage by design (history, re-export and recovery keep
 * working off them). Users reach the outside world through the system's
 * own destinations — no storage permissions exist in this app:
 *
 * - Save to…: the platform save UI (Storage Access Framework
 *   ACTION_CREATE_DOCUMENT on Android, the Files picker on iOS) with the
 *   app streaming the file to the user-chosen location. The app's copy is
 *   kept.
 * - Share: on iOS through core Share (file:// URLs); on Android through
 *   the engine module's FileProvider content URI (core Share is text-only
 *   there).
 * - Delete: the engine's `deleteJob` removes the job's durable record; it
 *   never deletes the output file (bridge contract: "outputs are not
 *   touched"). The control is labeled and confirmed accordingly.
 */

/** Snapshot shape including the engine's output-location field. */
type SnapshotWithOutputPath = JobSnapshot & {outputPath?: string | null};

/** The output file's absolute path, or null when the engine has none. */
export function outputLocationOf(job: JobSnapshot): string | null {
  const path = (job as SnapshotWithOutputPath).outputPath;
  return typeof path === 'string' && path.length > 0 ? path : null;
}

export interface OutputActionsProps {
  job: JobSnapshot;
}

export function OutputActions({job}: OutputActionsProps) {
  const deleteJob = useJobsStore(state => state.deleteJob);
  const [removing, setRemoving] = useState(false);
  const [saving, setSaving] = useState(false);
  const [sharing, setSharing] = useState(false);
  const outputPath = outputLocationOf(job);
  const outputName = job.outputName ?? job.sourceName;

  const saveToDestination = useCallback(async () => {
    if (outputPath === null) {
      return;
    }
    setSaving(true);
    try {
      const result = await saveOutput(outputPath, outputName);
      if (!result.saved && !result.canceled) {
        Alert.alert(
          'Could not save',
          result.error ?? 'The file could not be written to the destination.',
        );
      }
    } finally {
      setSaving(false);
    }
  }, [outputName, outputPath]);

  const share = useCallback(async () => {
    if (outputPath === null) {
      return;
    }
    setSharing(true);
    try {
      const shared = await shareOutput(outputPath, outputName);
      if (!shared) {
        Alert.alert(
          'Could not share',
          'No app could receive the file. Try "Save to…" instead.',
        );
      }
    } finally {
      setSharing(false);
    }
  }, [outputName, outputPath]);

  const removeRecord = useCallback(async () => {
    setRemoving(true);
    try {
      const removed = await deleteJob(job.id);
      if (!removed) {
        Alert.alert(
          'Could not remove',
          'The engine did not remove this job record. Pull to refresh and try again.',
        );
      }
    } catch (error) {
      Alert.alert('Could not remove', engineClient.describeEngineError(error));
    } finally {
      setRemoving(false);
    }
  }, [deleteJob, job.id]);

  const confirmRemove = useCallback(() => {
    Alert.alert(
      'Remove from history',
      `Remove the record for "${job.sourceName}" from history? The converted file itself is kept in app storage.`,
      [
        {text: 'Keep record', style: 'cancel'},
        {
          text: 'Remove',
          style: 'destructive',
          onPress: () => {
            removeRecord();
          },
        },
      ],
    );
  }, [job.sourceName, removeRecord]);

  return (
    <>
      <Button
        label="Save to…"
        variant="primary"
        loading={saving}
        disabled={outputPath === null}
        accessibilityLabel={`Save ${outputName} to a destination you choose`}
        onPress={() => {
          saveToDestination();
        }}
      />
      <Button
        label="Share"
        variant="secondary"
        loading={sharing}
        disabled={outputPath === null}
        accessibilityLabel={`Share ${outputName}`}
        onPress={() => {
          share();
        }}
      />
      <Button
        label="Remove"
        variant="dangerGhost"
        loading={removing}
        accessibilityLabel={`Remove ${job.sourceName} from history`}
        onPress={confirmRemove}
      />
    </>
  );
}
