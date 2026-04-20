### 生成带声音的视频
```bash
ffmpeg -f lavfi -i color=c=black:s=1920x1080:d=7 -f lavfi -i "sine=frequency=1000:duration=5" -af "volume='if(between(mod(t,1),0,0.4),1,0)':eval=frame" -c:v libx264 -c:a aac -shortest -y temp_video.mp4
```
命令解析：
- sine=frequency=1000:duration=5：生成一个连续的 5 秒长的 1000Hz 声音。
- -af "volume='if(between(mod(t,1),0,0.4),1,0)':eval=frame"：
这是一个数学表达式。
mod(t,1)：将时间每 1 秒循环一次。
between(..., 0, 0.4)：如果循环时间在 0 到 0.4 秒之间。
1, 0：如果是，音量设为 1（有声音）；否则音量设为 0（静音）。

### 封装字幕流
```bash
ffmpeg -i temp_video.mp4 -i countdown.srt -c:v copy -c:a copy -c:s mov_text -map 0:v -map 0:a -map 1:s -y test_Subtitles.mp4
```