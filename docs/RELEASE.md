# Velmux — Release Engineering

How release builds are produced, what is signed, and what a maintainer must
add for store distribution.

## Versioning

- Release tag: `v<semver>[-rc.N]` (e.g. `v0.1.0-rc.1`). RC tags publish as
  GitHub pre-releases.
- Android: `versionName` = tag without the leading `v`; `versionCode` starts
  at 1 for v0.1.0 and must be bumped for every store upload. Both are
  injectable: `-PvelmuxVersionName=… -PvelmuxVersionCode=…` (defaults live
  in `apps/mobile/android/app/build.gradle`).
- iOS: `MARKETING_VERSION` = the numeric part of the tag; `CURRENT_PROJECT_VERSION`
  = the release workflow run number. Overridden at the `xcodebuild` command
  line, never hand-edited.

## Pipeline

`.github/workflows/release.yml` (tag push `v*` or manual dispatch):

1. `host-tests` — the full host suite runs inside the workflow (pinned
   FFmpeg build + every engine/backend/bridge test through ctest). A release
   is never published from a failing build.
2. `android-release` — requires the pinned FFmpeg Android artifacts from the
   latest successful `ffmpeg-mobile` run (missing artifacts are a hard
   failure, unlike per-push CI which warns and degrades). Produces:
   - release APK and AAB (R8 minified + resource shrinking),
   - `r8-mapping.txt`,
   - native symbol archives (unstripped + stripped .so trees).
3. `ios-release` — same FFmpeg requirement via the packaged xcframework.
   Produces the Release-configuration simulator `.app` and an **unsigned**
   device `.app` (archive-capable).
4. `github-release` — generates `SHA256SUMS.txt` and `manifest.json`
   (version, commit, signing status, FFmpeg identity, artifact hashes),
   then creates the GitHub Release with all artifacts and notes from
   `docs/release-notes/<version>.md`.

## Signing

### Android

CI signs with a release keystore when these repository secrets exist:

| Secret | Meaning |
|---|---|
| `ANDROID_KEYSTORE_BASE64` | base64 of the keystore file |
| `ANDROID_KEYSTORE_PASSWORD` | keystore password |
| `ANDROID_KEY_ALIAS` | key alias |
| `ANDROID_KEY_PASSWORD` | key password |

Without them the release build is **test-signed with the debug keystore** and
the manifest records `debug-keystore (test-signed)`. The artifact is still a
genuine release build (R8, non-debuggable); it is installable for testing but
must never be uploaded to a store.

To sign for distribution yourself from the unsigned artifacts: the AAB/APK
are already signed, so the intended flow is configuring the secrets (delete
and re-run the release workflow). Generating a keystore:

```bash
keytool -genkeypair -v -storetype PKCS12 \
  -keystore velmux-release.keystore -alias velmux \
  -keyalg RSA -keysize 4096 -validity 10000
base64 -w0 velmux-release.keystore   # -> ANDROID_KEYSTORE_BASE64
```

Keep the keystore out of the repository. Losing it means a new application
ID for future store updates.

### iOS

The CI produces signing-independent artifacts. For distribution:

1. Open `apps/mobile/ios/Velmux.xcworkspace` on a Mac with an Apple Developer
   account, or run `xcodebuild archive` with your team/identity.
2. The unsigned device `.app` in the release assets can be re-signed
   (`codesign`) and wrapped, but the clean path is archiving from source at
   the release tag — the pinned FFmpeg xcframework is rebuilt by the release
   workflow's steps and `VelmuxEngine.podspec` picks it up via
   `VELMUX_FFMPEG_ROOT`.

## Local release smoke

`./gradlew assembleRelease -PvelmuxVersionName=… -PvelmuxVersionCode=…` in
`apps/mobile/android` builds the same release variant locally (debug-signed
unless keystore properties are passed).
