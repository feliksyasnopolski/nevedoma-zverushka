# TB330FU sensors

## Purpose

The stock sensor stack is Android/vendor-facing.
The TB330FU producer normalizes selected Android sensor events into standard Linux IIO devices for native Linux consumers.

## Architecture

```text
MediaTek sensor drivers and SCP
    |
Android sensor HAL: android.hardware.sensors.ISensors/default
    |  AIDL Binder + event/wake-lock FMQs
tb330fu-sensor-iio-producer
    |  32-byte feed ABI
/dev/tb330fu-sensor-feed
    |
tb330fu_iio.ko
    |
Linux IIO direct-mode devices
    |
iio-sensor-proxy
    |
desktop sensor consumers
```

Android/vendor components provide the hardware-facing sensor service.
The producer is the native Linux compatibility process.
`tb330fu_iio.ko` has no Binder or Android dependency.
`iio-sensor-proxy` owns the standard desktop sensor-facing side.

## Components

| Path | Role | Classification |
|---|---|---|
| `src/sensors/tb330fu-sensor-iio-producer.cpp` | AIDL/FMQ sensor producer | hand-written source |
| `src/sensors/tb330fu_sensor_feed.h` | producer/module feed ABI | hand-written source |
| `modules/tb330fu_iio/tb330fu_iio.c` | feed-to-IIO kernel shim | hand-written source |
| `modules/tb330fu_iio/Makefile` | external module build description | hand-written build file |
| `systemd/tb330fu-sensor-iio-module.service` | module load and IIO udev trigger | systemd unit |
| `systemd/tb330fu-sensor-iio-producer.service` | long-running producer | systemd unit |

The repository contains no TB330FU-specific udev rule.
The packaged `iio-sensor-proxy` rules recognize the standard IIO channel names.

## Vendor sensor source

The producer checks the Binder service `android.hardware.sensors.ISensors/default`, enumerates its AIDL sensor list, and selects `ACCELEROMETER`, `GYROSCOPE`, `LIGHT`, and `STEP_DETECTOR` by standard sensor type.
It uses Android event and wake-lock FMQs.
The exact MediaTek SCP transport below the Android HAL is vendor-owned and is not reimplemented here.

## Native bridge

`tb330fu-sensor-iio-producer` is a bionic AArch64 Android-ABI executable.
It opens `/dev/tb330fu-sensor-feed`, initializes the AIDL HAL/FMQ path, activates the four selected sensors, and writes fixed 32-byte samples.
Acceleration and angular velocity are converted from SI float values to integer micro-m/s2 and micro-rad/s respectively.
Light is passed as an integer lux-like value.
`STEP_DETECTOR` events are accumulated into a producer-session count.

The process exits if the feed, HAL, enumeration, FMQ initialization, or sensor activation fails.
It has no restart loop.
SIGTERM and SIGINT perform clean sensor deactivation and feed close.
Runtime library lookup requires the Android system, system extension, vendor, runtime APEX, and i18n APEX paths specified in the systemd unit.

The feed ABI is version 1 and exactly 32 bytes.
The module rejects short, oversized, unknown-version, zero-timestamp, and unknown-kind writes.
The feed device is root-only (`0600`).

## IIO module

`tb330fu_iio.ko` stores the latest sample for each selected sensor under a mutex and exposes four independent direct-mode IIO devices:

| IIO device name | Channels or attribute | Units/scale |
|---|---|---|
| `tb330fu-accelerometer` | `in_accel_x_raw`, `in_accel_y_raw`, `in_accel_z_raw`, `scale`, `mount_matrix` | micro-m/s2; scale `0.000001` |
| `tb330fu-gyroscope` | `in_anglvel_x_raw`, `in_anglvel_y_raw`, `in_anglvel_z_raw`, `scale` | micro-rad/s; scale `0.000001` |
| `tb330fu-light` | `in_illuminance_input` | integer lux-like scalar |
| `tb330fu-pedometer` | processed steps channel | absolute producer-session count |

The accelerometer mount matrix is `-1, 0, 0; 0, -1, 0; 0, 0, 1`.
IIO device numbers are dynamic and must not be used as identifiers.
The target kernel exposes direct sysfs channels but does not provide the FIFO or triggered-buffer options used by buffered consumers, so the module publishes latest values only.

## Startup and dependencies

`tb330fu-sensor-iio-module.service` loads the module from `/usr/lib/tb330fu/sensors-iio/tb330fu_iio.ko` and triggers IIO udev handling.
It is a oneshot service that remains active.
The producer is ordered after `tb330fu-android-boot-complete.service`, the module unit, and `iio-sensor-proxy.service`; it requires all three.
The producer is long-running and has `Restart=no`.

## IIO consumers

`iio-sensor-proxy` consumes the accelerometer and light IIO interfaces through the standard channel names and publishes its normal SensorProxy interface.
It provides orientation and ambient-light data to desktop consumers.
Desktop automatic rotation maintains aligned image, touch, and stylus behavior in all four physical tablet orientations.
Behavior for other desktop consumers is unverified.

## Build

Build the producer from this tree with `src/sensors/build.sh`.
It requires the external Android AIDL/FMQ headers, AIDL tool, Android NDK, Binder NDK library, and Android support libraries.
These are not copied into the release tree.

Build the module with `modules/tb330fu_iio/build.sh`.
It uses the established external stock-kernel module tree, Android clang `r510928`, reconstructed `Module.symvers`, and zstd-capable `pahole`.
`CONFIG_DEBUG_INFO_BTF_MODULES` must remain enabled.
The exact kernel vermagic, symbol CRCs, and split module BTF must match the running Android-derived kernel.
The script does not disable, strip, or bypass BTF requirements.
The kernel/BTF tree and toolchains remain external project build infrastructure.

## Installation

Install the producer as `/usr/local/libexec/tb330fu-sensor-iio-producer`, the module as `/usr/lib/tb330fu/sensors-iio/tb330fu_iio.ko`, and both units under `/etc/systemd/system/`.
This repository provides source and units, not a deployment framework.
Do not install the built `.ko` or producer into this repository's tracked source paths.

## Verification

Use names, not dynamic `iio:deviceN` numbers:

```bash
grep -H . /sys/bus/iio/devices/iio:device*/name
lsmod | grep '^tb330fu_iio\b'
systemctl is-active tb330fu-sensor-iio-module.service tb330fu-sensor-iio-producer.service iio-sensor-proxy.service
ps -eo pid,user,args | grep '[t]b330fu-sensor-iio-producer'
```

For a named device, resolve its directory from the `name` file, then inspect its channels and values.
`monitor-sensor` is an optional check when installed; it must report the packaged `iio-sensor-proxy` interface rather than a custom SensorProxy facade.

## Failure modes

- The Android sensor service is unavailable or does not enumerate the four required standard types.
- The producer cannot open `/dev/tb330fu-sensor-feed` or cannot load the Android library/FMQ dependencies.
- The module fails to load because kernel vermagic, symbol CRC, or module BTF metadata does not match.
- The feed interface is absent or rejects samples.
- IIO devices exist but their latest values stop updating.
- `iio-sensor-proxy` is unavailable or does not classify the standard channels.

## Known limitations

The Android sensor HAL and MediaTek sensor/SCP stack remain external vendor dependencies.
The producer is not a pure glibc Linux implementation.
The module provides latest-value direct sysfs channels, not buffered streaming.
Calibration data and the exact vendor transport below the Android HAL are outside this release tree.
IIO numbering is not stable across boots.