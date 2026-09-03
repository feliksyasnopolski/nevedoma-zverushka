# TB330FU audio

## Purpose

The native audio path exposes the MT6768/MT6358 sound card to ordinary Linux applications through ALSA, PipeWire, and WirePlumber.
The kernel and vendor BSP provide the MediaTek ASoC implementation.
Native Linux owns route initialization and desktop audio policy.

Android AudioFlinger, an Android Audio HAL, bionic audio daemon, and Binder audio service are not part of the working playback or capture path.
Other vendor-runtime services may remain required by unrelated hardware; see [VENDOR_RUNTIME.md](VENDOR_RUNTIME.md).

## Architecture

```text
MediaTek kernel/BSP
    -> MT6768/MT6358 ASoC drivers
    -> ALSA card mt6768-mt6358
    -> UCM2 routes and PCM endpoints
    -> PipeWire ALSA monitor
    -> WirePlumber
    -> Plasma/Linux applications
```

## Components

| Path | Role |
|---|---|
| `src/audio/tb330fu-load-audio-platform` | Loads the audio-specific platform provider outside the shared graph closure. |
| `src/audio/tb330fu-load-audio` | Loads the audio ASoC tail and waits for card registration. |
| `src/audio/lenovo-audio-init` | Applies speaker, headphone, and primary analog-microphone mixer routes. |
| `systemd/tb330fu-audio-*.service` | Orders platform preparation, modules, and route initialization. |
| `alsa/ucm2/` | Project-authored UCM2 master, driver lookup alias, and HiFi devices. |
| `wireplumber/51-lenovo-m11-audio.conf` | TB330FU-specific ALSA monitor and Playback_3 node properties. |

The stock vendor modules and firmware are deployment prerequisites and are not copied into this repository.
Device nodes are owned by `tb330fu-devd`, as described in [DEVD.md](DEVD.md).

## Kernel and startup prerequisites

The shared MTK/BSP module closure is owned by `tb330fu-connectivity-graph.service`; this audio integration does not duplicate it.
The audio-specific platform unit loads `nvmem-mt635x-efuse.ko`.

The module unit then loads, in order:

```text
mt6358-accdet.ko
mtk-sp-spk-amp.ko
snd-soc-aw883xx.ko
snd-soc-mt6660.ko
mtk-btcvsd.ko
mtk-afe-external.ko
snd-soc-mtk-common.ko
snd-soc-mt6768-afe.ko
snd-soc-mt6358.ko
mt6768-mt6358.ko
```

`snd-soc-mt6660.ko` may fail to initialize because it is not in the active topology; the loader continues.
Other failures are fatal.
Already-loaded modules are accepted.
The loader waits up to 20 seconds for the `mt6768mt6358` ALSA card identity.

The platform unit requires `tb330fu-android-boot-complete.service`.
The module unit follows it and precedes `tb330fu-devnodes.service` and `sound.target`.
The route unit follows the module and devnodes units.
All three units are oneshot services with `RemainAfterExit=yes` and are enabled at `multi-user.target` in the deployed integration.

The module files are stock vendor modules from `/vendor/lib/modules`.
No project-built audio kernel module, BTF requirement, or audio firmware build is present in this source tree.
Boot and vendor module ownership is described in [BOOT.md](BOOT.md).

## ALSA card and routes

The stable card identity is `mt6768-mt6358` with ALSA ID `mt6768mt6358`.
Numeric card indices are not an interface contract.

| Function | PCM / UCM device | Route |
|---|---|---|
| Speaker playback | `hw:mt6768mt6358,3` / `Speaker` | DL3 to I2S1 with `Low_Jitter`; headphone and alternate DL paths are disabled. |
| Analog headphone playback | `hw:mt6768mt6358,3` / `Headphones` | DL3 to HPL/HPR audio-playback muxes; `Headset Volume` is `9,9`. |
| Primary analog capture | `hw:mt6768mt6358,4` / `Mic` | ACC input, left/right preamplifiers, AIN0 PGAs, and UL1 links. |

The card also exposes other capture PCMs.
They are not part of the promoted route contract.
Playback is stereo S16 at 48 kHz in the validated direct ALSA path.
The UCM devices declare two playback and two capture channels.

## Mixer initialization and UCM

`tb330fu-audio-route.service` runs `lenovo-audio-init` with its default speaker mode.
The helper also enables the primary analog capture path.
Its `headphone` and `capture` modes are explicit operations for route handling and diagnostics.
The speaker and headphone operations first disable the mutually exclusive paths and are safe to repeat.

The UCM2 profile is project-authored from the validated board route.
Its `HiFi` verb provides `Speaker`, `Headphones`, and `Mic` devices.
Speaker and headphone devices conflict, both use Playback_3, and the headphone device declares the `Headset Jack` control.
The driver-derived lookup file under `alsa/ucm2/conf.d/mt6768-mt6358/` is a symlink to the single master profile; it is required because the ALSA card driver and card ID use different names.

UCM is declarative route description.
The route oneshot remains because the current boot path requires a known mixer baseline before desktop audio policy.
This promotion does not consolidate the helper and UCM sequences.

## PipeWire, WirePlumber, and Plasma

The target uses distribution PipeWire, PipeWire-Pulse, and WirePlumber packages.
This repository contains only the TB330FU-specific WirePlumber configuration.
It enables ACP/UCM discovery, soft mixer behavior, automatic profile/port selection, and S16LE/48 kHz properties for the Playback_3 sink.
No generic distro PipeWire configuration is copied.

Plasma uses the standard PipeWire desktop controls.
There is no project- specific KDE audio integration.

The UCM profile is accepted by alsaucm; the driver-derived lookup symlink matches the observed ALSA driver/card naming.
The retained integration report did not establish a stable PipeWire client session on the headless boot, so PipeWire playback/capture through a user session remains unverified.
Do not treat package installation or ALSA enumeration as PipeWire client validation.

## Jack detection

The source workspace contains a Python helper that reads blocking `EV_SW` events from the input device named `mt6768-mt6358 Headset Jack`, reconciles the initial switch bitmap, and invokes the route helper.
It is not promoted as a current service: automatic UCM/WirePlumber switching was not proven and the deployed helper was left disabled.
No udev or extcon rule is required by the current validated route.

## Verification

These checks are read-only:

```bash
cat /proc/asound/cards
aplay -l
arecord -l
alsaucm -c mt6768mt6358 dump text
systemctl status tb330fu-audio-platform.service tb330fu-audio-modules.service tb330fu-audio-route.service
systemctl --user status pipewire pipewire-pulse wireplumber
wpctl status
```

Validated behavior includes a registered card, operator-confirmed speaker playback, and finite nonconstant data from the primary analog capture PCM.

## Failure behavior and limitations

Missing vendor modules, a failed required `insmod`, or failure to register the card fails the module unit.
A missing mixer control fails the route unit.
The jack helper's missing input source is not a current service recovery case because that helper is not deployed.
User-session PipeWire recovery is owned by the normal desktop session and was not redesigned here.

No audio component in this directory requires compilation.
The promoted helpers are shell scripts and the remaining files are systemd, UCM, and WirePlumber configuration.