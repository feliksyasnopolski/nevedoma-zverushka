# Camera

Landscape-first, touch-first rear-camera UI for the Lenovo Tab M11 / TB330FU. Linux connects to the local camera bridge; macOS remains a deterministic UI shell with a bundled preview.

## Run and validate

```sh
flutter run -d macos
flutter analyze
flutter test
flutter test integration_test/camera_test.dart -d macos
flutter build macos
```

The macOS window opens at 1180 × 800 with a 900 × 640 minimum. The rails scroll at constrained heights to preserve touch target sizes.

## Structure and behavior

- `lib/main.dart`: app entry point and restrained dark theme.
- `lib/camera/camera_screen.dart`: manual rail, contextual control panels, preview composition, action rail and selectors.
- `lib/camera/camera_backend.dart`: local HTTP/WebSocket client and Pictures artifact handoff.
- `lib/camera/camera_state.dart`: backend-authoritative status, capture/recording state, manual values, and focus intent.
- `lib/camera/camera_preview.dart`: replaceable `MockPreviewSurface`, normalized tap coordinates and animated focus reticle.

Photo retains JPEG/DNG selection. On Linux, the shutter calls the bridge and copies the returned canonical still into `~/Pictures/RCAM_DDMMYYYY_HHMMSS.jpg` or `.dng` before another operation can overwrite the producer slot. Video start/stop uses backend recording state and copies completed MP4 output into `~/Videos/RCAM_DDMMYYYY_HHMMSS.mp4`. The app does not index or browse either directory.

AF, EV, ISO, exposure, manual focus and combined AE/AWB lock have contextual controls. AF and manual focus stay consistent. All values are simulated and do not alter the preview. The chevron collapses manual controls.

`requestFocus(x, y)` only updates a reticle, using normalized visible-preview coordinates with a top-left origin. Manual controls remain UI-only in this phase. Replace the preview surface without changing surrounding controls.

## Visual reference

The supplied composition is retained: narrow gray left rail, dominant photograph, black action rail, blue selectors, white shutter and smaller red record control. No front camera, flash, torch or zoom UI.

Intentional differences: similar generated photograph, explicit mock-preview badge in place of fabricated battery/time, Auto exposure initially, focus reticle appearing on tap, and EV adjustment in a contextual panel rather than beside the reticle. The 12 MP badge and manual ranges are illustrative mockup values, not device capability claims. The physical tablet bezel is not drawn.

`assets/mock_preview.png` is locally bundled, generated with the built-in image generation tool. Prompt: a photorealistic landscape 4:3 sunny European riverside, blue sky and clouds, historic stone buildings, arched bridge, reflective water, green/golden trees, diagonal black iron railing and classic street lamp; natural tablet-camera photograph; no frame, UI, text or reticle.

No runtime package dependencies were added; the client uses Dart `dart:io`. Flutter SDK `integration_test` is development-only. `integration_test/live_linux_test.dart` is a target-only acceptance test for a running bridge and camera stack.

## Validation (2026-09-07)

Formatting and analysis passed. Both widget tests passed, including the 900 × 612 content layout. Native macOS integration passed all Photo/Video, JPEG/DNG, preview-focus movement, manual-control selection, lock, collapse, mock shutter and recording-state assertions, including teardown. The release application built and launched, and its actual macOS screen capture was inspected for font/icon rendering and layout. No Linux deployment or backend validation is part of this milestone.
