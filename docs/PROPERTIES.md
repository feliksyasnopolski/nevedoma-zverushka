# Android property compatibility

## Purpose

This subsystem supplies Android-compatible property storage and access for retained Android and vendor components below the native Linux runtime.

```text
Android/vendor property clients
    |
Android property ABI and property area
    |
Rediska implementation
    |
native Linux runtime
```

The implementation is named Rediska: a reference to Redis as a key/value store and to Russian “редиска”, a deliberately unserious mild insult.

The subsystem does not own the meaning of individual properties.
The vendor graph produces lifecycle properties and consumes property triggers.
The `tb330fu-android-boot-complete.service` readiness boundary and graph completion semantics remain graph/runtime responsibilities and are not part of this subsystem.

## Components

| Path | Role | Status |
|---|---|---|
| `src/properties/rediska_android_property_service.cpp` | Android `property_service` socket frontend and property-area writer | source |
| `src/properties/rediska.c` | Native broker for seed import, admin events, and observability | source |
| `src/properties/rediskactl.c` | Broker administrative client | source |
| `src/properties/rediska_protocol.h` | Native broker protocol definitions | source |
| `src/properties/rediska_property_info_gen.cpp` | Android r17 property-context serializer wrapper | source |
| `src/properties/rediska_property_contexts.test` | Parser/generator test input | test source |
| `src/properties/rediska_genuine_backend.c` | Creator-path validation writer | validation source |
| `src/properties/rediska_genuine_client.c` | Independent bionic reader/waiter | validation source |
| `src/properties/rediska-no-selinux-xattr.c` | Creator-only xattr compatibility preload | deployment helper |
| `systemd/rediska.service` | Native broker unit | unit |
| `systemd/rediska-genuine-property.service` | Genuine Android area and frontend unit | unit |
| `systemd/tb330fu-stock-selinux.service` | Permissive stock-policy prerequisite | unit |
| `systemd/rediska-genuine-start` | Runtime-area setup and seed wrapper | helper |

The release tree contains no generated property area, property dump, binary, vendor shared library, or stock Android image.

## Property area and startup

`rediska-genuine-property.service` requires `rediska.service` and `tb330fu-stock-selinux.service`.
It starts after both units and before `multi-user.target`.
The connectivity graph requests this unit and starts after it.
The service is independent of `tb330fu-android-boot-complete.service`.

The wrapper creates `/run/tb330fu/rediska-genuine-area`, bind-mounts it on `/dev/__properties__`, copies the externally generated `property_info`, and imports the external Android-style snapshot.
It removes the graph-owned transient entries `vendor.connsys.driver.ready`, `sys.boot_completed`, and `dev.bootcomplete` before import.
It then executes the frontend through `/apex/com.android.runtime/bin/linker64`.

The deployed external inputs and runtime paths are:

| Path | Role |
|---|---|
| `/var/lib/tb330fu/binder/android-properties.txt` | seed snapshot |
| `/usr/lib/tb330fu/rediska/property_info` | serialized metadata |
| `/usr/local/libexec/rediska-android-property-service` | frontend |
| `/run/tb330fu/rediska-genuine-area` | area backing directory |
| `/dev/__properties__` | bind-mounted Android area |
| `/dev/socket/property_service` | Android set-property socket |

The seed is imported at service start.
Runtime changes are written to the shared area only and do not update the seed or survive reboot.

## ABI and interfaces

Android clients use the target bionic `__system_property_*` ABI.
The shared area owns `prop_info` objects, property serials, and wait visibility.
The target libc exports the platform functions used by the writer:

```text
__system_property_area_init  __system_property_add
__system_property_update     __system_property_find
__system_property_serial
```

The read, get, set, and wait APIs are also provided by target libc.
Serial publication and `__system_property_wait()` wakeups therefore use the Android area implementation and futex protocol.
The native broker's condition variable protocol is not an Android area ABI.

The frontend implements the Android `setprop` v2 request shape on the stream socket.
It obtains peer credentials and checks legal names, value length, special control/persistence names, metadata type, read-only `ro.*` updates, and SELinux `property_service set` access.
The socket is root-owned, mode `0666`.
If the dynamically loaded SELinux access functions are unavailable or deny the request, the write is rejected.
SELinux is intentionally permissive in this deployment; this does not remove the source-level check.

Names and values are each shorter than 92 bytes.
The frontend rejects `ctl.*`, `sys.powerctl`, `persist.*`, and `next_boot.*`.
It does not implement Android persistence or control-action semantics.

The native broker runs with `/run/tb330fu/rediska.sock` and `/run/tb330fu/rediska-admin.sock`.
It imports `/var/lib/tb330fu/property-bootstrap/bootstrap.properties` into a separate in-memory map.
The frontend uses the admin socket only for optional `ANDROID_EVENT` observability records.
`rediskactl` is an operational admin interface, not Android public API; its `set` does not write the shared area.

## Metadata and ownership

`rediska_property_info_gen` invokes the Android 15 r17 `ParsePropertyInfoFile`/`BuildTrie` serializer.
Its context sources and serializer are external AOSP inputs.
The generated `property_info` is a deployment artifact and must be regenerated from ordered context inputs; it is not hand-edited.

| Owner | Responsibility |
|---|---|
| target libc | Android property-area layout, serials, reads, and waits |
| frontend | Android socket, validation, and area writes |
| native broker | separate in-memory admin map and event reporting |
| vendor graph | lifecycle-property meaning and graph ordering |
| native Linux services | Linux policy outside retained vendor requirements |

## Build

The native broker targets AArch64 Linux with `aarch64-linux-gnu-gcc`.
The Android frontend, creator, and reader targets use the target bionic property ABI and must be built as AArch64 programs.
The frontend is a PIE launched by the Android runtime linker; do not substitute the host libc.

Android targets require Android 15 r17 AOSP property parser and serializer sources, matching bionic `system_properties` headers, the Binder/property runtime supplied by the target deployment, and the corresponding external parser/serializer libraries.
The frontend itself does not provide a Binder implementation.

`src/properties/Makefile` accepts these source-root and library variables:

```text
AOSP_ROOT
BIONIC_INC
PROPERTY_PARSER_INC
PROPERTY_SERIALIZER_INC
ANDROID_PROPERTY_SERVICE_LDFLAGS
PROPERTY_INFO_SERIALIZER_LDFLAGS
```

Set `AOSP_ROOT` to the Android 15 r17 AOSP checkout before building Android targets:

```bash
make -C src/properties
make -C src/properties android-frontend AOSP_ROOT=/path/to/aosp15-init \
  ANDROID_PROPERTY_SERVICE_LDFLAGS='...'
make -C src/properties property-info-generator AOSP_ROOT=/path/to/aosp15-init \
  PROPERTY_INFO_SERIALIZER_LDFLAGS='...'
```

These build dependencies are external to this repository.
The policy unit requires the externally preserved stock policy at `/opt/tb330fu/android-selinux/stock-policy` and loads it permissively; the stock SELinux policy file is not redistributed here.

## Verification

Read-only deployment checks are:

```bash
systemctl is-active rediska.service rediska-genuine-property.service
ss -lxnp | grep -E '/run/tb330fu/rediska|/dev/socket/property_service'
find /dev/__properties__ -maxdepth 1 -type f -printf '%f %m %u:%g %s\n' | sort
ps -eo pid,ppid,user,args | grep '[r]edis[k]a\|property-service'
```

Expected evidence is an active genuine unit, the Android property socket, `properties_serial`, `property_info`, context area files, and a frontend command line using the Android linker.
These checks do not prove that every vendor process performs a property read or write.

The validation sources exercise the genuine creator path, independent bionic read, Add, Update, serial publication, and wait wakeup behavior.
They are test programs separate from the production deployment.

## Failure modes and recovery

The genuine unit fails if the Android linker, target libc symbols, external `property_info`, seed snapshot, or SELinux policy dependency is missing.
A malformed context trie prevents frontend startup.
A missing or incompatible area prevents Android clients from discovering the expected files.

Stopping the unit unmounts `/dev/__properties__` and removes its PID marker.
It does not reconstruct or restore another property-area implementation.
Recovery therefore requires reinstalling a known-good property deployment before retained Android/vendor clients are started.

## Known limitations

The seed is a broad external snapshot filtered for three graph-owned transient properties.
It is not proof of graph execution.
The frontend implements the set socket and shared-area ABI needed by current retained clients, not the full Android property-service feature set.
Persistence, control messages, Android SELinux domain policy, and all Android property namespaces are not claimed here.