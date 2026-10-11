# Eye-tracked foveated streaming to the Steam Frame

Status: research and plan only. No product code yet.
Date: 2026-10-11. Revised the same day with facts from the user, who owns a
Steam Frame.

## Goal

Stream from a vibeshine host to a Valve Steam Frame. The encoder spends more
bits where the wearer is looking and fewer bits in the periphery. The headset's
eye tracker supplies the gaze point. This is how Valve's own Steam Link
streaming already works on the Frame. The aim is to get the same benefit in a
Moonlight-protocol stream.

## What the user runs today

These facts come from the user, not from public sources:

- The client is **Moonlight from Flathub** (`com.moonlight_stream.Moonlight`,
  installed through Discover). That is upstream `moonlight-qt`, the Linux
  build, running as a flat window under SteamOS on the Frame. It is not the
  Android APK and not the Lepton Moonlight XR fork.
- It decodes **HEVC**. H.264 also works.
- **PyroWave is not available.** It does not show as a codec option in the
  client, and the host does not offer it at pairing. Stock Moonlight has no
  PyroWave decoder; only Nonary's VRR fork of `moonlight-qt` has one
  (`docs/pyrowave-protocol.md`) [S14].

What public sources add:

- The Flathub package builds upstream `moonlight-qt`. The newest upstream
  release is v6.2.0 (2026-10-04) [S16]. The Flathub manifest pins no
  architecture list, so Flathub builds it for aarch64 as well as x86_64 [S17].
  The exact version on the Frame is whatever Discover installed. Check it in
  Moonlight's settings or with `flatpak info com.moonlight_stream.Moonlight`.
- Nonary's fork publishes x86_64 Linux builds only [S14]. A community build,
  `murmrs/moonlight-qt` v6.1.0-vrr18-handheld1, turns PyroWave on for aarch64.
  It ships as a Flatpak with the same app ID, so it replaces the Flathub
  Moonlight. Its PyroWave support on Adreno has had only limited testing
  [S15]. This matters only for the optional PyroWave phase at the end.
- Not confirmed: whether the Frame decodes HEVC in hardware or in software.
  Moonlight's statistics overlay names the decoder. Phase 0 records it.

## Summary

- **Gaze is available to third-party apps, but only to OpenXR apps.** Valve
  documents eye tracking on the Frame through the OpenXR extension
  `XR_EXT_eye_gaze_interaction` [S1]. The Unity path uses the same extension
  [S2]. The user's client is a flat Qt window with no OpenXR session, so it
  cannot read gaze as it stands. **Open hardware check, first in the list:**
  can anything running next to a flat Moonlight window on the Frame read gaze
  through `XR_EXT_eye_gaze_interaction`? See section 1.
- **Encoder priority follows what the user streams: HEVC on NVENC first.**
  Every phase targets NVENC HEVC. NVENC's HEVC path takes a per-CTB QP delta
  map (`qpDeltaMap`). NVENC's emphasis-map mode is H.264-only, so it is not on
  the main path. PyroWave moves to an optional last phase. That phase starts by
  adding PyroWave decode to the Frame client, and runs only if the HEVC
  results show it is worth it.
- **The host side is mostly plumbing.** vibeshine has no ROI, QP-map or gaze
  code today. The vendored NVENC headers (SDK 13.0) already expose
  `qpDeltaMap` and `qpMapMode`.
- **The smallest proof needs no client change.** A fixed-centre QP delta map
  on the standalone NVENC HEVC path is driven by a config option. The user's
  Flathub Moonlight shows the result as it is. Phase 0 below covers it.
- **The gaze transport has two routes.** One is a new control packet in
  `moonlight-common-c`, which needs a `moonlight-qt` change and a custom
  Flatpak build. The other is a small side channel from a gaze helper on the
  Frame straight to the host, which leaves Moonlight untouched. Section 2
  compares them.
- **Weigh this first.** Valve says Steam Link's foveated streaming applies to
  "all streamed Steam content" without developer work, flat games included
  [S8]. If the game is in Steam and Steam Link's quality is good enough, the
  user may not need this project at all. The case for vibeshine is non-Steam
  content, vibeshine's display and virtual-display management, or control
  over codec and bitrate.

## 1. How the Steam Frame exposes gaze

| Route | What it gives | Usable by us? | Source |
|---|---|---|---|
| OpenXR `XR_EXT_eye_gaze_interaction` on the headset | Gaze pose action (`/user/eyes_ext/input/gaze_ext/pose`) located with `xrLocateSpace` at a predicted time | **Yes, for an OpenXR app.** This is the supported route. The user's Moonlight is not an OpenXR app. | Valve custom-engine docs [S1]; Khronos spec [S3] |
| `XR_META_foveation_eye_tracked`, `XR_FB_foveation*` | Eye-tracked foveated *rendering* of the app's own swapchains | Not for gaze data | [S1] |
| Unity "Eye Gaze Interaction" feature | Wrapper over the same OpenXR extension | Only if the client is Unity-based | [S2] |
| Private shared memory `/dev/shm/eye-server.mmap` on the headset | Gaze plus eyelid openness at 90–136 Hz while streaming | **No.** Undocumented, may change with any SteamOS update, and the tool's author warns it may conflict with the Steam Subscriber Agreement. | frameeyeosc README [S4] |
| SteamVR on the PC (gaze to PC OpenXR apps while the Frame is tethered to SteamVR) | Gaze on the host | Only while the Frame is in SteamVR/Steam Link mode. In that mode Steam Link, not vibeshine, carries the video. Out of scope. | inferred from [S1][S8] |
| Steam Link's foveated-streaming internals | Not publicly documented beyond "eye tracking to optimise the streamed image to have the highest quality at the very centre of your view" | Nothing to reuse | [S8][S9] |

### Getting gaze for a flat Moonlight window

The Flathub client draws a normal window. SteamOS on the Frame places that
window on a virtual screen. No public document says how a flat app can read
gaze. These are the options, cheapest first. Each needs the hardware check
below before any work starts.

| Option | How it works | Cost | Unknowns |
|---|---|---|---|
| **G1. Separate gaze helper process** | A small native aarch64 OpenXR program runs next to Moonlight. It creates an OpenXR session, enables `XR_EXT_eye_gaze_interaction`, reads the gaze pose and sends it on. It submits no visible layers. | Small. Moonlight is unchanged. | Whether the Frame's OpenXR runtime allows a second session while a flat app is on screen, without taking over the display or hiding the Moonlight window. Whether it supports a headless or overlay session (`XR_MND_headless` exists only on Monado; `XR_EXTX_overlay` support is unknown). Whether a session that is not focused still gets gaze samples. |
| **G2. A SteamVR- or runtime-side API** | Read gaze from a system API on the headset instead of from an app session, for example an OpenVR or SteamVR input path, or a Monado-style IPC API if the Frame's runtime is built on one. | Small if it exists. | No public documentation found. Valve documents only the OpenXR extension [S1]. Do not use the private shared-memory file [S4]. |
| **G3. Make `moonlight-qt` an OpenXR app** | `moonlight-qt` opens its own OpenXR session and draws the decoded video on a quad layer instead of a window, then reads gaze itself. | Large. It needs a custom `moonlight-qt` build shipped as a Flatpak (or another package) that replaces the Flathub one. | Decoded-frame hand-off from FFmpeg or libplacebo to an OpenXR swapchain on the Frame's GPU. Valve lists "Linux Arm64" as a supported custom-engine target [S1]. |
| G4. Lepton Moonlight XR fork (fallback for gaze tests only) | The Android OpenXR fork already runs on the Frame [S5][S6]. | Small for a gaze experiment. | It still decodes software H.264 only, about 40 Mbps at 1440p (v0.3, 2026-10-11) [S5], so it cannot carry the user's HEVC stream. It is not a target client. It is still unverified that Lepton exposes `XR_EXT_eye_gaze_interaction` to an APK. |

G1 is the preferred first try. It keeps the user's working client, and it
fits the side-channel transport in section 2.

**Mapping gaze to the frame.** The host needs `(u, v)` in `[0,1]²` in video
frame coordinates. An OpenXR app (G3) can intersect the gaze ray with its own
screen quad. A helper (G1, G2) does not know where SteamOS placed the
Moonlight window. Until the window pose is known, the helper sends the gaze
direction relative to the head-forward direction. The host converts it to
`(u, v)` from a configured screen size in degrees, assuming a centred screen.
The user then calibrates by looking at the corners of a test pattern. This is
coarse but enough for a soft-edged fovea.

### Open hardware checks (do these first)

1. **Gaze next to a flat Moonlight window.** On the Frame, with Flathub
   Moonlight streaming, can a separate OpenXR program enable
   `XR_EXT_eye_gaze_interaction`? Does
   `XrSystemEyeGazeInteractionPropertiesEXT::supportsEyeGazeInteraction`
   report true? Does the gaze pose update while the Moonlight window stays
   visible? Is there a permission prompt, and what is the real sample rate?
   If the answer is no, try G2. Fall back to G3 only if G2 also fails.
2. **HEVC decoder path.** Hardware or software, and at what bitrate and
   resolution the decoder keeps up. Read it from Moonlight's statistics
   overlay.
3. **Window placement.** Can the virtual screen's size and position be read
   or fixed, so `(u, v)` mapping needs no manual calibration?

**Sending gaze with low latency.** The gaze source sends a small packet per
frame, or at the eye-tracker rate. It sends unreliably, so a stale sample is
dropped and never retransmitted. One open-source streaming client already
does this. The draft OXRSys Steam Frame client reads
`XR_EXT_eye_gaze_interaction` and sends a head-space gaze vector in its
tracking packet. Its server low-passes the gaze and steers the foveal centre,
gated on a client capability flag [S7]. That design is a good reference.

## 2. Client, transport and protocol

### Client

The target client is the one the user already runs: **Flathub Moonlight
(upstream `moonlight-qt`, aarch64) with HEVC.** Phases 0 and 1 need no client
change. Phase 2 adds a gaze source (G1, G2 or G3 above). Only G3 changes
Moonlight itself.

The earlier draft of this plan targeted the Lepton Moonlight XR fork. It is
now a fallback for gaze experiments only, because it decodes software H.264
only [S5].

### Transport options

| Route | Moonlight change? | Notes |
|---|---|---|
| **T1. Side channel from the gaze helper to the host** | None | The helper sends gaze datagrams to a new host UDP port. The host binds them to the running session. It must authenticate them: for example the helper pairs with the host like a client and uses the session's keys, or the host accepts only the streaming client's address while a session is up. The first is safer and is the target; the second is enough for a LAN test. Fits G1 and G2. |
| **T2. New control packet in `moonlight-common-c`** | Yes. `moonlight-common-c` plus `moonlight-qt`, shipped as a custom Flatpak. | Clean and authenticated through the existing control stream. The client needs gaze from somewhere, so this is natural for G3, or for G1 with local IPC from the helper into Moonlight. Details below. |

Start with T1. Move to T2 if the client becomes an OpenXR app (G3), or if a
custom Moonlight build is shipped anyway.

### Protocol change for T2 (host + `moonlight-common-c`)

This follows existing patterns:

- **Capability negotiation:**
  - Host advertises `LI_FF_GAZE_FOVEATION` (next free bit after `pen_touch=0x01`,
    `controller_touch=0x02`) in `a=x-ss-general.featureFlags`. This is emitted
    at [`src/rtsp.cpp:1640`](../../src/rtsp.cpp#L1640) from
    [`platf::get_capabilities()`](../../src/platform/common.h#L281).
  - Client advertises `ML_FF_GAZE` (next free bit after `HAPTICS_PCM 0x04`)
    in `x-ml-general.featureFlags`. This is
    `third-party/moonlight-common-c/src/SdpGenerator.c:271`, parsed by the host
    at [`src/rtsp.cpp:1864`](../../src/rtsp.cpp#L1864).
  - Optional ANNOUNCE attributes such as `x-ss-video[0].foveation*` carry
    screen angular size and preferred strength. This follows the
    `x-ss-video[0].pyrowave*` precedent at
    [`src/rtsp.cpp:1973`](../../src/rtsp.cpp#L1973).
- **New control packet** `IDX_GAZE` with a new unused type (e.g. `0x5504`).
  Added to `packetTypes[]` at [`src/stream.cpp:87`](../../src/stream.cpp#L87)
  and `packetTypesGen7Enc` in `moonlight-common-c/src/ControlStream.c:204`.
  - Payload, little-endian: `u16 version; u16 flags; float u; float v;
    float radius_deg_hint; u32 last_decoded_frame; u32 client_time_us`.
  - The client sends it with `sendMessageAndForget`
    (`ControlStream.c:847`) on a new unreliable, unsequenced channel. Gaze
    must never queue behind reliable traffic.
  - The host handler copies the RFI handler at
    [`src/stream.cpp:1475`](../../src/stream.cpp#L1475): parse, validate
    (finite, in range), raise a mail event.

T1 uses the same payload and the same host mail. Only the receive path
differs.

### Host mail (both transports)

- Add `MAIL(gaze)` next to `MAIL(idr)` and `MAIL(invalidate_ref_frames)`
  in the mail list at [`src/globals.h:46`–`58`](../../src/globals.h#L46-L58).
- Bind it like the other session events at
  [`src/stream.cpp:619`](../../src/stream.cpp#L619) and
  [`:3843`](../../src/stream.cpp#L3843).
- Drain it in the encode loop next to RFI at
  [`src/video.cpp:5139`](../../src/video.cpp#L5139). Take only the newest
  sample.

Why not an input packet: input packets (`SS_PEN`, `SS_CONTROLLER_MOTION` in
`Input.h`) are the precedent for high-rate floats, but the host processes
them on the input thread ([`src/input.cpp:461`](../../src/input.cpp#L461)).
A control packet or side-channel sample goes straight to the encode thread's
mail.

## 3. Where foveation goes in vibeshine

### Pipeline (read from source)

- **Capture:**
  - Windows: DXGI ([`display_vram.cpp:2434`](../../src/platform/windows/display_vram.cpp#L2434)),
    WGC ([`display_wgc.cpp:259`](../../src/platform/windows/display_wgc.cpp#L259)).
  - Linux: KMS, X11, wlroots, PipeWire portal, KWin and gamescope.
  - Capture threads: [`video.cpp:2931`](../../src/video.cpp#L2931) and
    [`:5850`](../../src/video.cpp#L5850).
- **Encode loop:** `encode_run` at [`video.cpp:4795`](../../src/video.cpp#L4795).
  Each iteration:
  1. drains bitrate, RFI and IDR events (`:5121`–`:5152`);
  2. pops an image (`:5233`);
  3. converts it (`:5244`);
  4. calls `encode()` (`:5286`), which dispatches at `:3580` to
     `encode_avcodec` (`:3408`), `encode_nvenc` (`:3487`) or `encode_amf`
     (`:3548`).

  PyroWave has its own loop, `encode_run_pyrowave` at `:5935`.
- **Session interface:** `video::encode_session_t`
  ([`src/video.h:278`](../../src/video.h#L278)) already has `request_idr_frame`,
  `invalidate_ref_frames`, `set_bitrate` and `set_hdr_metadata`. Add
  `set_foveation(const foveation_t &)`. Each backend implements it.
  Negotiated parameters go in `video::config_t`
  ([`src/video.h:37`](../../src/video.h#L37)).

A shared helper builds the map once per frame. It is a pure function of
`(gaze u,v, fovea radius px, falloff, block size, frame w×h)` → `int8` per
block, with a smooth radial falloff, not a hard edge. It is unit-testable
without a GPU. Each backend converts that one map to its own format.

### Per encoder, in priority order

| Priority | Encoder | Path in vibeshine | Mechanism | Granularity | Notes |
|---|---|---|---|---|---|
| **1** | **NVENC HEVC (standalone)** | [`src/nvenc/nvenc_base.cpp:936`](../../src/nvenc/nvenc_base.cpp#L936) `encode_frame`, `NV_ENC_PIC_PARAMS` at `:975` | `rcParams.qpMapMode = NV_ENC_QP_MAP_DELTA` and per-frame `pic_params.qpDeltaMap`/`qpDeltaMapSize` (signed 8-bit per block) | One value per CTB | **The main path for every phase.** This is what the user streams. Headers are SDK 13.0 (`third-party/nv-codec-headers`, `nvEncodeAPI.h:320`, `:1599`, `:2598`). Confirm the HEVC map block size against the SDK programming guide; size the map from it, not from a hard-coded 16×16. Rate control is CBR, ultra-low-latency, no lookahead (`nvenc_base.cpp:476`–`:501`). Under CBR a negative delta in the fovea is paid for by the periphery, which is the effect we want. `qpMapMode` is set at create time. It can be toggled with the existing reconfigure path used by `set_bitrate` (`nvenc_base.h:88`). The per-frame map is just a pointer. **Check:** interaction with `enableAQ` (config default off, `nvenc_config.h:63`). Older SDK docs say external QP maps and AQ are mutually exclusive [S11]. |
| 2 | NVENC H.264 (standalone) | Same | Same delta map (16×16 MB). Also `NV_ENC_QP_MAP_EMPHASIS`, levels 0–5, H.264 only; probe `NV_ENC_CAPS_SUPPORT_EMPHASIS_LEVEL_MAP` (`nvEncodeAPI.h:1148`) first. | 16×16 | Nearly free once HEVC works. Useful when a client falls back to H.264. |
| 3 | AMF (native) | [`src/amf/amf_d3d11.cpp:1663`](../../src/amf/amf_d3d11.cpp#L1663) `encode_frame`, per-surface properties | `AMF_VIDEO_ENCODER_HEVC_ROI_DATA` / `ROI_DATA` / `_AV1_ROI_DATA`: GRAY32 surface of importance 0–10. Or `BLOCK_QP_MAP`. | 64×64 (HEVC, AV1), 16×16 (H.264) | For AMD hosts. Declared in the vendored AMF SDK (`.../AMF/amf/public/include/components/VideoEncoderVCE.h:298`, `:347`). AMF ships a `SimpleROI` sample. VBAQ is configured at `amf_d3d11.cpp:536`. Check how ROI interacts with it. |
| 3 | QSV | [`video.cpp:1973`](../../src/video.cpp#L1973) (FFmpeg) | `AV_FRAME_DATA_REGIONS_OF_INTEREST` (`AVRegionOfInterest` rectangles with `qoffset`) | Rectangles → MB/CTU | hevc_qsv and h264_qsv only [S12]. Attach side data next to the HDR side data at `video.cpp:1176`, before `avcodec_send_frame` at `:3418`. Express the radial falloff as a few nested rectangles. |
| 3 | VAAPI | [`video.cpp:2405`](../../src/video.cpp#L2405) (FFmpeg) | Same ROI side data | Rectangles | Only if the driver exposes `VAConfigAttribEncROI`. FFmpeg ignores ROI with a warning otherwise [S13]. Region count is driver-capped. |
| 3 | Software (libx265, libx264) | [`video.cpp:2334`](../../src/video.cpp#L2334) | Same ROI side data | Rectangles | libsvtav1 does not consume it in FFmpeg 8.1.1. |
| — | NVENC via FFmpeg (`nvenc_legacy`) | [`video.cpp:1840`](../../src/video.cpp#L1840) | FFmpeg nvenc does **not** consume ROI side data in 8.1.1 | — | Leave unsupported; the standalone path is the default. |
| — | AMF via FFmpeg | [`video.cpp:2133`](../../src/video.cpp#L2133) | FFmpeg amf does not consume ROI side data in 8.1.1 | — | Use the native path. |
| — | Vulkan (FFmpeg `h264_vulkan`/`hevc_vulkan`, Linux) | [`video.cpp:2535`](../../src/video.cpp#L2535) | Not a ROI consumer in 8.1.1 | — | Defer. |
| — | Media Foundation | [`video.cpp:2276`](../../src/video.cpp#L2276) | None practical | — | Defer. |
| Optional, last | PyroWave | `third-party/pyrowave/` (Vulkan-compute wavelet codec). Loop at [`video.cpp:5935`](../../src/video.cpp#L5935). | Per-block **distortion weight**. Scale squared error by a spatial weight in `shaders/wavelet_quant.comp:127` (where `rdo_distortion_scale` is applied). | 64×64 coefficient blocks | **Not usable today:** the user's Frame client has no PyroWave decoder. See Phase 4. The exact-size rate control (`analyze_`/`resolve_rate_control.comp`) would reallocate bits towards the fovea automatically. Needs a new local patch under `third-party/pyrowave/patches/` and a gaze push-constant or SSBO. Intra-only, so gaze changes take effect on the next frame with no reference-frame smear. |

### Bandwidth and quality

- **The limit is the link, not the decoder, if HEVC decodes in hardware.**
  The Lepton fork's ~40 Mbps ceiling [S5] came from software H.264 on the CPU
  and does not apply to the user's client. Open check 2 confirms the decode
  path. If HEVC is decoded in software on the Frame, the CPU ceiling returns
  and foveation matters more.
- **The Frame is wireless.** Valve ships it with a Wi-Fi 6E dongle for the PC
  [S8]. The useful bitrate is whatever that link sustains without loss
  spikes. Foveation's job here is to keep the fovea sharp at a bitrate the
  wireless link can hold, not to save bandwidth for its own sake.
- **HEVC gains less than a codec with no references.** At a fixed CBR target,
  a QP map moves bits from the periphery to the fovea. Total bits stay the
  same, so the win is image quality where the eye is, not a lower bitrate.
  A lower bitrate at the same perceived quality is the second experiment:
  lower the target until the fovea looks like the uniform stream.
- **The periphery to degrade is small.** A virtual screen covers far less of
  the field of view than a VR eye buffer. Expect modest gains from QP maps.
  Foveated resolution (below) is where big savings are.
- **PyroWave does not fit this link today.** It needs very high bitrates and
  is intended for wired LANs (`docs/pyrowave-protocol.md`, [S15]). The host
  advertises `PyroWaveHostLinkMbps` as zero for wireless links. This is one
  more reason PyroWave is the last, optional phase.

### Foveated resolution, not just QP

Steam Link appears to send the fovea at higher *resolution*, not just higher
quality [S9]. OXRSys does it by warping the image around a moving centre and
un-warping on the client, with the centre carried per frame [S7]. This saves
far more bandwidth than QP maps. It also changes the image geometry, so it
needs:

- a host-side warp pass after capture (for example in the existing convert
  shader stage);
- the warp centre carried per frame in the video stream;
- a matching un-warp on the client, which means a custom `moonlight-qt`
  build.

It is a phase-3 option, not the first step. QP maps are invisible to
non-foveation-aware clients and need no client change, which is why they come
first for a stock Flathub client.

## 4. Phased plan

Every phase uses NVENC HEVC and the user's Flathub Moonlight unless it says
otherwise.

### Phase 0: fixed-centre QP map on NVENC HEVC (host only, no client change)

- Set up the build first. See [Build setup](#build-setup-for-phase-0-on-windows).
- Add a config option, e.g. `foveation = off|fixed`, with
  `foveation_center = 0.5,0.5`, `foveation_radius = 0.15`
  (fraction of frame height) and `foveation_strength` (max QP delta).
- Shared map builder plus unit tests: map shape, size per codec block, and
  clamping.
- NVENC: set `qpMapMode = NV_ENC_QP_MAP_DELTA` at create time when enabled.
  Fill `qpDeltaMap` per frame in `encode_frame`, sized from the HEVC block
  size.
- **Exit check:**
  - Stream HEVC to the Flathub Moonlight on the Frame.
  - Record the decoder path and the highest bitrate that streams without
    dropped frames (open checks 2 and the Wi-Fi limit).
  - Confirm visually that the centre is sharper and the edges softer at a
    fixed bitrate.
  - Log encoded frame sizes to confirm CBR holds.
  - Record whether the map holds every frame. An old forum report says only
    the first frame was honoured in some RC modes [S11].
  - Optionally repeat on H.264, including emphasis mode.

### Phase 1: moving stand-in gaze on the host

- Add `foveation = synthetic`. The centre follows the host mouse cursor, or a
  slow scripted sweep (cf. OXRSys's `FRAME_CLIENT_SYNTHETIC_GAZE` [S7]).
- Add `encode_session_t::set_foveation` and the `MAIL(gaze)` plumbing, with a
  host-side source feeding it.
- Add low-pass and hysteresis on the centre, so fixation jitter does not make
  the quality boundary shimmer [S7].
- **Exit check:** the map moves with no encoder resets and no IDRs. Measure
  the per-frame cost of building and uploading the map.

In parallel, run open hardware check 1 on the Frame. It decides Phase 2.

### Phase 2: real gaze from the Frame

- **Gaze source:** G1 (helper) if check 1 passes. Otherwise G2, then G3.
  - The source enables `XR_EXT_eye_gaze_interaction` and checks
    `XrSystemEyeGazeInteractionPropertiesEXT::supportsEyeGazeInteraction`.
  - It sends a gaze direction (G1, G2) or `(u, v)` on the screen quad (G3).
- **Transport:** T1 side channel for G1 and G2. T2 control packet for G3.
- **Host:** convert direction to `(u, v)` from the configured screen size and
  calibration. Use the newest sample at encode time. When gaze goes invalid,
  decay towards the centre or fall back to uniform quality; do not freeze the
  last point [S7].
- **Latency:** measure gaze-to-encode time from the sample timestamp. Measure
  end-to-end with a test pattern that flashes at the gaze point.

### Phase 3: other encoders and stronger foveation

- **NVENC H.264** if not already done in Phase 0.
- **Native AMF:** add an ROI importance surface.
- **QSV, VAAPI, x265 and x264:** add FFmpeg ROI side data built from nested
  rectangles.
- **Optional:** foveated resolution (warp/un-warp, with a per-frame centre in
  the video header) for the biggest bitrate saving. It needs a custom
  `moonlight-qt` build.
- **WebRTC path:** `src/webrtc_stream.cpp` is a separate output path. Decide
  whether it gets gaze at all.

### Phase 4 (optional): PyroWave

Only if Phases 0–2 show that HEVC with a QP map is not good enough, and only
if the Frame's link can carry PyroWave's bitrate.

1. **Add PyroWave decode to the Frame client first.** That means an aarch64
   build of Nonary's VRR `moonlight-qt` fork, such as the community
   `murmrs` handheld build [S15], installed in place of the Flathub
   Moonlight. Test it on the Frame's Adreno GPU before any host work.
   An issue on Nonary's fork shows PyroWave failing on an Adreno GPU under
   Windows on ARM [S18].
2. Confirm the host then offers PyroWave at pairing and the stream is stable.
3. Add the distortion-weight patch described in the encoder table.

## Build setup for Phase 0 on Windows

Nothing is installed yet. This is what a Windows x64 PC with an NVIDIA GPU
needs to build vibeshine and run Phase 0. It follows `docs/building.md` and
the Windows CI job (`.github/workflows/ci-windows.yml`), which is the
authoritative package list. Do these steps in order.

**What is not needed:**

- **No CUDA toolkit and no NVIDIA Video Codec SDK download.** On Windows,
  NVENC is built against the vendored headers in
  `third-party/nv-codec-headers` (SDK 13.0). The CUDA options in
  `cmake/prep/options.cmake` exist only for Linux. At run time, NVENC needs
  only a current NVIDIA driver.
- **No Vulkan SDK.** PyroWave uses the Vulkan headers submodule, and its
  shaders are already compiled into the committed
  `third-party/pyrowave/pyrowave/shaders/slangmosh.hpp`.
- **No Visual Studio.** It is needed only for WebRTC
  (`SUNSHINE_ENABLE_WEBRTC`, default `OFF`). Leave WebRTC off.
- **No WiX.** It is needed only to package an MSI installer.

**Steps:**

1. **Install the NVIDIA driver** for the host GPU, if not already current.
2. **Install MSYS2** from <https://www.msys2.org> into the default
   `C:\msys64`.
3. **Open the "MSYS2 UCRT64" shell** from the Start menu. Run the remaining
   shell commands in it.
4. **Update MSYS2.** Run `pacman -Syu`. If it closes the shell, open
   "MSYS2 UCRT64" again and run `pacman -Syu` a second time.
5. **Install the build packages** (the same list as Windows CI):

   ```bash
   pacman -S --needed \
     git \
     mingw-w64-ucrt-x86_64-boost \
     mingw-w64-ucrt-x86_64-cmake \
     mingw-w64-ucrt-x86_64-cppwinrt \
     mingw-w64-ucrt-x86_64-curl-winssl \
     mingw-w64-ucrt-x86_64-gcc \
     mingw-w64-ucrt-x86_64-libjpeg-turbo \
     mingw-w64-ucrt-x86_64-libpng \
     mingw-w64-ucrt-x86_64-libwebp \
     mingw-w64-ucrt-x86_64-MinHook \
     mingw-w64-ucrt-x86_64-miniupnpc \
     mingw-w64-ucrt-x86_64-ninja \
     mingw-w64-ucrt-x86_64-nlohmann-json \
     mingw-w64-ucrt-x86_64-onevpl \
     mingw-w64-ucrt-x86_64-openssl \
     mingw-w64-ucrt-x86_64-opus \
     mingw-w64-ucrt-x86_64-sqlite3 \
     mingw-w64-ucrt-x86_64-tools
   ```

   This gives GCC, CMake and Ninja inside UCRT64.
6. **Install the .NET SDK** (CI uses 10.x) from
   <https://dotnet.microsoft.com/download>. Configuration fails without it,
   because the Playnite plugin is always built
   (`cmake/targets/windows.cmake:56`).
7. **Install Node.js 22 LTS** (CI pins 22.22.2) from <https://nodejs.org>,
   with npm. The `web_ui` target builds the web interface with it.
8. **Get the source with submodules.** The build needs every submodule; this
   worktree has none checked out yet.

   ```bash
   git clone --recurse-submodules https://github.com/Shaunf1/vibeshine.git
   cd vibeshine
   ```

   In an existing checkout, run `git submodule update --init --recursive`
   instead.
9. **Configure.** Point CMake at the Windows Node.js and .NET installs. The
   paths below are the installers' defaults.

   ```bash
   export PATH="/c/Program Files/nodejs:$PATH"
   cmake -B build -G Ninja -S . \
     -DCMAKE_BUILD_TYPE=Release \
     -DBUILD_DOCS=OFF \
     -DBUILD_TESTS=ON \
     -DSUNSHINE_NPM_EXECUTABLE="C:/Program Files/nodejs/npm.cmd" \
     -DSUNSHINE_DOTNET_EXECUTABLE="C:/Program Files/dotnet/dotnet.exe"
   ```

   Configure and build download two pinned prebuilt packages from GitHub, so
   the PC needs internet access: FFmpeg from `LizardByte/build-deps`
   (`cmake/dependencies/ffmpeg.cmake`) and the virtual display driver from
   `Nonary/libvirtualdisplay` (`cmake/packaging/windows.cmake`).
10. **Build.**

    ```bash
    ninja -C build
    ```

    The output is `build/sunshine.exe`.
11. **Run the tests.**

    ```bash
    ctest --test-dir build --output-on-failure
    ```

    Phase 0's map-builder unit tests run here.
12. **Run the host for the Phase 0 check.** Start `build/sunshine.exe` from an
    administrator shell. Open the web UI at <https://localhost:47990>. Pair
    the Frame's Moonlight, then stream with HEVC selected in Moonlight's
    settings.

If a step fails, `docs/building.md` and the "Build Windows" step in
`.github/workflows/ci-windows.yml` are the references. CI also passes flags
for signing, symbols and bundled drivers, which a local Phase 0 build does
not need.

## Risks and unknowns

1. **Gaze for a flat client.** No public document says how a flat app on the
   Frame can read gaze. Valve documents `XR_EXT_eye_gaze_interaction` for
   OpenXR apps only [S1]. Open check 1 decides between G1, G2 and G3. If all
   fail, foveation stays host-side (fixed or synthetic centre) for the
   Flathub client.
2. **Decode path and link budget.** It is not yet confirmed whether HEVC
   decodes in hardware on the Frame. The Wi-Fi link sets the real bitrate.
3. **Latency versus fovea size.** Gaze to photon here includes:
   - the helper or client sampling delay;
   - the network trip;
   - one encode;
   - the decode;
   - compositor latency.

   That is likely several frames. The fovea must be large enough, with a soft
   falloff, so saccades land inside the sharp region until the next frame
   catches up. Tune this empirically. There are no published numbers for
   this setup.
4. **Coarse gaze mapping.** With a helper, the host does not know where the
   virtual screen is. Calibration covers this, but moving or resizing the
   screen breaks it.
5. **Small benefit for a flat screen.** A virtual screen covers far less of
   the field of view than a VR eye buffer. Expect modest gains from QP maps.
6. **Encoder behaviour.** It is unconfirmed whether NVENC honours per-frame
   HEVC delta maps under CBR, ultra-low-latency, multipass, on every frame,
   and on every supported driver (API probing accepts 11.0–13.0,
   `src/nvenc/nvenc_api.h:68`). It is also unconfirmed whether the maps
   interact badly with AQ.
7. **Rate-control artefacts.** A moving high-quality region under CBR with
   infinite GOP may leave a "quality trail" in referenced frames. P-frames
   inherit a sharp fovea from earlier frames for a while. This is likely
   harmless, but watch for it.
8. **Steam Link already does this.** For Steam content the user may get
   foveated streaming for free [S8]. This plan only pays off if the
   vibeshine-specific features matter.
9. **Private gaze sources are off the table.** The shared-memory route [S4]
   is undocumented and may breach the Steam Subscriber Agreement. Do not
   build on it.
10. **Side-channel security.** A T1 gaze port must not accept unauthenticated
    packets from the network. Gaze only steers quality, but the port is new
    attack surface.
11. **Licensing.** `moonlight-qt` and vibeshine are GPL-3.0. Changes to
    `moonlight-common-c` belong in the vibeshine branch of that submodule.

## Sources

- [S1] Valve, *Steam Frame: Custom Engines*: eye tracking uses `XR_EXT_eye_gaze_interaction`; supported foveation extensions; Android 10 / Linux ARM64 targets. <https://partner.steamgames.com/doc/steamhardware/steamframe/engines/custom> (index: <https://partner.steamgames.com/doc/steamhardware/steamframe/engines>)
- [S2] Valve, *Steam Frame: Unity*: Eye Gaze Interaction feature; foveated rendering settings. <https://partner.steamgames.com/doc/steamhardware/steamframe/engines/unity>
- [S3] Khronos, OpenXR specification, `XR_EXT_eye_gaze_interaction`. <https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html#XR_EXT_eye_gaze_interaction> and <https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_EXT_eye_gaze_interaction.html>
- [S4] frameeyeosc, reads Frame eye data from private shared memory on the headset (not recommended). <https://github.com/sasaken1102r/frameeyeosc> (upstream <https://github.com/konsti219/frameeyeosc>)
- [S5] ajbeavers, *Moonlight XR on the Steam Frame through Lepton*: STEAM_FRAME.md and releases (software H.264 decode, ~40 Mbps at 1440p, OpenXR extensions; v0.3 of 2026-10-11 still says "the Frame decodes in software"). <https://github.com/ajbeavers/moonlight-android-xr-steam-frame/blob/HEAD/STEAM_FRAME.md>, <https://github.com/ajbeavers/moonlight-android-xr-steam-frame/releases/tag/v0.3>
- [S6] GamingOnLinux, *Lepton from Valve to run Android games on Linux is now open source* (Sept 2026). Repo: <https://gitlab.steamos.cloud/frame-public/lepton/>. <https://www.gamingonlinux.com/2026/09/lepton-from-valve-to-run-android-games-on-linux-is-now-open-source/>
- [S7] demonixis/oxrsys PR #30, *Add a Steam Frame client (draft)*: eye gaze via `XR_EXT_eye_gaze_interaction`, gaze in tracking packet, capability-gated foveation centre, low-pass, synthetic gaze. <https://github.com/demonixis/oxrsys/pull/30>
- [S8] Road to VR, *Steam Frame hands-on*: foveated streaming applies to all streamed Steam content, VR or flat; Wi-Fi 6E dongle. <https://roadtovr.com/steam-frame-hands-on-valve-vr-headset-index-2>
- [S9] Community summaries of Steam Frame foveated streaming (unofficial): <https://www.resetera.com/threads/steam-frame-answering-a-lot-of-the-questions-and-misconceptions-that-people-have.1352908/page-3>, <https://en.wikipedia.org/wiki/Steam_Frame>
- [S10] UploadVR, Steam Frame announcement. <https://uploadvr.com/valve-steam-frame-official-announcement-features-details>
- [S11] NVIDIA developer forums on `qpDeltaMap` / emphasis maps. <https://forums.developer.nvidia.com/t/sdk-8-1-emphasis-map/60317>, <https://forums.developer.nvidia.com/t/qpdeltamap-in-nvenc-5-0-1-sdk-samples/39121>; `NV_ENC_RC_PARAMS` doxygen <https://ffmpeg.org/doxygen/3.4/structNV__ENC__RC__PARAMS.html>
- [S12] FFmpeg-devel, *qsvenc: Add ROI encoding support for h264_qsv and hevc_qsv*. <https://lists.ffmpeg.org/pipermail/ffmpeg-devel/2019-December/254794.html>
- [S13] FFmpeg-cvslog, *vaapi_encode: Add ROI support*. <https://lists.ffmpeg.org/pipermail/ffmpeg-cvslog/2019-July/118423.html>
- [S14] Nonary, VRR `moonlight-qt` fork, release v6.1.0-vrr18 (2026-09-30): PyroWave decode on Windows and Linux; Linux assets are x86_64 only. <https://github.com/Nonary/moonlight-qt/releases/tag/v6.1.0-vrr18>
- [S15] murmrs, *Moonlight VRR 6.1.0-vrr18 handheld 1*: aarch64 Flatpak with PyroWave decode enabled, same app ID as Flathub Moonlight, limited Adreno testing, PyroWave needs a very high bitrate. <https://github.com/murmrs/moonlight-qt/releases/tag/v6.1.0-vrr18-handheld1>
- [S16] moonlight-stream, upstream `moonlight-qt` releases (v6.2.0, 2026-10-04). <https://github.com/moonlight-stream/moonlight-qt/releases>
- [S17] Flathub, `com.moonlight_stream.Moonlight` manifest repository. <https://github.com/flathub/com.moonlight_stream.Moonlight>
- [S18] Nonary/moonlight-qt issue #19, *PyroWave on Snapdragon X (Adreno X1-85): D3D11 texture/fence import unsupported*. <https://github.com/Nonary/moonlight-qt/issues/19>
- In-tree: `docs/building.md`, `.github/workflows/ci-windows.yml`, `docs/pyrowave-protocol.md`, `third-party/nv-codec-headers` (SDK 13.0.19), `third-party/build-deps/third-party/FFmpeg/AMF` (AMF SDK headers and `SimpleROI` sample), `third-party/build-deps/third-party/FFmpeg/FFmpeg` (8.1.1), `third-party/pyrowave`, `third-party/moonlight-common-c`.
