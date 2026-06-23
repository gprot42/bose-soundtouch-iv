# Audio Playback on the Wave SoundTouch IV (and a path to native FLAC)

How the Wave SoundTouch IV actually decodes and plays audio, reconstructed from
the extracted firmware (build for the **lisa** platform / **nelson** product)
and verified against a live unit with root access.

> Codename note: the running unit reports `/proc/variant = lisa` and launches
> `*-nelson` configs. (Some configs in the image are for other products like
> `triode`; those are not what this device runs.) Verified on firmware
> **27.00.06**.

---

## TL;DR

- The program that plays MP3 (and AAC/ALAC/WMA/FLAC/OGG/WAV) is **`APServer`**
  — the *Audio Path server*. It decodes in software and writes PCM to the
  custom ALSA device `/dev/snd/snd_shelby`.
- `UpnpSource` is only the DLNA front-end; it hands a URL to the audio path and
  **does not decode**.
- **FLAC decode already exists natively in APServer** (`BOSE_AD_FLAC`, Bose's
  ARM-optimized `vflac`), and `UpnpSource` already advertises `audio/flac`.
  The reason we currently transcode FLAC→MP3 is almost certainly a
  **control-point bug**, not a missing decoder. See
  [Native FLAC](#native-flac-theory).

---

## What plays the audio: `APServer`

`/opt/Bose/APServer` (the *Audio Path server*, AKA AudioServer). On the live
device it is the only process holding `/dev/snd/snd_shelby` open. It is launched
at elevated priority and auto-reboots on crash — it is the realtime audio
engine.

It links:
- `libcurl` — fetch the stream over HTTP
- `libasound` — write PCM to ALSA (`snd_shelby2` kernel driver)
- `libSoundTouchInternal` / `libProtobufMessagingIPC` — control plane (IPC)

It exposes (over IPC): `AudioServerMsgSetURL`, `AudioServerMsgTransportControl`,
plus amp/zone/group control for multi-room.

### Built-in software decoders

The decoder dispatch enum in the binary:

```
BOSE_AD_MP3  BOSE_AD_AAC  BOSE_AD_HEAAC  BOSE_AD_ALAC  BOSE_AD_FLAC
BOSE_AD_WMA  BOSE_AD_WMAL BOSE_AD_VORBIS BOSE_AD_PCM    BOSE_AD_SBC
```

There is **no** GStreamer/ffmpeg/libmad/mpg123 anywhere on the rootfs — all
decoding is Bose's own code statically built into `APServer`
(e.g. `APAudioSubParser_FLAC` / `APAudioSubDecoder_FLAC`, the `vflac` codec).

Decoder selection is driven by the stream's **HTTP `Content-Type`** header
(strings: `FAILED TO GET HTTP CONTENT TYPE`, `Content type ...`). This detail
is the crux of the FLAC story below.

---

## Data flow for a streamed track

```
DLNA control point  ── SetAVTransportURI(url, DIDL res protocolInfo) ──►
      │
      ▼
UpnpSource  ── IPC ──►  BoseApp ── IPC ──►  APServer
(UPnP renderer,                 (orchestrator)   │  GET url (libcurl)
 advertises sink formats,                        │  pick decoder by Content-Type
 no decode)                                      │  decode → PCM
                                                 │  ASRC resample → 46875 Hz
                                                 ▼
                                      /dev/snd/snd_shelby   (snd_shelby2)
                                                 │  I2S PCM
                                                 ▼
                                       SM2 amp/DSP module ◄── ABLServer (/dev/abl0-2 control)
                                                 │  (DSP image: lisa_sm2_normal.cfg)
                                                 ▼
                                             speakers
```

Notes from `APConfig-nelson.xml`:
- The SM2 DSP runs at a fixed `outputSampleRate="46875"` Hz.
- APServer contains an **ASRC** (asynchronous sample-rate converter,
  `asrcConstants`, "APAuxSrc SAMPLE RATE CHANGED TO %d"). **Any input sample
  rate is resampled to 46875 Hz**, so input rate is not a hard gate.
- Output goes through `VOLUME_FORWARD_ABL` and `ABLMute` — volume/mute are
  forwarded to the SM2 over ABL.

---

## How it is set up (boot)

1. `/etc/init.d/SoundTouch` (linked as `rc5.d/S99SoundTouch`) starts
   **`shepherdd`**, the SoundTouch process supervisor.
2. `shepherdd` reads `/proc/variant` (= `lisa`) and the product (`nelson`),
   then launches the daemons listed in the matching `Shepherd-*.xml`:
   - `Shepherd-core.xml` — avahi, Bluetooth, NetManager, **CLIServer**, …
   - `Shepherd-nelson.xml` — **APServer** (`-c APConfig-nelson.xml`, `nice="-6"`,
     `recovery="reboot"`), **UpnpSource** (`-c UpnpSource-nelson.xml`),
     WebServer, **BoseApp** (`-c BoseApp-nelson.xml`)
   - `Shepherd-product.xml` — **ABLServer**
   - `Shepherd-hsp.xml` — **scmmond** (SCM monitor: mute / low-power)
   - `Shepherd-noncore.xml` — STSCertified, IoT, TPDA (voice)
3. ALSA hardware is brought up earlier by `rc5.d/S90shelby_sound`
   (TI **AIC3256** codec + STM32 amp over I2S; driver `snd_shelby2`).

So to "play an MP3", the chain is: a control point (app / DLNA / Spotify /
the repo's `send-to-bose.py`) gives BoseApp a URL → BoseApp hands it to
`APServer` → `APServer` fetches + decodes + resamples → SM2 amp → speakers.

---

## Native FLAC <a name="native-flac-theory"></a>

### Current state

- `send-to-bose.py` lists FLAC under `TRANSCODE_EXTENSIONS` and **transcodes
  FLAC→MP3** at serve time (lossy), with a comment "Wave IV won't decode FLAC".
- But the firmware shows: `BOSE_AD_FLAC` + a full FLAC parser/decoder in
  `APServer`, **and** `UpnpSource` already advertises `audio/flac` /
  `audio/x-flac` in its DLNA sink `ProtocolInfo`.

So the decoder is present and the renderer claims FLAC support. Native FLAC is
very likely a matter of *triggering the existing decoder correctly*, not adding
one.

### Most likely root cause (Hypothesis A — control point)

Decoder selection keys off the HTTP `Content-Type`. In `send-to-bose.py` the
local HTTP server sets the type from Python's `mimetypes.guess_type()`:

```381:384:dlna-sender/send-to-bose.py
        mime, _ = mimetypes.guess_type(file_path)
        if not mime:
            mime = "application/octet-stream"
        return size, mime
```

Python's `mimetypes` has **no built-in mapping for `.flac`**, so a FLAC file is
served as `application/octet-stream`. Combined with a DIDL `res@protocolInfo`
that doesn't declare FLAC, `APServer` can't map the stream to `BOSE_AD_FLAC`
and you get silence — which looks like "the Wave can't decode FLAC", prompting
the transcode workaround.

**Proposed fix (no firmware change):**
1. Register the MIME type so the file server emits the right header:
   ```python
   mimetypes.add_type("audio/x-flac", ".flac")   # or "audio/flac"
   ```
2. Set the DLNA `res@protocolInfo` in the DIDL-Lite metadata to match, e.g.
   `http-get:*:audio/x-flac:*` (or with DLNA flags
   `DLNA.ORG_OP=01;DLNA.ORG_FLAGS=...`). Generic control points that send
   `audio/mpeg` for everything will also defeat the decoder, so the metadata
   must reflect FLAC.
3. Drop `.flac` from `TRANSCODE_EXTENSIONS` (or add a `--native-flac` flag) so
   the file is served as-is.
4. Ensure the HTTP server honors **range requests** for `.flac` (seek/scan).
   The current `_send_file` advertises `Accept-Ranges: bytes`, which is good;
   confirm FLAC seeking works, since FLAC needs the `STREAMINFO`/header.

This is the highest-probability path and is purely a control-point change.

### Secondary constraint (Hypothesis B — hi-res limits)

The ASRC resamples any rate to 46875 Hz, so **44.1/48 kHz, 16-bit FLAC should
just work** once the Content-Type is correct. Risk areas to test:
- **24-bit / 88.2–192 kHz "hi-res" FLAC**: the `vflac` decoder and the fixed
  buffer/IPC path may reject or stutter on large frames (`Frame format not
  supported`, buffer-overrun guards exist). If so, the right move is to
  **downsample/redither to 16-bit/48 kHz FLAC** (still lossless-class, stays
  FLAC) rather than transcode to MP3 — preserving quality while fitting the
  path.
- **Compression level / block size**: standard `flac` encoder defaults are
  safe; exotic block sizes are worth a test.

### If the decoder were actually gated (Hypothesis C — unlikely)

If testing shows `APServer` refuses FLAC even with correct Content-Type, the
next places to look are a per-product codec allow-list (none was found in
`APConfig-nelson.xml`, which only configures the DSP/output path, not codecs)
or a license/capability flag in `BoseApp`. Given the decoder and MIME
advertisement are both present, this is improbable.

### Verification plan

1. **Confirm the sink advertises FLAC on the live device** — query the
   MediaRenderer `ConnectionManager::GetProtocolInfo` and look for
   `audio/x-flac` in the `Sink` list.
2. **Direct test** — serve a 16-bit/44.1 kHz FLAC with `Content-Type:
   audio/x-flac` and a matching `res@protocolInfo`, then `SetAVTransportURI` +
   `Play`. Use `send-to-bose.py --debug` to see transport/stream logs.
3. **On-device introspection** — `APServer` exposes an `audiopath capture`
   CLI (`audiopath capture <stop|src|bdsp_in|dumper> <file>`) and logs the
   chosen decoder; watch for the FLAC parser path vs a decode error while a
   FLAC stream plays.
4. **Hi-res matrix** — repeat with 16/48, 24/48, 24/96 to find the ceiling, and
   set the downsample target accordingly.

### Outcome

Native, lossless FLAC playback on the Wave IV is most likely achievable
**without modifying firmware** — by serving FLAC with the correct MIME /
DLNA metadata and (for hi-res files) downsampling to a supported FLAC profile
instead of transcoding to MP3.
