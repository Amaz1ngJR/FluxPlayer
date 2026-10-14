#!/usr/bin/env python3
"""生成短测试码流，验证项目动态库的 FLV 解复用与实际解码，不启动 GUI。"""
import argparse
import json
import os
import re
import struct
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def invoke(command, env):
    return subprocess.run(command, env=env, capture_output=True, text=True, timeout=60)


def checked(command, env):
    result = invoke(command, env)
    if result.returncode:
        raise RuntimeError(f"{command}\n{result.stderr}")
    return result.stdout


def read_flv(path):
    data = path.read_bytes()
    assert data[:3] == b"FLV"
    offset = int.from_bytes(data[5:9], "big") + 4
    tags = []
    while offset < len(data):
        assert offset + 15 <= len(data)
        size = int.from_bytes(data[offset + 1:offset + 4], "big")
        timestamp = int.from_bytes(data[offset + 4:offset + 7], "big") | (data[offset + 7] << 24)
        payload = data[offset + 11:offset + 11 + size]
        assert len(payload) == size
        assert int.from_bytes(data[offset + 11 + size:offset + 15 + size], "big") == size + 11
        tags.append((data[offset], timestamp, payload))
        offset += size + 15
    return data[:9], tags


def write_flv(path, header, tags):
    data = bytearray(header + b"\0\0\0\0")
    for kind, timestamp, payload in tags:
        data += bytes([kind]) + len(payload).to_bytes(3, "big")
        data += (timestamp & 0xffffff).to_bytes(3, "big") + bytes([timestamp >> 24]) + b"\0\0\0"
        data += payload + (len(payload) + 11).to_bytes(4, "big")
    path.write_bytes(data)


def enhanced_hevc(path, destination, coded_x=False):
    header, original = read_flv(path)
    tags = []
    for kind, timestamp, payload in original:
        if kind == 18:
            continue
        if kind == 9:
            assert payload[0] & 15 == 12
            packet_type = payload[1]
            if packet_type == 1 and coded_x:
                assert payload[2:5] == b"\0\0\0"
                packet_type = 3
            prefix = bytes([0x80 | (payload[0] & 0x70) | packet_type]) + b"hvc1"
            if packet_type == 1:
                prefix += payload[2:5]
            payload = prefix + payload[5:]
        tags.append((kind, timestamp, payload))
    write_flv(destination, header, tags)


def mp4_configuration(path, wanted):
    # 按 ISO BMFF 的盒长度遍历；FFmpeg 4 的 extradata 已剥离 av1C 四字节头，
    # 所以测试必须取实际容器配置，不能用 ffprobe extradata 冒充 Enhanced FLV 头。
    data = path.read_bytes()
    containers = {b"moov", b"trak", b"mdia", b"minf", b"stbl"}
    def visit(start, end):
        offset = start
        while offset + 8 <= end:
            size, kind = struct.unpack_from(">I4s", data, offset)
            header_size = 8
            if size == 1:
                size = int.from_bytes(data[offset + 8:offset + 16], "big")
                header_size = 16
            elif size == 0:
                size = end - offset
            assert size >= header_size and offset + size <= end
            payload = offset + header_size
            if kind == wanted:
                return data[payload:offset + size]
            child_start = None
            if kind in containers:
                child_start = payload
            elif kind == b"stsd":
                child_start = payload + 8
            elif kind in {b"av01", b"vp09"}:
                child_start = payload + 78
            if child_start is not None:
                result = visit(child_start, offset + size)
                if result is not None:
                    return result
            offset += size
        return None
    result = visit(0, len(data))
    assert result is not None, wanted
    return result


def ivf_flv(source, destination, fourcc, configuration):
    data = source.read_bytes()
    assert data[:4] == b"DKIF"
    rate, scale = struct.unpack_from("<II", data, 16)
    tags = [(9, 0, b"\x90" + fourcc + configuration)]
    offset = int.from_bytes(data[6:8], "little")
    while offset < len(data):
        size, timestamp = struct.unpack_from("<IQ", data, offset)
        offset += 12
        payload = data[offset:offset + size]
        assert len(payload) == size
        flags = 0x91 if len(tags) == 1 else 0xa1
        tags.append((9, round(timestamp * scale * 1000 / rate), bytes([flags]) + fourcc + payload))
        offset += size
    write_flv(destination, b"FLV\x01\x01\x00\x00\x00\x09", tags)


def without_av1_sequence(payload):
    """按 OBU 的 LEB128 长度移除序列头，保留真实编码帧，验证配置标签能独立起播。"""
    output = bytearray()
    offset = 0
    removed = 0
    while offset < len(payload):
        start = offset
        header = payload[offset]
        offset += 1 + bool(header & 4)
        assert header & 2, "测试 OBU 必须包含长度字段"
        size = 0
        for shift in range(0, 56, 7):
            value = payload[offset]
            offset += 1
            size |= (value & 127) << shift
            if not value & 128:
                break
        else:
            raise AssertionError("OBU 长度字段过长")
        offset += size
        assert offset <= len(payload)
        if (header >> 3) & 15 == 1:
            removed += 1
        else:
            output += payload[start:offset]
    return bytes(output), removed


class FixtureSuite:
    def __init__(self, prefix, work):
        self.work = work.resolve()
        self.work.mkdir(parents=True, exist_ok=True)
        self.ffmpeg = str(prefix / "bin" / "ffmpeg")
        self.ffprobe = str(prefix / "bin" / "ffprobe")
        self.base_env = dict(os.environ)
        self.base_env.pop("DYLD_LIBRARY_PATH", None)
        lib_dir = (ROOT / "third_party" / "ffmpeg-macos" / "lib").resolve()
        self.project_env = dict(self.base_env, DYLD_LIBRARY_PATH=str(lib_dir))
        # 先证明进程加载项目库，防止误测系统 FFmpeg 而产生假阳性。
        loaded = invoke([self.ffprobe, "-version"], dict(self.project_env, DYLD_PRINT_LIBRARIES="1"))
        if loaded.returncode or str(lib_dir / "libavformat.58.76.100.dylib") not in loaded.stderr:
            raise RuntimeError("测试进程没有加载项目 libavformat\n" + loaded.stderr)

    def generate(self):
        def encode(name, options, audio=False):
            command = [self.ffmpeg, "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=96x64:rate=12"]
            if audio:
                command += ["-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000"]
            command += ["-t", "1", *options, str(self.work / name)]
            checked(command, self.base_env)

        encode("hevc.mp4", ["-c:v", "libx265", "-x265-params", "log-level=error:pools=1:frame-threads=1:bframes=2",
                            "-tag:v", "hvc1", "-c:a", "aac"], audio=True)
        encode("hevc-no-b.mp4", ["-an", "-c:v", "libx265", "-x265-params",
                                 "log-level=error:pools=1:frame-threads=1:bframes=0", "-tag:v", "hvc1"])
        encode("h264.flv", ["-c:v", "libx264", "-c:a", "aac"], audio=True)

        for source, output in [("hevc.mp4", "legacy-hevc.flv"), ("hevc-no-b.mp4", "legacy-hevc-no-b.flv")]:
            checked([self.ffmpeg, "-v", "error", "-y", "-i", str(self.work / source), "-c", "copy",
                     "-f", "flv", str(self.work / output)], self.project_env)
        enhanced_hevc(self.work / "legacy-hevc.flv", self.work / "enhanced-hevc.flv")
        enhanced_hevc(self.work / "legacy-hevc-no-b.flv", self.work / "enhanced-hevc-x.flv", coded_x=True)

        for codec, name, options in [
            ("libaom-av1", "av1", ["-cpu-used", "8"]),
            ("libvpx-vp9", "vp9", ["-deadline", "realtime", "-cpu-used", "8"]),
        ]:
            encode(name + ".mp4", ["-an", "-c:v", codec, "-threads", "2", *options])
            checked([self.ffmpeg, "-v", "error", "-y", "-i", str(self.work / (name + ".mp4")),
                     "-c:v", "copy", "-an", "-f", "ivf", str(self.work / (name + ".ivf"))], self.base_env)
            config = mp4_configuration(self.work / (name + ".mp4"), b"av1C" if name == "av1" else b"vpcC")
            if name == "av1":
                assert config and config[0] == 0x81
            ivf_flv(self.work / (name + ".ivf"), self.work / ("enhanced-" + name + ".flv"),
                    b"av01" if name == "av1" else b"vp09", config)

        for channels in [1, 2]:
            checked([self.ffmpeg, "-v", "error", "-y", "-f", "lavfi", "-i", "sine=sample_rate=48000",
                     "-t", "1", "-ac", str(channels), "-c:a", "libopus", "-f", "flv",
                     str(self.work / f"legacy-opus-{channels}.flv")], self.project_env)


    def decoded(self, name):
        output = checked([self.ffmpeg, "-v", "error", "-xerror", "-i", str(self.work / name),
                          "-f", "framemd5", "-"], self.project_env)
        frames = [line.strip() for line in output.splitlines() if line and not line.startswith("#")]
        assert frames, name
        return frames

    def probe(self, name):
        return json.loads(checked([self.ffprobe, "-v", "error", "-show_streams", "-show_packets",
                                   "-of", "json", str(self.work / name)], self.project_env))

    def verify_media(self):
        for name, codec in [("legacy-hevc.flv", "hevc"), ("enhanced-hevc.flv", "hevc"),
                            ("enhanced-hevc-x.flv", "hevc"), ("enhanced-av1.flv", "av1"),
                            ("enhanced-vp9.flv", "vp9"), ("h264.flv", "h264"), ("hevc.mp4", "hevc")]:
            info = self.probe(name)
            video = [stream for stream in info["streams"] if stream["codec_type"] == "video"]
            assert len(video) == 1 and video[0]["codec_name"] == codec, name
            assert video[0]["width"] == 96 and video[0]["height"] == 64, name
            assert any("K" in packet["flags"] for packet in info["packets"] if packet["stream_index"] == video[0]["index"]), name
            frames = self.decoded(name)
            assert sum(line.startswith("0,") for line in frames) == 12, name
            print(f"PASS {name}: {codec}, 12 decoded frames")
        assert self.decoded("legacy-hevc.flv") == self.decoded("enhanced-hevc.flv"), "HEVC 画面/时间戳不一致"
        assert self.decoded("legacy-hevc-no-b.flv") == self.decoded("enhanced-hevc-x.flv"), "CodedFramesX 时间戳不一致"
        header, tags = read_flv(self.work / "enhanced-av1.flv")
        stripped = []
        removed = 0
        for kind, timestamp, payload in tags:
            if kind == 9 and payload[0] & 15 == 1:
                frame, count = without_av1_sequence(payload[5:])
                payload = payload[:5] + frame
                removed += count
            stripped.append((kind, timestamp, payload))
        assert removed > 0
        write_flv(self.work / "enhanced-av1-config-only.flv", header, stripped)
        assert self.decoded("enhanced-av1-config-only.flv") == self.decoded("enhanced-av1.flv")
        print("PASS AV1 sequence header only in av1C configuration")
        for channels in [1, 2]:
            name = f"legacy-opus-{channels}.flv"
            info = self.probe(name)["streams"][0]
            assert info["codec_name"] == "opus" and info["channels"] == channels and info["sample_rate"] == "48000"
            self.decoded(name)
            print(f"PASS {name}: Opus {channels} channel(s)")


    def verify_bad_tags(self):
        # 重复配置、未知 FourCC 和结束包不应生成第二条虚假视频轨。
        header, tags = read_flv(self.work / "enhanced-hevc-x.flv")
        sequence = next(tag for tag in tags if tag[0] == 9 and tag[2][0] & 15 == 0)
        augmented = [sequence, (9, 0, b"\x90zzzzunknown"), *tags, (9, 1000, b"\x92hvc1")]
        write_flv(self.work / "enhanced-extra-tags.flv", header, augmented)
        assert self.decoded("enhanced-extra-tags.flv") == self.decoded("enhanced-hevc-x.flv")
        assert len(self.probe("enhanced-extra-tags.flv")["streams"]) == 1
        print("PASS extra sequence header, unknown FourCC and end tag")
        for payload in [b"\x91hv", b"\x91hvc1\x00\x00"]:
            write_flv(self.work / "truncated.flv", header, [sequence, (9, 1, payload)])
            result = invoke([self.ffmpeg, "-v", "error", "-xerror", "-i", str(self.work / "truncated.flv"),
                             "-f", "framemd5", "-"], self.project_env)
            assert result.returncode != 0 or not any(line.startswith("0,") for line in result.stdout.splitlines())
        print("PASS truncated Enhanced FLV headers")


    def verify_capabilities(self):
        # 对照原 Homebrew 库验证容器和输入协议没有因重编而减少。
        for option in ["-demuxers", "-protocols"]:
            def names(env):
                output = checked([self.ffprobe, "-v", "quiet", option], env)
                if option == "-protocols":
                    return {line.strip() for line in output.splitlines() if line.startswith("  ")}
                return {match.group(1) for line in output.splitlines()
                        if (match := re.match(r"\s+D\s+(\S+)", line))}
            missing = names(self.base_env) - names(self.project_env)
            assert not missing, f"能力减少 {option}: {sorted(missing)}"
        print("PASS demuxer/protocol capability regression")
        print("All FFmpeg FLV compatibility checks passed.")

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ffmpeg-prefix", type=Path)
    parser.add_argument("--work-dir", type=Path, default=ROOT / "build" / "ffmpeg-flv-tests")
    args = parser.parse_args()
    prefix = args.ffmpeg_prefix
    if prefix is None:
        env = dict(os.environ, HOMEBREW_NO_AUTO_UPDATE="1")
        prefix = Path(checked(["brew", "--prefix"], env).strip()) / "opt" / "ffmpeg@4"
    suite = FixtureSuite(prefix, args.work_dir)
    suite.generate()
    suite.verify_media()
    suite.verify_bad_tags()
    suite.verify_capabilities()



if __name__ == "__main__":
    main()
