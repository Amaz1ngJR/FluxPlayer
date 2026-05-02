#!/usr/bin/env python3
"""
Interactive stream extractor — extract, play, or download web videos.
Supports Bilibili, YouTube, and 1000+ sites via yt-dlp.

Usage:
  python extract_stream.py            # interactive mode
  python extract_stream.py <url>      # skip URL prompt
Requires: yt-dlp, ffplay, ffmpeg
"""

import subprocess
import json
import sys
import os
import platform


def detect_default_browser() -> str:
    if platform.system() == "Darwin":
        try:
            r = subprocess.run(
                ["defaults", "read",
                 "com.apple.LaunchServices/com.apple.launchservices.secure",
                 "LSHandlers"],
                capture_output=True, text=True, timeout=5
            )
            out = r.stdout.lower()
            if "chrome" in out:
                return "chrome"
            if "firefox" in out:
                return "firefox"
            if "edge" in out:
                return "edge"
        except Exception:
            pass
        return "safari"
    return "chrome"


def get_info(url: str) -> dict:
    print("正在解析视频信息，请稍候...")
    browser = detect_default_browser()
    cmd = ["yt-dlp", "-j", "--no-playlist", "--cookies-from-browser", browser, url]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        raise RuntimeError(r.stderr.strip())
    return json.loads(r.stdout)


def format_duration(seconds) -> str:
    if not seconds:
        return "直播/未知"
    s = int(seconds)
    h, s = divmod(s, 3600)
    m, s = divmod(s, 60)
    if h > 0:
        return f"{h:02d}:{m:02d}:{s:02d}"
    return f"{m:02d}:{s:02d}"


def format_filesize(size_bytes) -> str:
    if not size_bytes:
        return "未知"
    for unit in ("B", "KB", "MB", "GB"):
        if abs(size_bytes) < 1024:
            return f"{size_bytes:.1f} {unit}"
        size_bytes /= 1024
    return f"{size_bytes:.1f} TB"


def print_video_info(info: dict):
    print("\n" + "=" * 60)
    print("  视频信息")
    print("=" * 60)
    print(f"  标题:     {info.get('title', '未知')}")
    print(f"  时长:     {format_duration(info.get('duration'))}")
    print(f"  上传者:   {info.get('uploader') or info.get('channel') or '未知'}")
    print(f"  平台:     {info.get('extractor_key') or info.get('extractor') or '未知'}")

    if info.get("view_count") is not None:
        print(f"  播放量:   {info['view_count']:,}")
    if info.get("upload_date"):
        d = info["upload_date"]
        print(f"  上传日期: {d[:4]}-{d[4:6]}-{d[6:]}")
    if info.get("description"):
        desc = info["description"]
        if len(desc) > 120:
            desc = desc[:120] + "..."
        print(f"  简介:     {desc}")

    formats = info.get("formats", [])
    videos = [f for f in formats if f.get("vcodec") != "none" and f.get("height")]
    if videos:
        heights = sorted(set(f["height"] for f in videos), reverse=True)
        quality_str = ", ".join(f"{h}p" for h in heights[:6])
        print(f"  可用画质: {quality_str}")
    print("=" * 60)


def get_best_stream(info: dict) -> tuple[str, str, str, bool]:
    """Return (video_url, audio_url, headers, is_dash)."""
    http_headers = info.get("http_headers", {})
    header_str = "".join(f"{k}: {v}\r\n" for k, v in http_headers.items())

    formats = info.get("formats", [])
    combined = [
        f for f in formats
        if f.get("url") and f.get("vcodec") != "none" and f.get("acodec") != "none"
    ]
    if combined:
        best = combined[-1]
        return best["url"], "", header_str, False

    video = next(
        (f for f in reversed(formats)
         if f.get("url") and f.get("vcodec") != "none"),
        None
    )
    audio = next(
        (f for f in reversed(formats)
         if f.get("url") and f.get("acodec") != "none" and f.get("vcodec") == "none"),
        None
    )
    if video and audio:
        return video["url"], audio["url"], header_str, True
    if video:
        return video["url"], "", header_str, False
    raise RuntimeError("未找到可播放的流")


def action_show_url(info: dict):
    video_url, audio_url, headers, is_dash = get_best_stream(info)
    print("\n--- 播放地址 ---")
    if is_dash:
        print(f"[DASH] 视频和音频为分离流")
        print(f"视频流: {video_url}")
        print(f"音频流: {audio_url}")
    else:
        print(f"播放地址: {video_url}")
    if headers:
        print(f"HTTP Headers: {headers.strip()}")
    print("----------------")


def action_play(info: dict):
    video_url, audio_url, headers, is_dash = get_best_stream(info)
    title = info.get("title", "未知")
    print(f"\n正在播放: {title}")

    if is_dash:
        print("DASH 模式: 使用 ffmpeg 合流后播放...")
        ffmpeg_cmd = [
            "ffmpeg", "-loglevel", "quiet",
            "-headers", headers, "-i", video_url,
            "-headers", headers, "-i", audio_url,
            "-c", "copy", "-f", "matroska", "pipe:1",
        ]
        ffplay_cmd = ["ffplay", "-window_title", title, "-"]
        p1 = subprocess.Popen(ffmpeg_cmd, stdout=subprocess.PIPE)
        p2 = subprocess.Popen(ffplay_cmd, stdin=p1.stdout)
        p1.stdout.close()
        try:
            p2.wait()
        except KeyboardInterrupt:
            p2.terminate()
            p1.terminate()
    else:
        cmd = ["ffplay", "-window_title", title]
        if headers:
            cmd += ["-headers", headers]
        cmd.append(video_url)
        try:
            subprocess.run(cmd)
        except KeyboardInterrupt:
            pass
    print("播放结束。")


def action_download(url: str):
    default_dir = os.path.expanduser("~/Downloads")
    path = input(f"请输入保存路径 (默认: {default_dir}): ").strip()
    if not path:
        path = default_dir
    path = os.path.expanduser(path)

    if not os.path.isdir(path):
        try:
            os.makedirs(path, exist_ok=True)
        except OSError as e:
            print(f"创建目录失败: {e}")
            return

    browser = detect_default_browser()
    cmd = [
        "yt-dlp",
        "-f", "bestvideo+bestaudio/best",
        "--cookies-from-browser", browser,
        "--merge-output-format", "mp4",
        "--newline", "--progress", "--continue",
        "-o", os.path.join(path, "%(title)s.%(ext)s"),
        url,
    ]

    print(f"\n开始下载到: {path}")
    print("按 Ctrl+C 可取消下载\n")
    try:
        proc = subprocess.Popen(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, bufsize=1,
        )
        for line in proc.stdout:
            line = line.rstrip()
            if "[download]" in line:
                print(f"\r{line}", end="", flush=True)
            elif line:
                print(line)
        proc.wait()
        print()
        if proc.returncode == 0:
            print("下载完成!")
        else:
            print(f"下载失败 (exit code {proc.returncode})")
    except KeyboardInterrupt:
        proc.terminate()
        proc.wait()
        print("\n下载已取消。")


def main():
    if len(sys.argv) > 1:
        url = sys.argv[1]
    else:
        url = input("请输入视频网址: ").strip()
        if not url:
            print("未输入网址，退出。")
            sys.exit(1)

    try:
        info = get_info(url)
    except Exception as e:
        print(f"解析失败: {e}")
        sys.exit(1)

    print_video_info(info)

    while True:
        print("\n请选择操作:")
        print("  1. 返回播放地址")
        print("  2. 使用 ffplay 播放")
        print("  3. 下载视频")
        print("  q. 退出")

        choice = input("\n请输入选项 [1/2/3/q]: ").strip().lower()

        if choice == "1":
            action_show_url(info)
        elif choice == "2":
            action_play(info)
        elif choice == "3":
            action_download(url)
        elif choice in ("q", "quit", "exit"):
            print("再见!")
            break
        else:
            print("无效选项，请重新输入。")


if __name__ == "__main__":
    main()
