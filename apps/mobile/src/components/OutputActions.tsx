import React, {useCallback, useState} from 'react';
import {Alert, Platform, Share} from 'react-native';

import {Button} from './Button';
import type {JobSnapshot} from '../native/NativeVelmux';
import * as engineClient from '../native/engineClient';
import {useJobsStore} from '../state/jobsStore';

/**
 * Output-file actions for completed jobs (History).
 *
 * Output location: the engine reports the absolute path of the produced
 * file in every job snapshot — C++ `JobSnapshot::outputPath` ("Absolute
 * path of the produced file once known (for open/share)"), serialized by
 * the bridge as `outputPath` and passed through to JS unchanged (empty
 * string until the engine assigns it at run start). The TS contract
 * declares the field as it lands with the output-location contract wave;
 * `outputLocationOf` reads it defensively so this component compiles and
 * behaves correctly against either shape.
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

/**
 * Platform honesty (checked against the engine contract and RN 0.88):
 *
 * - Share: RN core `Share` accepts a `url` only on iOS; the engine returns
 *   a bare absolute path, which iOS shares correctly once prefixed with
 *   `file://`. On Android the core `Share` API can only share text — a
 *   local file needs a `content://` URI from a FileProvider, which is a
 *   native-manifest change outside this JS layer. So Share is offered on
 *   iOS only; no dead button is shipped on Android.
 * - Play/Open: `Linking.openURL` with a `file://` URI is unreliable on iOS
 *   and requires `content://` on Android, and RN core has no document
 *   interaction controller or inline player. On iOS the share sheet
 *   already offers "open in app" targets, so no separate Play/Open button
 *   is shipped rather than one that pretends to work.
 * - Delete: the engine's `deleteJob` removes the job's durable record; it
 *   never deletes the output file (bridge contract: "outputs are not
 *   touched"). The control is labeled and confirmed accordingly.
 */
export function OutputActions({job}: OutputActionsProps) {
  const deleteJob = useJobsStore(state => state.deleteJob);
  const [removing, setRemoving] = useState(false);
  const outputPath = outputLocationOf(job);

  const shareOutput = useCallback(async () => {
    if (outputPath === null) {
      return;
    }
    // The engine reports a bare absolute POSIX path, not a file URI.
    const url = outputPath.startsWith('file://')
      ? outputPath
      : `file://${outputPath}`;
    try {
      await Share.share({
        title: job.outputName ?? job.sourceName,
        url,
      });
    } catch {
      // Sharing was canceled or the system refused the URL; the share
      // sheet itself is the user-facing feedback, so nothing is faked here.
    }
  }, [job.outputName, job.sourceName, outputPath]);

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

  if (Platform.OS !== 'ios') {
    // Android: no honest file-share path exists in this build (see the
    // platform notes above). Record removal still works everywhere.
    return (
      <Button
        label="Remove"
        variant="dangerGhost"
        loading={removing}
        accessibilityLabel={`Remove ${job.sourceName} from history`}
        onPress={confirmRemove}
      />
    );
  }

  return (
    <>
      <Button
        label="Share"
        variant="secondary"
        disabled={outputPath === null}
        accessibilityLabel={`Share ${job.outputName ?? job.sourceName}`}
        onPress={() => {
          shareOutput();
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
