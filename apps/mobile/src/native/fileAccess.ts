import {Platform, Share} from 'react-native';
import {
  keepLocalCopy,
  saveDocuments,
  type DocumentPickerResponse,
  type FileToCopy,
} from '@react-native-documents/picker';

import type {MediaSource} from './NativeVelmux';
import * as engineClient from './engineClient';

/**
 * Durable, user-visible file access (the platform layer of the media
 * lifecycle).
 *
 * Architecture: the engine's contract is LOCAL PATHS — every backend opens
 * `source.uri` (and the external subtitle / replace-audio / cover-art URIs)
 * through a file://-or-absolute-path resolver, and refused `content://`
 * URIs with honest errors ("needs a local file") before this layer existed.
 * This module is the missing platform half:
 *
 *  - IMPORT: the system document picker returns `content://` URIs (Android;
 *    readable only through the Storage Access Framework — FFmpeg and
 *    AVFoundation cannot open them) and security-scoped in-place `file://`
 *    URLs (iOS; valid only while the app holds scope, which does not
 *    survive restarts). `importPickedFiles` copies each picked file into
 *    the app's DURABLE storage once, at pick time, and returns local
 *    sources. Copies live next to the engine's own data (documentDirectory
 *    / filesDir, NOT the OS-purgeable cache), so inspection, samples,
 *    jobs, recovery, retry and "re-use settings" all read the same durable
 *    file across process deaths. Originals are never modified.
 *
 *  - EXPORT: engine outputs live in app-private storage by design. Users
 *    reach them through the system's own destinations — `saveOutput`
 *    presents the platform save UI (ACTION_CREATE_DOCUMENT on Android,
 *    the Files picker on iOS) and the app streams the file to the
 *    user-chosen location; the app's copy is kept (history keeps working).
 *
 *  - SHARE: Android needs a FileProvider content URI through the native
 *    engine module (RN core's Share cannot share files there); iOS's core
 *    Share handles file:// URLs directly.
 *
 * No storage permissions are requested anywhere: SAF/document-picker flows
 * need none, and none are added just to make a section appear in system
 * settings.
 */

/** One picked file that failed the durable-import copy. */
export interface ImportFailure {
  name: string;
  reason: string;
}

/** Result of importing a whole pick: sources plus honest per-file failures. */
export interface ImportResult {
  sources: MediaSource[];
  failures: ImportFailure[];
}

/**
 * Copies picked files into durable app storage and returns engine-ready
 * local sources. Never throws for per-file copy errors — they are reported
 * through `failures` so callers can proceed with the successes and surface
 * the rest (the same honest partial-failure posture as batch start).
 */
export async function importPickedFiles(
  picked: readonly DocumentPickerResponse[],
): Promise<ImportResult> {
  if (picked.length === 0) {
    return {sources: [], failures: []};
  }
  const filesToCopy = picked.map(file => ({
    uri: file.uri,
    fileName: file.name ?? `picked-${file.size ?? 0}`,
  }));
  const responses = await keepLocalCopy({
    // Non-empty by the early return above; the library's type demands a
    // first element, and .map() alone cannot express that.
    files: filesToCopy as [FileToCopy, ...FileToCopy[]],
    // Durable by design: the app's document directory (iOS) / filesDir
    // (Android). The cache directory is OS-purgeable and would break
    // recovery, retry and history re-runs.
    destination: 'documentDirectory',
  });
  const sources: MediaSource[] = [];
  const failures: ImportFailure[] = [];
  picked.forEach((file, index) => {
    const name = file.name ?? 'Untitled media';
    const copy = responses[index];
    const localUri =
      copy != null && copy.status === 'success' ? copy.localUri : null;
    if (localUri == null || localUri.length === 0) {
      const reason =
        copy != null && copy.status === 'error'
          ? copy.copyError
          : 'the copy did not complete';
      failures.push({name, reason});
      return;
    }
    sources.push({uri: localUri, displayName: name});
  });
  return {sources, failures};
}

/** Single-file convenience wrapper around [importPickedFiles]. */
export async function importPickedFile(
  picked: DocumentPickerResponse,
): Promise<{source: MediaSource | null; failure: ImportFailure | null}> {
  const {sources, failures} = await importPickedFiles([picked]);
  if (failures.length > 0) {
    return {source: null, failure: failures[0]};
  }
  return {source: sources[0] ?? null, failure: null};
}

/** Outcome of presenting the system save UI for one output file. */
export interface SaveResult {
  /** True when the file was written to the user-chosen destination. */
  saved: boolean;
  /** True when the user dismissed the system save UI. Not an error. */
  canceled: boolean;
  /** The destination URI on success, when the platform reports one. */
  uri: string | null;
  /** Human-readable failure reason when `saved` is false and not canceled. */
  error: string | null;
}

/**
 * Presents the platform save UI and writes the output file to the
 * destination the user picks (file manager / Downloads / Files app).
 * The app's own copy in private storage is kept — history, re-share and
 * re-export keep working.
 */
export async function saveOutput(outputPath: string, outputName: string): Promise<SaveResult> {
  const mimeType = mimeForName(outputName);
  const sourceUri = outputPath.startsWith('file://')
    ? outputPath
    : `file://${outputPath}`;
  try {
    const [saved] = await saveDocuments({
      sourceUris: [sourceUri],
      mimeType: mimeType ?? undefined,
      fileName: outputName,
      // iOS: copy (keep the app's original); Android always copies.
      copy: true,
    });
    if (saved?.error != null) {
      return {saved: false, canceled: false, uri: null, error: saved.error};
    }
    return {
      saved: true,
      canceled: false,
      uri: saved?.uri ?? null,
      error: null,
    };
  } catch (error) {
    if (isUserCanceledSave(error)) {
      return {saved: false, canceled: true, uri: null, error: null};
    }
    return {
      saved: false,
      canceled: false,
      uri: null,
      error: error instanceof Error ? error.message : 'the save failed',
    };
  }
}

/**
 * Shares one output file through the platform share sheet. On Android this
 * goes through the native engine module (FileProvider content URI — RN
 * core's Share API is text-only there); on iOS through core Share with a
 * file:// URL.
 */
export async function shareOutput(
  outputPath: string,
  outputName: string,
): Promise<boolean> {
  const mimeType = mimeForName(outputName);
  if (Platform.OS === 'android') {
    try {
      return await engineClient.shareOutputFile(outputPath, mimeType ?? '', outputName);
    } catch {
      return false;
    }
  }
  const url = outputPath.startsWith('file://')
    ? outputPath
    : `file://${outputPath}`;
  try {
    await Share.share({title: outputName, url});
    return true;
  } catch {
    return false;
  }
}

/**
 * MIME type for an output name, for the system save/share UIs. Derived
 * from the extension the engine chose for the container.
 */
export function mimeForName(name: string): string | null {
  const extension = name.slice(name.lastIndexOf('.') + 1).toLowerCase();
  const table: Record<string, string> = {
    mp4: 'video/mp4',
    m4v: 'video/mp4',
    mov: 'video/quicktime',
    mkv: 'video/x-matroska',
    webm: 'video/webm',
    avi: 'video/x-msvideo',
    ts: 'video/mp2t',
    '3gp': 'video/3gpp',
    m4a: 'audio/mp4',
    mp3: 'audio/mpeg',
    wav: 'audio/wav',
    flac: 'audio/flac',
    ogg: 'audio/ogg',
    opus: 'audio/opus',
    aac: 'audio/aac',
    jpg: 'image/jpeg',
    jpeg: 'image/jpeg',
    png: 'image/png',
    srt: 'application/x-subrip',
    vtt: 'text/vtt',
    ass: 'text/plain',
  };
  return table[extension] ?? null;
}

/** Narrowing for the save-documents cancellation error shape. */
function isUserCanceledSave(error: unknown): boolean {
  return (
    typeof error === 'object' &&
    error !== null &&
    'code' in error &&
    (error as {code?: string}).code === 'OPERATION_CANCELED'
  );
}
