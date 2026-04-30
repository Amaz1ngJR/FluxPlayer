#!/usr/bin/env python3
"""
Extract playable stream info from a webpage (Bilibili, YouTube, etc.)
Usage:
  python extract_stream.py <url>          # print stream info + ffplay command
  python extract_stream.py <url> --play   # launch ffplay directly
Requires: yt-dlp, ffplay
"""

import subprocess
import json
import sys


def get_info(url: str) -> dict:
    r = subprocess.run(
        ["yt-dlp", "-j", "--no-playlist", url],
        capture_output=True, text=True, timeout=60
    )
    if r.returncode != 0:
        raise RuntimeError(r.stderr.strip())
    return json.loads(r.stdout)


def build_ffplay_cmd(info: dict) -> list[str]:
    """Return an ffplay command that plays the best available stream."""
    http_headers = info.get("http_headers", {})
    header_str = "".join(f"{k}: {v}\r\n" for k, v in http_headers.items())

    formats = info.get("formats", [])
    # prefer combined video+audio
    combined = [f for f in formats if f.get("url") and f.get("vcodec") != "none" and f.get("acodec") != "none"]
    if combined:
        best = combined[-1]
        return ["ffplay", "-headers", header_str, best["url"]]

    # DASH: separate video + audio — use ffplay with two inputs via lavfi
    video = next((f for f in reversed(formats) if f.get("url") and f.get("vcodec") != "none"), None)
    audio = next((f for f in reversed(formats) if f.get("url") and f.get("acodec") != "none" and f.get("vcodec") == "none"), None)
    if video and audio:
        # merge via ffmpeg pipe
        return [
            "ffplay", "-",
            # caller should use ffmpeg to merge; return marker for caller to handle
            "__dash__", video["url"], audio["url"], header_str
        ]
    raise RuntimeError("No playable format found")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <url> [--play]")
        sys.exit(1)

    url = sys.argv[1]
    play = "--play" in sys.argv

    print(f"Extracting: {url}", file=sys.stderr)
    info = get_info(url)
    print(f"Title: {info.get('title')}", file=sys.stderr)

    cmd = build_ffplay_cmd(info)

    if cmd[2] == "__dash__":
        _, _, _, video_url, audio_url, header_str = cmd
        print(f"Format: DASH (video+audio separate)", file=sys.stderr)
        # merge with ffmpeg and pipe to ffplay
        ffmpeg_cmd = [
            "ffmpeg", "-loglevel", "quiet",
            "-headers", header_str, "-i", video_url,
            "-headers", header_str, "-i", audio_url,
            "-c", "copy", "-f", "matroska", "pipe:1"
        ]
        ffplay_cmd = ["ffplay", "-"]
        print("Command: ffmpeg [...] | ffplay -")
        if play:
            p1 = subprocess.Popen(ffmpeg_cmd, stdout=subprocess.PIPE)
            p2 = subprocess.Popen(ffplay_cmd, stdin=p1.stdout)
            p1.stdout.close()
            p2.wait()
    else:
        print(f"URL: {cmd[-1][:100]}...")
        print(f"Command: ffplay -headers '...' '<url>'")
        if play:
            subprocess.run(cmd)
