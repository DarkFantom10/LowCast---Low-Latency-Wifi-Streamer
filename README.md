# LowCast

LowCast is a native Windows application that captures system audio with WASAPI loopback and streams stereo PCM over a local network. It supports DLNA/UPnP MediaRenderer devices and unencrypted AirPlay v1 (RAOP) receivers. It was built for the HiFiMAN HE1000 WiFi path, but the DLNA path can work with other standards-compliant renderers.

## Requirements

- 64-bit Windows 10 or Windows 11
- A receiver on the same IPv4 LAN
- Visual Studio 2022 Build Tools with MSVC `14.44.35207`
- Windows SDK `10.0.26100.0`

The tool locations are set near the top of `build.ps1`; update them if your Visual Studio or SDK installation differs.

## Firmware
Hifiman firmware was provided by their customer service team for this project. Airplay v1 functionality is required for LowCast to use low latency streaming. Airplay v2 has built in encryption and thus was determined to not meet my latency requirements.

## Build

From PowerShell in the repository root:

```powershell
.\build.ps1
```

This performs a serial x64 release build at below-normal process priority and writes `LowCast-Dark.exe`. Windows PowerShell 5.1 and PowerShell 7 are supported.

The deterministic regression executable is built and run separately:

```powershell
.\build.ps1 -Tests
.\build\regression.exe
```

## Use

1. Run `LowCast-Dark.exe`.
2. Choose the Windows output device to capture, plus the DLNA format and rate.
3. Select **Rescan** if the receiver is not listed.
4. Select **START** beside a DLNA or AirPlay receiver. Use **STOP** before changing receivers or transport.

The vertical volume control sets the AirPlay receiver volume. Leaving it at `auto` preserves receiver control until the slider is moved. The latency beep can help compare the visible flash with the sound arrival time. `LowCast-Dark.exe uipreview` opens a local mock UI without starting audio, discovery, networking, battery polling, logging, or settings writes.

## Latency

DLNA is the compatibility and fidelity path; renderer firmware commonly adds hundreds of milliseconds of buffering. The 2x and 4x DLNA rate choices can reduce that firmware buffer at the cost of bandwidth.

AirPlay is the lower-latency path. The default sender buffer is 150 ms, while 250 ms is the safer general starting point. Values below 250 ms have less loss-repair margin, and values below 100 ms are clamped to 100 ms. Receiver firmware and WiFi conditions determine the usable floor.

## Battery warnings

When Windows exposes a Bluetooth battery property, LowCast shows the most recent value in the main window. It opens an application alert at 40%, 20%, and 10%. The alert stays open until **Acknowledge** is pressed; closing it with Alt+F4 does not dismiss it. If the battery crosses a more urgent threshold while the alert is open, the same window updates. Alerts re-arm for a new charge cycle at 80%.

The main label shows `LOW` at 40% or below, `CRIT` at 20% or below, and `URGENT` at 10% or below. A temporary missing battery property retains the last valid reading.

The value is Bluetooth-sourced and may be stale while the headset is in WiFi mode. A dash means no battery value has been observed.

## Network and firewall

Allow LowCast on **Private** Windows networks. Discovery uses SSDP multicast on UDP 1900 and mDNS on UDP 5353. DLNA receivers fetch the stream from TCP ports 16600-16619. AirPlay uses the receiver's advertised RTSP and UDP ports. VPNs, guest-network isolation, multicast filtering, or a firewall can prevent discovery or playback.

LowCast has no authentication or encryption layer for its local HTTP/RAOP traffic; use it on a trusted LAN.

## Known limitations

- AirPlay 2 pairing, FairPlay, and HomeKit encryption are not implemented.
- Only one AirPlay session is active at a time, and a receiver cannot use its DLNA and AirPlay outputs simultaneously.
- End-to-end latency, reconnect behavior, and battery freshness depend on receiver firmware, drivers, and network conditions.
- The application is Windows/x64 only.

## Validation

The repository includes noninteractive checks for the embedded icon, dark-control wiring, offscreen volume rendering, battery alert state and hidden-window acknowledgment, bounded log behavior, AirPlay button reset, and vertical volume dragging. The automated checks do not replace real receiver, audio-device, WiFi-loss, sleep/resume, or headset-battery testing.
