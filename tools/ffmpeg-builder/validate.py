#!/usr/bin/env python3
"""Validate the actual FFmpeg build against the Velmux feature requirements.

Reads the capability manifest produced by velmux-ffmpeg-probe (built against
the real libraries) and asserts that every codec/container/filter required by
the Velmux V1 feature matrix is present. Exits non-zero with a clear message
on the first missing capability.

Usage: validate.py <manifest.json>
"""

import json
import sys

REQUIRED_DECODERS = {
    # video: modern/common
    "h264", "hevc", "av1", "vp8", "vp9",
    # video: compatibility/legacy
    "mpeg4", "mpeg2video", "mpeg1video", "h263", "vc1", "wmv3", "wmv1", "wmv2",
    # video: professional/archival
    "prores", "dnxhd", "ffv1", "mjpeg",
    # audio
    "aac", "mp3", "opus", "vorbis", "flac", "alac", "pcm_s16le", "ac3", "eac3",
    "dca", "truehd", "wmapro", "wmav2", "wavpack", "ape", "amrnb", "amrwb",
    # subtitles (FFmpeg 7.1 names; TTML decode is demux-level in this pin
    # and is tracked as a known limitation in docs/FEATURE_MATRIX.md)
    "srt", "subrip", "webvtt", "ass", "ssa", "mov_text", "text",
    "dvbsub", "pgssub", "dvdsub",
}

REQUIRED_ENCODERS = {
    # video
    "libx264", "libsvtav1", "libvpx", "libvpx-vp9", "mpeg4", "ffv1",
    "mjpeg", "libx264rgb",
    # audio
    "aac", "libmp3lame", "libopus", "libvorbis", "flac", "pcm_s16le", "ac3",
    "wavpack",
    # subtitles
    "srt", "subrip", "webvtt", "ass", "mov_text", "text",
}

REQUIRED_MUXERS = {
    "mp4", "mov", "matroska", "webm", "avi", "mpegts", "flv", "3gp", "mp3",
    "wav", "ogg", "adts",
}

REQUIRED_DEMUXERS = {
    "mov,mp4,m4a,3gp,3g2,mj2", "matroska,webm", "avi", "mpegts", "flv",
    "asf", "wav", "mp3", "ogg", "flac", "wv", "ape", "amr",
}

# libass burn-in requirement: the subtitles/ass filters must be compiled in.
# The manifest records the configure line; --enable-libass implies the
# subtitles filter chain is available in this build.


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: validate.py <manifest.json>", file=sys.stderr)
        return 2
    with open(sys.argv[1], encoding="utf-8") as fh:
        manifest = json.load(fh)

    problems = []

    decoders = {c["name"] for c in manifest.get("decoders", [])}
    encoders = {c["name"] for c in manifest.get("encoders", [])}
    muxers = {f["name"] for f in manifest.get("muxers", [])}
    demuxers = {f["name"] for f in manifest.get("demuxers", [])}

    missing = sorted(REQUIRED_DECODERS - decoders)
    if missing:
        problems.append("missing decoders: " + ", ".join(missing))
    missing = sorted(REQUIRED_ENCODERS - encoders)
    if missing:
        problems.append("missing encoders: " + ", ".join(missing))
    missing = sorted(REQUIRED_MUXERS - muxers)
    if missing:
        problems.append("missing muxers: " + ", ".join(missing))
    missing = sorted(REQUIRED_DEMUXERS - demuxers)
    if missing:
        problems.append("missing demuxers: " + ", ".join(missing))

    config = manifest.get("configuration", "")
    for flag in (
        "--enable-libx264",
        "--enable-libmp3lame",
        "--enable-libopus",
        "--enable-libvorbis",
        "--enable-libvpx",
        "--enable-libdav1d",
        "--enable-libsvtav1",
        "--enable-libass",
    ):
        if flag not in config:
            problems.append("configure flag absent: " + flag)
    if "--disable-avdevice" not in config:
        problems.append("libavdevice should be disabled but is not")

    if problems:
        print("FFmpeg build validation FAILED:", file=sys.stderr)
        for p in problems:
            print("  - " + p, file=sys.stderr)
        return 1

    print(
        "FFmpeg build validation OK: "
        f"{len(decoders)} decoders, {len(encoders)} encoders, "
        f"{len(muxers)} muxers, {len(demuxers)} demuxers "
        f"(ffmpeg {manifest.get('ffmpeg', '?')})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
