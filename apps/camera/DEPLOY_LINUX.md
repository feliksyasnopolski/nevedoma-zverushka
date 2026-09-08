# Native Linux deployment

This project is built and run natively on the Lenovo TB330FU. The source
location on the release host is `/Users/felix/tb330fu-release/apps/camera`.
The persistent development location on the tablet is
`/home/felix/apps/camera`.

## Requirements

- TB330FU running native Linux and an `aarch64` userspace
- Flutter 3.44.8 installed at `/opt/flutter`
- `clang`, `cmake`, `ninja`, `pkg-config`, and GTK 3 development files
- An active Plasma/Wayland session for launch and screenshot capture

The Linux runner files are part of this project. No source changes are needed
for the Linux target beyond the existing project files.

## Deploy the source

Run from the release host:

```bash
rsync -a --delete \
  --exclude='.dart_tool/' \
  --exclude='build/' \
  apps/camera/ nevedoma-zverushka.local:/home/felix/apps/camera/
```

Create `/home/felix/apps` on the tablet first if it does not exist:

```bash
ssh nevedoma-zverushka.local 'mkdir -p /home/felix/apps/camera'
```

The deployed source must contain `lib/main.dart`, `pubspec.yaml`, and
`linux/CMakeLists.txt`.

## Resolve and build

Run on the TB330FU:

```bash
export PATH=/opt/flutter/bin:$PATH
cd /home/felix/apps/camera
flutter --no-version-check pub get
flutter --no-version-check build linux
```

The expected native release executable is:

```text
/home/felix/apps/camera/build/linux/arm64/release/bundle/camera
```

Confirm the result is an AArch64 executable:

```bash
file /home/felix/apps/camera/build/linux/arm64/release/bundle/camera
```

## Launch from SSH

Use the real Plasma session environment:

```bash
export XDG_RUNTIME_DIR=/run/user/1000
export WAYLAND_DISPLAY=wayland-0
export QT_QPA_PLATFORM=wayland
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
cd /home/felix/apps/camera
setsid ./build/linux/arm64/release/bundle/camera \
  >/home/felix/camera-linux.log 2>&1 < /dev/null &
```

The process should remain running and display a window in the tablet's
graphical session. On Linux, the preview widget starts the native
`/dev/video-rear` consumer and renders its registered Flutter texture. Camera
control and still capture are not part of this app integration.

## Capture launch evidence

KDE Spectacle works with the tablet's compositor when invoked through the
Wayland session:

```bash
export XDG_RUNTIME_DIR=/run/user/1000
export WAYLAND_DISPLAY=wayland-0
export QT_QPA_PLATFORM=wayland
spectacle --background --nonotify --fullscreen \
  --output /home/felix/camera-linux-launch-YYYYMMDD.png
```

The resulting PNG is the visual preview evidence. Check that it shows the
rear-camera scene rather than the fallback mock asset. The native preview
bridge requests 1920x1080 and accepts NV21 or the deployed YU12 loopback
format; it converts frames to RGBA natively for `FlPixelBufferTexture`.

## Phase-1 terminal checklist

Run the app in the foreground while testing so `[camera-app]` diagnostics stay
visible in the terminal. If using the background launch above, inspect the
log afterward with:

```bash
tail -f /home/felix/camera-linux.log
```

The app copies completed still artifacts into the user-owned `Pictures`
directory and completed MP4 artifacts into `Videos`. It does not rename files
in `/run/tb330fu-camera`, because that directory is normally owned by the
camera services.

1. Confirm the target user and destination are writable:

   ```bash
   id
   mkdir -p "$HOME/Pictures"
   test -w "$HOME/Pictures" && echo PICTURES_WRITABLE
   ```

2. Start the app and wait for the live rear preview. The terminal should show
   `WebSocket connected` and `native preview frames`.

3. Select Photo → DNG and press the shutter once. Expected terminal lines:
   `HTTP POST /v1/capture?format=dng`, a `source exists` line, and
   `preserve succeeded`. Confirm:

   ```bash
   ls -l --time-style=long-iso "$HOME"/Pictures/RCAM_*.dng
   file "$HOME"/Pictures/RCAM_*.dng
   ```

4. Select Photo → JPEG and press the shutter once. Repeat the expected log
   and checks for `.jpg`.

5. Select Video, press record, wait at least five seconds, then press stop.
   Expected terminal lines include `HTTP POST /video/start`,
   `HTTP POST /video/stop`, the returned producer path, and `preserve
   succeeded`. Confirm:

   ```bash
   ls -l --time-style=long-iso "$HOME"/Videos/RCAM_*.mp4
   ffprobe -v error -show_entries stream=codec_name,width,height \
     -of default=nw=1 "$HOME"/Videos/RCAM_*.mp4
   ```

6. If preservation fails, copy the complete `[camera-app] preserve ...` block
   from the terminal. It reports the exact source path, destination path,
   source existence/size/mode, and the filesystem error. Also provide:

   ```bash
   ls -ld "$HOME" "$HOME/Pictures" /run/tb330fu-camera
   ls -l /run/tb330fu-camera/last.jpg /run/tb330fu-camera/last.dng \
     /var/lib/tb330fu-camera/video.mp4 2>&1
   ```

7. Close the app. Confirm the terminal stops producing preview frames and run:

   ```bash
   curl -sS http://127.0.0.1:3909/v1/state
   ```

   The expected backend state is `"state":"idle"`.
