# wmt-play

A fullscreen video player for the **WonderMedia WM8505**, specifically targeting the early-2010s ARM netbooks built around this SoC.

It plays MJPEG AVI files with 16-bit PCM audio. It sits on top of the in-kernel `wmt-jdec` and `wmt-scl` V4L2 drivers, and leases the display from X for the length of playback.

## Capabilities

* MJPEG decoding on the JPEG Decoder (JDEC).
* Scaling and NV12 to RGB color conversion on the Scaler (SCL), keeping the aspect ratio.
* Zero-copy dma-buf handoff from the decoder to the scaler to the display.
* Vblank-timed page flips on a CRTC leased via RandR 1.6.
* A/V sync to the ALSA playback clock, dropping late frames.
* Pause, seeking, frame stepping, and looping.

The CPU demuxes the file, copies each compressed frame into the decoder, and writes the audio.

## Usage

```sh
wmt-play [-s SEC] [-l] [-a] [-v] FILE
```

`-s` starts SEC seconds into the file, `-l` loops it, `-a` disables audio, and `-v` prints per-frame stats.

| Keys           | Action                      |
| -------------- | --------------------------- |
| Space          | Pause or resume             |
| Left / Right   | Seek 5 seconds              |
| Down / Up      | Seek 30 seconds             |
| Comma / Period | Step one frame while paused |
| Q / Escape     | Quit                        |

## Encoding

Convert videos on another computer with `ffmpeg` installed (or on the netbook itself if you're brave enough), using `wmt-encode.sh`:

```sh
./wmt-encode.sh input.mp4                 # 800x480 panels, the default
RES=1024x600 ./wmt-encode.sh input.mp4    # 1024x600 panels
```

The output goes beside the source as `input.avi`, unless a second argument names it. The script scales larger video down to fit the panel, converts the color matrix to BT.601 for the hardware color converters, and writes the audio as 48 kHz 16-bit stereo PCM.

## Building

Requires: `libavformat-dev libasound2-dev libdrm-dev libxcb1-dev libxcb-randr0-dev libxcb-screensaver0-dev libxcb-keysyms1-dev x11proto-dev pkgconf`

```sh
make
sudo make install
```

`make FFMPEG=<dir>` builds an AVI-only libavformat from the FFmpeg source tree in `<dir>` and links it statically, in place of `libavformat-dev`.

## License

MIT - see [LICENSE.md](LICENSE.md).
