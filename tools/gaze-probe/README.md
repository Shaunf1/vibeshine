# Steam Frame gaze probe

This is a hardware check for
[`docs/plans/eye-tracked-foveated-streaming.md`](https://github.com/Shaunf1/vibeshine/pull/1).
It answers **open check 1**: can a separate program on the Steam Frame read eye
gaze through OpenXR `XR_EXT_eye_gaze_interaction` while the Flathub Moonlight
window is open and streaming? If it can, gaze source **G1** (a helper process
next to an unchanged Moonlight) works. If it cannot, the plan moves to G2 or G3.

| File | Runs on | What it does |
| --- | --- | --- |
| `gaze_probe.c` | Steam Frame | OpenXR program. It reads the gaze pose at about 90 Hz, prints it, and sends each sample as a 48-byte UDP packet. |
| `gaze_listener.py` | Streaming PC | Receives the samples and acks each one. It prints the samples, packets per second, loss and a latency estimate. Uses the Python standard library only. |
| `fake_gaze_sender.py` | PC | Pretends to be the probe, so you can test the listener without a headset. |
| `build.sh` | Steam Frame | Builds `gaze_probe` with `cc`. Downloads the four OpenXR headers once. |
| `cross-build.sh` | PC | Fallback: builds an aarch64 `gaze_probe` on the PC with podman or docker. |

The host streaming code is not changed. Phase 0 is happening in a separate
branch.

## Why a small C program

- Valve recommends native **Linux Arm64** or Android 10 builds for the Frame,
  and documents eye tracking as `XR_EXT_eye_gaze_interaction` [1].
- The Frame's SteamOS image already includes GCC, CMake, Ninja and Git.
  OpenMW-VR is built on the headset with them and renders through **SteamVR's
  native ARM64 OpenXR runtime** [2]. A single C file builds with nothing more
  than `cc`.
- SteamVR on the Frame accepts an OpenGL session through `XR_KHR_opengl_enable`
  plus `XR_MNDX_egl_enable`. OpenCE's arm64 VR mode uses exactly that pairing
  on the Frame [3]. The probe uses the same binding, but surfaceless: it opens
  no window and draws nothing.
- The probe has **no OpenXR loader dependency**. It finds the active runtime
  manifest the same way the Khronos loader does (`XR_RUNTIME_JSON`,
  `~/.config/openxr/1/`, `/etc/xdg/openxr/1/`, `/etc/openxr/1/`). It then loads
  the runtime itself through `xrNegotiateLoaderRuntimeInterface`. EGL is also
  loaded at runtime. So the build needs no `-dev` packages, which the read-only
  SteamOS image may not have.
- **Python with pyopenxr was not chosen.** pyopenxr does ship a
  `linux_aarch64` loader [4]. But the SteamVR runtime still needs a graphics
  binding, so Python would need EGL through ctypes anyway, plus pip or a venv on
  a read-only OS. That is more moving parts than one C file.

### Session types it tries (`--mode auto`)

1. **headless** (`XR_MND_headless`): no graphics and no frame loop, so it
   cannot take over the display. Only Monado is known to have it, so expect
   `headless=no` on SteamVR.
2. **overlay** (`XR_EXTX_overlay` with EGL): the runtime treats the probe as an
   overlay on top of the main app. Whether SteamVR supports this is unknown.
   The probe prints the answer.
3. **egl**: a normal session that submits **zero layers** every frame. This is
   the likely path on SteamVR. The open question is whether the runtime then
   treats the probe as the scene app and hides the Moonlight window.

The probe prints the runtime's full extension list, so a single run answers the
question of which session types the runtime supports.

## On the PC: start the listener

```powershell
# Once, so the Frame can reach the PC (Windows, admin PowerShell):
New-NetFirewallRule -DisplayName "vibeshine gaze probe" -Direction Inbound -Protocol UDP -LocalPort 48200 -Action Allow

python tools\gaze-probe\gaze_listener.py --port 48200
# Optional: --allow <frame-ip> to accept only the Frame, --quiet for STATS lines only
```

Port 48200 is used because it is outside Sunshine's 47984–48010 range. Note the
PC's LAN IP address (`ipconfig`). The Frame sends to that address.

## On the Steam Frame

### 1. Get a shell

1. On the headset: **Settings > System > Enable Developer Mode**. Then in the
   **Developer** section, choose **Set User Password** [5].
2. From the PC: `ssh steamos@frame` (or use the headset's IP address), with the
   password you just set [5]. Valve's docs also describe Remote Desktop to the
   headset's Linux desktop, with the same user and password, if you prefer a
   desktop terminal [5].

### 2. Build

```sh
cc --version && curl --version | head -1     # both should print a version
git clone --depth 1 -b sfardhalfbrick/gaze-probe https://github.com/Shaunf1/vibeshine.git ~/vibeshine-gaze
cd ~/vibeshine-gaze/tools/gaze-probe
./build.sh                                    # prints "Built .../gaze_probe"
```

**If there is no `cc` on the headset**, build on the PC and copy the result
over (podman or docker; run from Git Bash on Windows):

```sh
tools/gaze-probe/cross-build.sh               # makes tools/gaze-probe/gaze_probe (aarch64)
scp tools/gaze-probe/gaze_probe steamos@frame:~/gaze_probe
```

### 3. Run it with Moonlight open

1. Start Moonlight (Flathub) on the Frame and begin streaming from vibeshine as
   usual. Leave the stream running.
2. If the Frame has eye tracking or calibration settings, make sure eye
   tracking is on. Then put the headset on.
3. From the SSH session:

   ```sh
   ./gaze_probe <PC-IP> 48200
   ```

   Move your eyes left, right, up and down, then blink a few times. Press
   Ctrl+C to stop. Add `--quiet` to see only one `STATS` line per second.

4. Run it **once per mode** and note what happens to the Moonlight window each
   time:

   ```sh
   ./gaze_probe <PC-IP> 48200 --mode overlay   # only if the CHECK line says overlay=yes
   ./gaze_probe <PC-IP> 48200 --mode egl
   ```

Other options: `--space local` reports gaze in the room-fixed space instead of
relative to the head. `--duration 30` stops after 30 seconds.
`--rate 120` sets the sample rate in headless mode; other modes run at the
display rate.

## Reading the output

| Line | Meaning |
| --- | --- |
| `Runtime manifest:` / `Runtime library:` | Which OpenXR runtime was loaded. It should be SteamVR's. |
| `CHECK eye_gaze_interaction=yes headless=… overlay=… opengl=… egl=…` | The runtime's answers to the extension questions. |
| `Session type:` | Which session type `--mode auto` picked. |
| `CHECK supportsEyeGazeInteraction=true` | The system says it can give this app gaze. |
| `STATE IDLE → READY → SYNCHRONIZED → VISIBLE → FOCUSED` | Session lifecycle. Input, including gaze, only flows in `FOCUSED`. |
| `PROFILE /user/eyes_ext -> /interaction_profiles/ext/eye_gaze_interaction` | The runtime bound the gaze input. |
| `GAZE seq=… valid=1 dir=(x y z) yaw=… pitch=…` | One sample. `dir` is a unit vector in head space, with -Z straight ahead. Positive yaw is right and positive pitch is up. |
| `STATS rate=90.0Hz valid=97% unfocused=0% acks=90 rtt_us=…` | Once a second. `acks` shows the listener is answering. |
| PC: `STATS pps= 90.0 valid= 97% lost=0 latency~1.2ms …` | Once a second. `latency~` is half the median round trip, which needs no clock sync. `wall_one_way` is only accurate if both clocks are NTP-synced. |

### Success (G1 works)

All of these must be true:

- `CHECK supportsEyeGazeInteraction=true`, and the state reaches `FOCUSED`.
- `valid=1` most of the time, and `yaw` and `pitch` follow your eyes (roughly
  ±30°). They drop to `valid=0` briefly when you blink.
- `rate` and `pps` are about 90.
- **The Moonlight stream stays visible and keeps playing while the probe runs.**
  This is the part only you can see. Write down which `--mode` you used.

### Failure, and what it means for the plan

| What you see | Meaning |
| --- | --- |
| `FAIL: no OpenXR runtime manifest found` | SteamVR is not registered as the OpenXR runtime for this user. Find its manifest with `find / -name '*.json' -path '*openxr*' 2>/dev/null`, or look for a `steamxr*.json` under the SteamVR install, then rerun with `XR_RUNTIME_JSON=/path/to/it.json ./gaze_probe …`. |
| `FAIL: … xrCreateInstance … XR_ERROR_RUNTIME_FAILURE` over SSH | The runtime may need the desktop session's environment. Run the same command from the headset's desktop terminal (Remote Desktop) instead. |
| `CHECK eye_gaze_interaction=NO` | The runtime does not offer gaze to apps. G1 is out. Try G2 or G3. |
| `CHECK supportsEyeGazeInteraction=FALSE` | Eye tracking is off, not calibrated, or not allowed for this app. Check the headset settings, then retry. |
| State stays below `FOCUSED`, or `valid=0` the whole time | The session exists but is not given input while Moonlight has focus. G1 does not work as is. Note which mode you used. |
| Gaze works, but the Moonlight window disappears or freezes | The probe takes over the display. G1 only works if an overlay or headless session is possible. Otherwise move to G3 (Moonlight as its own OpenXR app). |
| PC shows `WAITING no packets yet` | Network or firewall problem. Check the PC IP address and the firewall rule. The probe does not need the listener to be running. |
| `pps` is about 90 but `latency=?` | Acks are not getting back to the Frame. Something is filtering return traffic. |

Paste the `CHECK`, `Session type`, `STATE` and two `STATS` lines from both
sides into the PR, and say whether the stream stayed visible in each mode.

## Packet format

The format is little-endian and is defined in `gaze_packet.h`.

- **Sample, 48 bytes:** `"GZP1"`, `u16 version=1`, `u16 flags` (bit 0 valid,
  bit 1 focused, bit 2 runtime sample time), `u32 seq`, `u32 rtt_us`,
  `u64 send_time_ns` (sender CLOCK_REALTIME), `i64 xr_sample_time`,
  `f32 dir[3]`, `f32 reserved`.
- **Ack, 16 bytes:** `"GZA1"`, `u32 seq`, `u64 echo_send_time_ns`.

The probe takes the ack's arrival time from the kernel (`SO_TIMESTAMPNS`), so
the RTT does not include the wait until the next frame.

**Security:** this is a LAN test tool. Packets are not authenticated. The real
side channel needs authentication; see risk 10 in the plan.

## Testing without a headset

```sh
python -m unittest discover -s tools/gaze-probe -p "test_*.py" -v
# or by hand, in two terminals:
python tools/gaze-probe/gaze_listener.py --port 48200
python tools/gaze-probe/fake_gaze_sender.py 127.0.0.1 48200 --duration 10 --drop-every 30 --invalid-every 10
```

The probe itself was also run against Monado's simulated headset in a Debian
container, in all three modes. This exercises runtime loading, the session
lifecycle and the UDP path end to end. The simulated headset has no eye
tracker, so `--force` is used and `valid` stays 0:

```sh
apt-get install gcc curl libc6-dev monado-service libopenxr1-monado libegl1 libegl-mesa0 libgl1-mesa-dri
export XR_RUNTIME_JSON=/usr/share/openxr/1/openxr_monado.json XRT_COMPOSITOR_NULL=1 SIMULATED_ENABLE=1
sleep infinity | monado-service &        # needs a stdin it can poll
./build.sh && ./gaze_probe 127.0.0.1 48200 --force --mode headless   # also: --mode egl, --mode overlay
```

## Sources

1. Valve, *Steam Frame: Custom Engines*: eye tracking via `XR_EXT_eye_gaze_interaction`; targets Android 10 or Linux Arm64. <https://partner.steamgames.com/doc/steamhardware/steamframe/engines/custom>
2. *openmw-vr-steam-frame*: native ARM64 build on the headset with the image's GCC, CMake, Ninja and Git; renders through SteamVR's ARM64 OpenXR runtime; SSH as `steamos` or Konsole in Desktop Mode. <https://github.com/stephen-moskal/openmw-vr-steam-frame>
3. OpenCE PR #85, *VR on the Steam Frame: native OpenXR mode for the arm64 Linux build*: `XR_KHR_opengl_enable` + `XR_MNDX_egl_enable`, tested on the Frame with SteamVR. <https://github.com/OpenCommunityEdition/OpenCE/pull/85>
4. pyopenxr bundles loaders for `linux_aarch64`, `linux_x86_64`, `windows_x86_64` and `android_arm_v8a`. <https://github.com/cmbruns/pyopenxr/tree/main/src/xr/library>
5. Valve, *Steam Frame: Debugging* and *Setup*: Developer Mode, Set User Password, `ssh steamos@frame`, Remote Desktop. <https://partner.steamgames.com/doc/steamhardware/steamframe/debugging>, <https://partner.steamgames.com/doc/steamhardware/steamframe/setup>
