# Eye-tracked foveated streaming to the Steam Frame

Status: research and plan only. No product code yet.
Date: 2026-10-11.

## Goal

Stream from a vibeshine host to a Valve Steam Frame. The encoder spends more
bits where the wearer is looking and fewer bits in the periphery. The headset's
eye tracker supplies the gaze point. This is how Valve's own Steam Link
streaming already works on the Frame. The aim is to get the same benefit in a
Moonlight-protocol stream.

## Summary

- **Gaze is available to third-party apps.** Valve documents eye tracking on
  the Frame through the OpenXR extension `XR_EXT_eye_gaze_interaction` [S1].
  The Unity path uses the same extension [S2]. A client must be an OpenXR app
  on the headset to read it.
- **A suitable client already exists in early form.** A community fork of
  Moonlight XR runs as a native OpenXR app on the Frame through Lepton, Valve's
  Android compatibility layer [S5][S6]. It decodes H.264 in software and does
  not use eye tracking yet [S5].
- **The host side is mostly plumbing.** vibeshine has no ROI, QP-map or gaze
  code today. The vendored headers already expose everything needed for NVENC
  (`qpDeltaMap` with `qpMapMode`) and native AMF (`ROIData`). The prebuilt
  FFmpeg 8.1.1 accepts `AV_FRAME_DATA_REGIONS_OF_INTEREST` for QSV, VAAPI,
  libx264 and libx265.
- **The protocol change is small.** It adds one new client-to-host control
  packet carrying a normalised gaze point, plus a feature flag in each
  direction. The handler follows the existing reference-frame-invalidation
  (RFI) handler.
- **The smallest proof** is a fixed-centre QP delta map on the standalone
  NVENC path. It is driven by a config option and needs no client change.
  Phase 0 below covers it.
- **Weigh this first.** Valve says Steam Link's foveated streaming applies to
  "all streamed Steam content" without developer work, flat games included
  [S8]. If the game is in Steam and Steam Link's quality is good enough, the
  user may not need this project at all. The case for vibeshine is non-Steam
  content, vibeshine's display and virtual-display management, or control
  over codec and bitrate.

## 1. How the Steam Frame exposes gaze

| Route | What it gives | Usable by us? | Source |
|---|---|---|---|
| OpenXR `XR_EXT_eye_gaze_interaction` on the headset | Gaze pose action (`/user/eyes_ext/input/gaze_ext/pose`) located with `xrLocateSpace` at a predicted time | **Yes. This is the supported route.** | Valve custom-engine docs [S1]; Khronos spec [S3] |
| `XR_META_foveation_eye_tracked`, `XR_FB_foveation*` | Eye-tracked foveated *rendering* of the app's own swapchains | Not for gaze data. Useful later for rendering the client's own layers. | [S1] |
| Unity "Eye Gaze Interaction" feature | Wrapper over the same OpenXR extension | Only if the client is Unity-based | [S2] |
| Private shared memory `/dev/shm/eye-server.mmap` on the headset | Gaze plus eyelid openness at 90–136 Hz while streaming | **No.** Undocumented, may change with any SteamOS update, and the tool's author warns it may conflict with the Steam Subscriber Agreement. | frameeyeosc README [S4] |
| SteamVR on the PC (OpenXR eye gaze to PC apps while the Frame is tethered to SteamVR) | Gaze on the host | Only while the Frame is in SteamVR/Steam Link mode. In that mode Steam Link, not vibeshine, carries the video. Out of scope. | inferred from [S1][S8] |
| Steam Link's foveated-streaming internals | Not publicly documented beyond "eye tracking to optimise the streamed image to have the highest quality at the very centre of your view" | Nothing to reuse | [S8][S9] |

**Can a Moonlight-style client on the headset read gaze and send it with low
latency?** Yes, if it is an OpenXR app. The OpenXR session gives the gaze pose
each frame. The client sends a small packet per decoded frame, or at the
eye-tracker rate, over the existing ENet control connection. It sends
unreliably, so a stale gaze sample is dropped and never retransmitted. One
open-source streaming client already does exactly this pattern for its own
runtime. The draft OXRSys Steam Frame client reads `XR_EXT_eye_gaze_interaction`
and sends a head-space gaze vector in its tracking packet. Its server low-passes
the gaze and steers the foveal centre, gated on a client capability flag [S7].
That design is a good reference.

Things not yet confirmed on hardware (see Unknowns):

- whether Lepton's OpenXR path exposes `XR_EXT_eye_gaze_interaction` to an
  Android APK, not only to native Linux ARM64 apps;
- any permission prompt;
- the real sample rate.

## 2. Which client, and what protocol change

### Client options

| Option | Notes |
|---|---|
| **A. Moonlight XR fork on Lepton** (`ajbeavers/moonlight-android-xr-steam-frame`, GPL-3.0) [S5][S6] | Already streams from Sunshine-family hosts on the Frame as an OpenXR app. It builds on `moonlight-common-c`, so our protocol additions slot in. **Limits:** H.264 only, software decode on the CPU, about 40 Mbps at 1440p before the decoder falls behind, and 90 Hz only [S5]. Eye gaze is not wired up. **Recommended first target.** |
| B. Native Linux ARM64 OpenXR client (e.g. `moonlight-qt` ported, or a new client in the style of the OXRSys Frame client [S7]) | Could get hardware decode and HEVC/AV1. Valve recommends "Android 10 or Linux Arm64" for custom engines [S1]. Far more work. The fork's README says a native "Moonlight XR for Frame" client is coming, but this is unconfirmed. |
| C. Flat Android Moonlight in Lepton's 2D window | Lepton hides the flat window by default [S6] and a 2D app has no OpenXR session, so it cannot read gaze. **Rejected.** |

The software-decode limit matters for this project. It fits the
foveation story: foveation keeps the fovea sharp at a bitrate the CPU decoder
can sustain. It also means H.264 is the codec that matters on the client side
today. NVENC's emphasis mode is H.264-only. QP delta maps work for all codecs.

### Mapping gaze to the stream

vibeshine streams a flat desktop or game, which the client shows on a virtual
screen quad in the OpenXR scene. The client therefore:

1. locates the gaze pose in the same reference space as the screen quad;
2. intersects the gaze ray with the quad, giving `(u, v)` in `[0,1]²` in
   **video-frame coordinates**, or "off-screen";
3. sends `(u, v)`, a validity/confidence flag, the frame number of the latest
   decoded frame and a client timestamp.

The host never needs to know about eyes, FOV or head pose. Only `(u, v)` in
frame space matters. The client should also send the angular size of the
screen, or the host can take it from config. With that, the host can size the
fovea in pixels: a 5° radius covers a different fraction of the frame on a
40° virtual screen than on a 100° one.

### Protocol change (host + `moonlight-common-c`)

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
- **Host mail:**
  - Add `MAIL(gaze)` next to `MAIL(idr)` and `MAIL(invalidate_ref_frames)`
    in the mail list at [`src/globals.h:46`–`58`](../../src/globals.h#L46-L58).
  - Bind it like the other session events at
    [`src/stream.cpp:619`](../../src/stream.cpp#L619) and
    [`:3843`](../../src/stream.cpp#L3843).
  - Drain it in the encode loop next to RFI at
    [`src/video.cpp:5139`](../../src/video.cpp#L5139). Take only the newest
    sample.

Why a control packet and not an input packet: input packets (`SS_PEN`,
`SS_CONTROLLER_MOTION` in `Input.h`) are the precedent for high-rate floats,
but the host processes them on the input thread
([`src/input.cpp:461`](../../src/input.cpp#L461)). A control packet goes
straight to the encode thread's mail.

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

### Per encoder

| Encoder | Path in vibeshine | Mechanism | Granularity | Notes |
|---|---|---|---|---|
| **NVENC (standalone)** | [`src/nvenc/nvenc_base.cpp:936`](../../src/nvenc/nvenc_base.cpp#L936) `encode_frame`, `NV_ENC_PIC_PARAMS` at `:975` | `rcParams.qpMapMode = NV_ENC_QP_MAP_DELTA` and per-frame `pic_params.qpDeltaMap`/`qpDeltaMapSize`. Alternatively `NV_ENC_QP_MAP_EMPHASIS` with levels 0–5, H.264 only. | 16×16 MB (H.264), CTB (HEVC), SB (AV1) | **Best first target.** Headers are SDK 13.0 (`third-party/nv-codec-headers`, `nvEncodeAPI.h:320`, `:1599`, `:2598`). Rate control is CBR, ultra-low-latency, no lookahead (`nvenc_base.cpp:476`–`:501`). Under CBR a negative delta in the fovea is paid for by the periphery, which is the effect we want. `qpMapMode` is set at create time. It can be toggled with the existing reconfigure path used by `set_bitrate` (`nvenc_base.h:88`). The per-frame map is just a pointer. **Check:** interaction with `enableAQ` (config default off, `nvenc_config.h:63`). Older SDK docs say external QP maps and AQ are mutually exclusive [S11]. Probe `NV_ENC_CAPS_SUPPORT_EMPHASIS_LEVEL_MAP` (`nvEncodeAPI.h:1148`) before using emphasis mode. |
| NVENC via FFmpeg (`nvenc_legacy`) | [`video.cpp:1840`](../../src/video.cpp#L1840) | FFmpeg nvenc does **not** consume ROI side data in 8.1.1 | — | Leave unsupported; the standalone path is the default. |
| **AMF (native)** | [`src/amf/amf_d3d11.cpp:1663`](../../src/amf/amf_d3d11.cpp#L1663) `encode_frame`, per-surface properties | `AMF_VIDEO_ENCODER_ROI_DATA` / `_HEVC_ROI_DATA` / `_AV1_ROI_DATA`: GRAY32 surface of importance 0–10. Or `BLOCK_QP_MAP`. | 16×16 (H.264), 64×64 (HEVC, AV1) | Declared in the vendored AMF SDK (`.../AMF/amf/public/include/components/VideoEncoderVCE.h:298`, `:347`). AMF ships a `SimpleROI` sample. VBAQ is configured at `amf_d3d11.cpp:536`. Check how ROI interacts with it. |
| AMF via FFmpeg | [`video.cpp:2133`](../../src/video.cpp#L2133) | FFmpeg amf does not consume ROI side data in 8.1.1 | — | Use the native path. |
| **QSV** | [`video.cpp:1973`](../../src/video.cpp#L1973) (FFmpeg) | `AV_FRAME_DATA_REGIONS_OF_INTEREST` (`AVRegionOfInterest` rectangles with `qoffset`) | Rectangles → MB/CTU | h264_qsv and hevc_qsv only [S12]. Attach side data next to the HDR side data at `video.cpp:1176`, before `avcodec_send_frame` at `:3418`. Express the radial falloff as a few nested rectangles. |
| **VAAPI** | [`video.cpp:2405`](../../src/video.cpp#L2405) (FFmpeg) | Same ROI side data | Rectangles | Only if the driver exposes `VAConfigAttribEncROI`. FFmpeg ignores ROI with a warning otherwise [S13]. Region count is driver-capped. |
| **Software** (libx264, libx265) | [`video.cpp:2334`](../../src/video.cpp#L2334) | Same ROI side data | Rectangles | libsvtav1 does not consume it in 8.1.1. |
| Vulkan (FFmpeg `h264_vulkan`/`hevc_vulkan`, Linux) | [`video.cpp:2535`](../../src/video.cpp#L2535) | Not a ROI consumer in 8.1.1 | — | Would need FFmpeg work or a QP-map extension. Defer. |
| Media Foundation | [`video.cpp:2276`](../../src/video.cpp#L2276) | None practical | — | Defer. |
| **PyroWave** | `third-party/pyrowave/` (Vulkan-compute wavelet codec). Loop at [`video.cpp:5935`](../../src/video.cpp#L5935). | Per-block **distortion weight**. Scale squared error by a spatial weight in `shaders/wavelet_quant.comp:127` (where `rdo_distortion_scale` is applied). | 64×64 coefficient blocks | The exact-size rate control (`analyze_`/`resolve_rate_control.comp`) then reallocates bits towards the fovea automatically. This needs a new local patch under `third-party/pyrowave/patches/` and a gaze push-constant or SSBO. PyroWave is intra-only, so gaze changes take effect on the next frame with no reference-frame smear, which is a good fit. |

### Foveated resolution, not just QP

Steam Link appears to send the fovea at higher *resolution*, not just higher
quality [S9]. OXRSys does it by warping the image around a moving centre and
un-warping on the client, with the centre carried per frame [S7]. This saves
far more bandwidth than QP maps. It also changes the image geometry, so it
needs:

- a host-side warp pass after capture (for example in the existing convert
  shader stage);
- the warp centre carried per frame in the video stream;
- a matching un-warp on the client.

It is a phase-3 option, not the first step. QP or ROI maps are invisible to
non-foveation-aware clients and need no client decode changes.

## 4. Phased plan

### Phase 0: fixed-centre QP map on NVENC (host only, no client change)

- Add a config option, e.g. `foveation = off|fixed`, with
  `foveation_center = 0.5,0.5`, `foveation_radius = 0.15`
  (fraction of frame height) and `foveation_strength` (max QP delta).
- Shared map builder plus unit tests: map shape, size per codec block, and
  clamping.
- NVENC: set `qpMapMode = NV_ENC_QP_MAP_DELTA` at create time when enabled.
  Fill `qpDeltaMap` per frame in `encode_frame`.
- **Exit check:**
  - Stream to any existing Moonlight client.
  - Confirm visually that the centre is sharper and the edges softer at a
    fixed bitrate.
  - Log encoded frame sizes to confirm CBR holds.
  - Try the same with emphasis mode on H.264.
  - Record whether the map holds every frame. An old forum report says only
    the first frame was honoured in some RC modes [S11].

### Phase 1: moving stand-in gaze on the host

- Add `foveation = synthetic`. The centre follows the host mouse cursor, or a
  slow scripted sweep (cf. OXRSys's `FRAME_CLIENT_SYNTHETIC_GAZE` [S7]).
- Add `encode_session_t::set_foveation` and the `MAIL(gaze)` plumbing, with a
  host-side source feeding it.
- Add low-pass and hysteresis on the centre, so fixation jitter does not make
  the quality boundary shimmer [S7].
- **Exit check:** the map moves with no encoder resets and no IDRs. Measure
  the per-frame cost of building and uploading the map.

### Phase 2: real gaze from the Frame

- **Protocol:** the feature flags and `IDX_GAZE` packet above, in the host and
  in a `moonlight-common-c` branch.
- **Client:** in the Moonlight XR Steam Frame fork:
  - enable `XR_EXT_eye_gaze_interaction`;
  - check `XrSystemEyeGazeInteractionPropertiesEXT::supportsEyeGazeInteraction`;
  - intersect the gaze ray with the screen quad;
  - send `(u, v)` each frame.
- **Host:** use the newest gaze sample at encode time. When gaze goes
  invalid, decay towards the centre or fall back to uniform quality; do not
  freeze the last point [S7].
- **Latency:** measure gaze-to-encode time with `client_time_us` and
  `last_decoded_frame`. Measure end-to-end with a test pattern that flashes at
  the gaze point.

### Phase 3: other encoders and stronger foveation

- **Native AMF:** add an ROI importance surface.
- **QSV, VAAPI, x264 and x265:** add FFmpeg ROI side data built from nested
  rectangles.
- **PyroWave:** add a distortion-weight patch.
- **Optional:** foveated resolution (warp/un-warp, with a per-frame centre in
  the video header) for the biggest bitrate saving. It needs client decode
  changes.
- **WebRTC path:** `src/webrtc_stream.cpp` is a separate output path. Decide
  whether it gets gaze at all.

## Risks and unknowns

1. **Lepton plus eye gaze.** It is unverified that an Android APK under Lepton
   can enable `XR_EXT_eye_gaze_interaction`. Valve documents it for engines
   generally, not per platform [S1]. The Lepton fork's OpenXR notes list
   only controller, OpenGL ES and Android create-instance extensions [S5].
   Verify on hardware first. If it is missing, the fallback is a native Linux
   ARM64 client (option B).
2. **Client decode budget.** Software H.264 at about 40 Mbps / 1440p [S5]
   limits resolution and frame rate. Foveation helps quality at that bitrate
   but cannot raise the decode ceiling.
3. **Latency versus fovea size.** Gaze to photon here includes:
   - the network round trip;
   - one encode;
   - a software decode;
   - compositor latency.

   That is likely several frames. The fovea must be large enough, with a soft
   falloff, so saccades land inside the sharp region until the next frame
   catches up. Tune this empirically. There are no published numbers for
   this setup.
4. **Small benefit for a flat screen.** A virtual screen covers far less of
   the field of view than a VR eye buffer, so the periphery that can be
   degraded is smaller than in Steam Link VR. Expect modest savings from QP
   maps. Foveated resolution is where the big wins are.
5. **Encoder behaviour.** It is unconfirmed whether NVENC honours per-frame
   delta maps under CBR, ultra-low-latency, multipass, on every frame, on
   every codec, and on every supported driver (API probing accepts 11.0–13.0,
   `src/nvenc/nvenc_api.h:68`). It is also unconfirmed whether the maps
   interact badly with AQ. Same for AMF ROI with VBAQ and pre-analysis.
6. **Rate-control artefacts.** A moving high-quality region under CBR with
   infinite GOP may leave a "quality trail" in referenced frames. P-frames
   inherit a sharp fovea from earlier frames for a while. This is likely
   harmless, but watch for it.
7. **Steam Link already does this.** For Steam content the user may get
   foveated streaming for free [S8]. This plan only pays off if the
   vibeshine-specific features matter.
8. **Private gaze sources are off the table.** The shared-memory route [S4]
   is undocumented and may breach the Steam Subscriber Agreement. Do not
   build on it.
9. **Licensing.** The candidate client fork is GPL-3.0 [S5]. vibeshine is
   GPL-3.0 as well (Sunshine). Changes to `moonlight-common-c` belong in the
   vibeshine branch of that submodule.

## Sources

- [S1] Valve, *Steam Frame: Custom Engines*: eye tracking uses `XR_EXT_eye_gaze_interaction`; supported foveation extensions; Android 10 / Linux ARM64 targets. <https://partner.steamgames.com/doc/steamhardware/steamframe/engines/custom> (index: <https://partner.steamgames.com/doc/steamhardware/steamframe/engines>)
- [S2] Valve, *Steam Frame: Unity*: Eye Gaze Interaction feature; foveated rendering settings. <https://partner.steamgames.com/doc/steamhardware/steamframe/engines/unity>
- [S3] Khronos, OpenXR specification, `XR_EXT_eye_gaze_interaction`. <https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html#XR_EXT_eye_gaze_interaction> and <https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_EXT_eye_gaze_interaction.html>
- [S4] frameeyeosc, reads Frame eye data from private shared memory on the headset (not recommended). <https://github.com/sasaken1102r/frameeyeosc> (upstream <https://github.com/konsti219/frameeyeosc>)
- [S5] ajbeavers, *Moonlight XR on the Steam Frame through Lepton*: STEAM_FRAME.md and releases (software H.264 decode, ~40 Mbps at 1440p, 90 Hz, OpenXR extensions). <https://github.com/ajbeavers/moonlight-android-xr-steam-frame/blob/HEAD/STEAM_FRAME.md>, <https://github.com/ajbeavers/moonlight-android-xr-steam-frame/releases/tag/v0.3>
- [S6] GamingOnLinux, *Lepton from Valve to run Android games on Linux is now open source* (Sept 2026). Repo: <https://gitlab.steamos.cloud/frame-public/lepton/>. <https://www.gamingonlinux.com/2026/09/lepton-from-valve-to-run-android-games-on-linux-is-now-open-source/>
- [S7] demonixis/oxrsys PR #30, *Add a Steam Frame client (draft)*: eye gaze via `XR_EXT_eye_gaze_interaction`, gaze in tracking packet, capability-gated foveation centre, low-pass, synthetic gaze. <https://github.com/demonixis/oxrsys/pull/30>
- [S8] Road to VR, *Steam Frame hands-on*: foveated streaming applies to all streamed Steam content, VR or flat; Wi-Fi 6E dongle. <https://roadtovr.com/steam-frame-hands-on-valve-vr-headset-index-2>
- [S9] Community summaries of Steam Frame foveated streaming (unofficial): <https://www.resetera.com/threads/steam-frame-answering-a-lot-of-the-questions-and-misconceptions-that-people-have.1352908/page-3>, <https://en.wikipedia.org/wiki/Steam_Frame>
- [S10] UploadVR, Steam Frame announcement. <https://uploadvr.com/valve-steam-frame-official-announcement-features-details>
- [S11] NVIDIA developer forums on `qpDeltaMap` / emphasis maps. <https://forums.developer.nvidia.com/t/sdk-8-1-emphasis-map/60317>, <https://forums.developer.nvidia.com/t/qpdeltamap-in-nvenc-5-0-1-sdk-samples/39121>; `NV_ENC_RC_PARAMS` doxygen <https://ffmpeg.org/doxygen/3.4/structNV__ENC__RC__PARAMS.html>
- [S12] FFmpeg-devel, *qsvenc: Add ROI encoding support for h264_qsv and hevc_qsv*. <https://lists.ffmpeg.org/pipermail/ffmpeg-devel/2019-December/254794.html>
- [S13] FFmpeg-cvslog, *vaapi_encode: Add ROI support*. <https://lists.ffmpeg.org/pipermail/ffmpeg-cvslog/2019-July/118423.html>
- In-tree: `third-party/nv-codec-headers` (SDK 13.0.19), `third-party/build-deps/third-party/FFmpeg/AMF` (AMF SDK headers and `SimpleROI` sample), `third-party/build-deps/third-party/FFmpeg/FFmpeg` (8.1.1), `third-party/pyrowave`, `third-party/moonlight-common-c`.
