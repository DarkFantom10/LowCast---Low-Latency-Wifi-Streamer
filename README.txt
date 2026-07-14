LowCast — minimal-latency system audio streamer
=============================================================

IF DISCOVERY EVER FAILS
Open Command Prompt in the exe's folder and run:   LowCast.exe probe
It prints every interface it searches on, every SSDP response, each handshake
step, and any UPnP error code the renderer returns; it also writes the same
output to lowcast-probe.log next to the exe, plus lowcast-desc.xml containing
the raw UPnP description of every device found (send both if asking for help) — the exact line where things stop tells you the cause.
Most common causes: Windows Firewall not allowed on Private networks, or
the headphones sitting on the router's Guest/IoT network (isolated subnet).

SETUP (once)
1. Put the appropriate device (e.g. HE1000 Wifi headset) in WiFi mode, connected to the same network as your PC.
   Strongly recommended: connect the headphones to the 5 GHz band of your
   router.
2. Run LowCast.exe. Windows Firewall will ask for permission the first time —
   ALLOW on Private networks (it needs to accept the headphone's HTTP pull and
   send SSDP discovery packets).

USE (every time — 2 clicks)
1. Your headphones appear as a button (e.g. "> START — HE1000 WiFi").
   Click it. It turns into "STOP" while streaming. That's it.
2. All system audio on the selected output is captured (games, Discord,
   browser — everything). No separate audio input is needed: the "Capture
   source" dropdown picks WHICH Windows output device to mirror; leave it on
   "Default output device" unless you route game audio somewhere specific.

CONTROLS
- Capture source ....... which Windows output's audio gets streamed (loopback)
- Audio type ........... WAV and LPCM are BOTH uncompressed PCM with identical
                         latency; WAV just wraps it in a 44-byte header some
                         firmwares require. If a renderer rejects the selected
                         type (UPnP error in the log), LowCast now retries as
                         WAV automatically and updates the dropdown.
- Rescan ............... re-run renderer discovery (3 s)
- Rate ................. THE latency lever. Firmware buffers a fixed number of
                         BYTES, so doubling/quadrupling the byte rate empties
                         that buffer in 1/2 / 1/4 the time. NOTE: higher rates also shrink the renderer's
                         jitter absorption by the same factor — 4x quarters
                         latency but also quarters dropout headroom. 2x is
                         the balanced choice for DLNA; for gaming prefer the
                         AirPlay path, which has loss repair built in. Restart the stream
                         (START) after changing it. Your HE1000 WiFi officially
                         supports PCM to 768 kHz, so 192 kHz is safely in-spec.
- Latency beep test .... injects a 1 kHz tick into the stream every 2 seconds
                         and flashes the yellow bar at the exact same instant.
                         The gap you perceive between FLASH and HEARD TICK is
                         your true end-to-end latency. Turn off when done.
- Stats panel .......... live sender-side latency (should read ~3-6 ms) and
                         connection info, updated 4x per second.

AIRPLAY (REALTIME) — USE THIS FOR GAMING
Devices that announce AirPlay (like the HE1000 WiFi) get a second button:
"START — (lightning) AirPlay: <name>". Unlike DLNA, AirPlay is a realtime
protocol where the SENDER controls the playback delay. Pick the delay with
the "AirPlay buffer" dropdown (50/75/100/150/250/350 ms) BEFORE pressing
START. Step down until you hear dropouts, then back off one notch. If a
very low value sounds no different from the one above it, the receiver
firmware has hit its internal floor and is clamping - that is your minimum.
AirPlay packet-loss repair: lost WiFi packets are automatically
retransmitted on the receiver's request (RTP resend protocol), so brief
radio hiccups no longer cause audible cutouts.
If AirPlay START fails with "ANNOUNCE rejected", the receiver demands
encrypted AirPlay — report it and encryption support can be added.
Audio is lossless 44.1 kHz/16-bit ALAC-wrapped PCM (CD quality) — the
AirPlay v1 protocol maximum, far above any Bluetooth codec.

PROTOCOLS
LowCast speaks both control protocols and picks per device, like swyh-rs:
OpenHome Playlist when the device exposes it (many streamer firmwares stub
their AVTransport service and only really work via OpenHome), otherwise
classic UPnP AVTransport. The log shows which one is used ([upnp/oh] vs
[upnp/av]).

WHY THIS IS FASTER THAN swyh-rs / TuneBlade / etc.
- No FLAC/MP3 encoding (zero codec delay), raw LPCM only.
- Zero sender-side buffering (swyh-rs ships with an "initial buffering" delay;
  here audio hits the socket ~3 ms after capture, TCP_NODELAY, tiny chunks).
- Continuous silence keep-alive so the headphone never stalls and re-buffers.