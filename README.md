# LowCast

A minimal-latency system audio streamer for Windows. LowCast captures whatever Windows is playing and streams it over WiFi to DLNA/UPnP renderers and AirPlay receivers, with a design that treats every millisecond of sender-side buffering as a bug.

The entire application is a single C++ file with no external dependencies beyond the Windows SDK. No codec libraries, no frameworks, no installer.

## Features

- **System audio capture** via WASAPI shared-mode loopback, event-driven, with an IAudioClient3 "period driver" that holds the audio engine at its minimum mixing period (typically ~3 ms instead of the default 10 ms)
- **DLNA/UPnP streaming** as uncompressed LPCM (L16/L24, big-endian) or WAV (16/24-bit), discovered via SSDP, controlled via AVTransport SOAP
- **AirPlay streaming** to RAOP-compatible receivers using verbatim (uncompressed) ALAC framing at 44.1 kHz/16-bit, with RTP retransmission, NTP-style timing replies, and periodic sync packets
- **High quality rate conversion**: a 96-tap Kaiser-windowed polyphase sinc resampler converts the capture rate (usually 48 kHz) to the 44.1 kHz AirPlay clock. Passband is flat to roughly 19.8 kHz and alias rejection measures better than -95 dBc, below the 16-bit noise floor
- **TPDF dither** on every float to 16-bit quantization
- **Clock-slaved transmission**: packets leave the moment one packet of audio exists. A proportional depth servo (authority +/-400 ppm) pins the sender pipe about 2 ms above a single packet, so sender-side buffering cannot drift or accumulate
- **Follows the default output device**: switching the Windows output device mid-stream is handled seamlessly
- **Silence keepalive**: wall-clock silence is injected when nothing is playing so renderers never stall and re-buffer
- **Built-in latency test**: an optional 1 kHz metronome tick with a synchronized on-screen flash lets you measure true end-to-end latency by ear
- **Session resilience**: lost AirPlay sessions reconnect automatically until stopped, and live sessions survive a failed device rescan

## Requirements

- Windows 10 or 11, x64
- A network where multicast works (SSDP uses 239.255.255.250:1900, mDNS uses 224.0.0.251:5353)

## Building

### MinGW-w64 (including cross-compilation from Linux)

```
x86_64-w64-mingw32-g++ -O2 -municode -mwindows -std=c++17 \
  -o LowCast.exe LowCast-source.cpp \
  -lws2_32 -liphlpapi -lole32 -lcomctl32 -lwinmm \
  -static -static-libgcc -static-libstdc++
```

The static flags produce a self-contained binary with no runtime DLL dependencies.

### MSVC

Open a Developer Command Prompt and run:

```
cl /O2 /std:c++17 /DUNICODE /D_UNICODE LowCast-source.cpp ^
  /link /SUBSYSTEM:WINDOWS ws2_32.lib iphlpapi.lib ole32.lib ^
  comctl32.lib winmm.lib user32.lib gdi32.lib
```

## Usage

1. Launch the executable. Allow it through Windows Firewall on **private networks** when prompted. This matters: AirPlay discovery relies on unicast mDNS replies that a blocking firewall will silently eat.
2. Discovered DLNA renderers and AirPlay receivers appear as buttons. Click one to start streaming, click again to stop.
3. Pick a capture device, or leave it on default to follow whatever Windows is using.
4. Choose a stream format for DLNA (LPCM/WAV, 16/24-bit) and a buffer size for AirPlay (25 to 350 ms).
5. Use **Rescan** to re-run discovery at any time.

The window log reports everything: capture format, engine period, discovery results, RTSP handshake details, retransmission counts, and reconnect attempts.

### Choosing the AirPlay buffer

The buffer setting is the playback offset the receiver is asked to schedule, not sender-side buffering. Low values (25 to 75 ms) work on receivers with good clocks and strong WiFi. If a receiver's RECORD response advertises a minimum latency above your setting, the log will warn you; some firmwares play silence rather than clamping, so raise the setting if you hear nothing.

### Latency measurement

Enable the metronome to mix a 1 kHz tick into the stream every two seconds, synchronized with a visual flash in the UI. The gap between flash and sound is your end-to-end latency. A local beep option provides a reference click on the PC itself for A/B comparison.

## Quality notes

The pipeline aims for transparency rather than nominal losslessness, and it is honest about what shared-mode capture can and cannot do:

- WASAPI loopback captures the output of the Windows mixer. Windows has already mixed and rate-converted every application to the device's shared format, so bit-exactness relative to a source file is impossible for any loopback capture on any software. Everything downstream of the mixer, however, is either bit-transparent (LPCM, verbatim ALAC) or measurably below the 16-bit noise floor (resampler, dither).
- For the cleanest AirPlay chain, set the Windows output device format to 44.1 kHz. The resampler then runs at unity ratio and only applies the servo's parts-per-million corrections.
- The optional upsample setting on the DLNA path trades fidelity for renderer latency and uses simple interpolation. Leave it at 1x when quality matters.

## Headless test mode

A command-line mode exercises the full RAOP stack against a receiver without capturing audio:

```
LowCast.exe raoptest <host> <port> <latency_ms> <seconds> [drift_ppm] [meas_err_ppm]
```

It streams a 440 Hz tone, optionally simulating soundcard clock drift and deliberate rate-measurement error to stress the depth servo, and logs pipe depth over time to `lowcast-raop.log`. Append `gaps` to simulate periodic 150 ms dropouts.

## Troubleshooting

- **No AirPlay receivers found**: check the log for `[mdns] port 5353 unavailable`, which means another service (often Bonjour or iTunes) holds the port exclusively; discovery falls back to a legacy socket in that case. Also verify the firewall rule covers private networks.
- **Receivers found on first scan but not rescan**: fixed in the current source. Discovery now queries on every network interface and accumulates split mDNS record refreshes across packets.
- **Audible dropouts on WiFi**: raise the AirPlay buffer. Retransmission and reconnect counters in the log help distinguish packet loss from clock problems.
- **Renderer takes seconds to start**: that is the renderer's own pre-buffer. The sender adds roughly 10 to 15 ms end to end; everything else lives on the receiving side.

## License

MIT
