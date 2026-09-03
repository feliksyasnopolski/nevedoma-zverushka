# Vendor runtime graph executor

## Purpose

The graph executor runs the compiler-produced Android-init subset that is retained for hardware support beneath native Linux.
It owns graph action ordering, property-trigger evaluation, filesystem/device actions, and the process lifetime of retained vendor services.
It is not a general Android init replacement and it does not own native Linux policy.

## Architecture

```text
vendor/init-corpus/mutable/
        |
tools/graph-compiler/compiler.py
        |
generated/full-corpus/full-corpus-graph.json
        |
tb330fu-graph-executor
        +-- filesystem, sysfs, module, and property actions
        +-- retained Android/vendor services
        |
runtime properties: sys.boot_completed, dev.bootcomplete, vendor.*
        |
tb330fu-android-boot-complete.service
        |
native Linux consumers
```

The graph is generated, disposable input.
It is not tracked or hand-edited.
The producer contract is [GRAPH_COMPILER.md](GRAPH_COMPILER.md).
Rediska is the property backend; its ABI and ownership are described in [PROPERTIES.md](PROPERTIES.md).

## Components

| Path | Role |
|---|---|
| `src/graph-executor/graph_executor.cpp` | graph loader, dispatcher, process and service runtime |
| `src/graph-executor/json.h`, `json/`, `jsoncpp.cpp` | vendored JSON parser used by the loader |
| `src/properties/rediska_protocol.h` | Rediska native wire-protocol interface |
| `src/graph-executor/wait_android_property.c` | target-bionic property waiter used by the readiness unit |
| `src/graph-executor/tb330fu-connectivity-finalize` | narrow `/dev/wmtWifi` post-graph action |
| `systemd/tb330fu-connectivity-graph.service` | resident executor unit; historical connectivity name |
| `systemd/tb330fu-connectivity-finalize.service` | post-readiness Wi-Fi finalizer unit |
| `systemd/tb330fu-android-boot-complete.service` | `sys.boot_completed=1` wait gate |

## Generated graph input

The loader requires `tb330fu-full-corpus-graph-v1`.
Required top-level records are `metadata`, `files`, `imports`, `actions`, `services`, `unsupported`, `transformations`, and `counts`.
Required metadata includes `format`, `compiler`, `source_corpus`, `ordering`, and `property_semantics`.
Malformed JSON, missing records, invalid action/service records, unsupported builtins, and invalid command shapes fail before runtime execution.
The loader does not accept the older connectivity graph format.

The installed default is `/usr/lib/tb330fu/graph/full-corpus-graph.json`.
`--graph PATH` and `TB330FU_GRAPH` select another graph.
The Rediska socket defaults to `/run/tb330fu/rediska.sock` and can be overridden with `TB330FU_REDISKA_SOCKET`.

## Startup and lifecycle

`tb330fu-connectivity-graph.service` starts after `rediska-genuine-property.service` and `tb330fu-devd.service`.
It dispatches these synthetic executor entry points, in order:

```text
early-init -> init -> late-init -> boot -> nonencrypted
```

Each event is dispatched once by the unit.
A `trigger EVENT` command appends a new event to a FIFO queue.
Directly queued actions run before queued trigger events.
Repeated triggers are not coalesced.
An action runs in declaration order; its commands run in source order.
The executor remains resident after the initial traversal and observes Rediska property serial changes.

## Actions, properties, and triggers

Event and property predicates are ANDed.
A property action matches an existing property value; wildcard predicates require a non-empty existing value.
Initial property state is evaluated after the initial event traversal.
Later matching property actions are latched until their predicates become false, so a stable value does not repeatedly fire on every observation.
`setprop` notifications are evaluated immediately, including same-value writes.
Property expansion supports `${name}` and `${name:-default}` through Rediska reads.

The executor reads and writes properties through the native Rediska socket.
It uses `GET`, `SET`, `HELLO` for the global serial, and `WAIT_ANY` for reactive observation.
`wait_for_prop` polls the Rediska value at 100 ms intervals: ordinary waits are bounded at five seconds; the compiler-inserted `vendor.all.modules.ready=1` barrier is explicitly unbounded.
A failed Rediska startup handshake or `WAIT_ANY` operation terminates the executor.

## Command subset

Commands emitted by the current full-corpus compiler have these dispositions:

| Disposition | Builtins |
|---|---|
| Execute | `chmod`, `chown`, `class_start`, `class_stop`, `class_reset`, `class_restart`, `copy`, `copy_per_line`, `exec`, `exec_background`, `exec_start`, `enable`, `export`, `insmod`, `load_exports`, `mkdir`, `mount`, `restart`, `restorecon`, `restorecon_recursive`, `rm`, `rmdir`, `setprop`, `setrlimit`, `start`, `stop`, `symlink`, `sysclktz`, `trigger`, `umount`, `wait`, `wait_for_prop`, `write` |
| Explicit adapter/no-op | `interface_start`, `interface_stop`, `interface_restart`; these log dispatch and return success without an interface manager |
| Hard failure as subordinate graph input | `bootchart`, `domainname`, `enter_default_mount_ns`, `ifup`, `init_user0`, `installkey`, `load_persist_props`, `load_system_props`, `loglevel`, `mark_post_data`, `mount_all`, `perform_apex_config`, `powerctl`, `remount_userdata`, `swapon_all`, `umount_all`, `update_linker_config`, `verity_update_state`, `hostname`, `switch_root` |

The parser/compiler may recognize a larger Android-init vocabulary for fail-closed carving.
Recognition is not executor support.
The executor does not silently discard an emitted command.

Filesystem paths are rooted at the process root, which is empty in the production unit.
`mkdir` tolerates `EEXIST`; `chmod` and `chown` defer an `ENOENT` operation for a later metadata pass.
`write`, `copy`, `mount`, `umount`, `rm`, `rmdir`, and `symlink` use fatal command results for their own action logging; there is no general retry.
`restorecon` is a logged policy evaluation and does not apply SELinux labels in this runtime.

## Services and process execution

Service records provide an executable and argv.
Property expansion is applied to service argv at start.
The child inherits the executor environment, including the Android-first `PATH` supplied by the systemd unit.
`native_cwd` is honored when present.
Resolved user, primary group, and supplementary groups are applied before `execv`.

`disabled`, `oneshot`, classes, `start`, `stop`, `restart`, `onrestart`, and `exec_start` are tracked.
Non-oneshot children return to stopped state after reaping; oneshot children become exited.
A SIGCHLD handler and `waitpid` reap children.
An execution-error pipe distinguishes `execv` failure from a child that started and later exited.
PID reuse protection is limited to the tracked PID association; there is no separate pidfd model.

Service sockets are created under `/dev/socket`, with the declared Unix type, mode, uid, and gid.
A live collision is rejected; a stale path is removed.
The child receives `ANDROID_SOCKET_<name>` containing the inherited descriptor.
SELinux labels, Linux namespaces, capabilities, interface registration, and task profiles are not applied by this executor even when corresponding metadata is preserved by the compiler.
These are current compatibility limitations, not claims of Android-init equivalence.

`exec` waits synchronously for completion.
`exec_background` forks and returns immediately; its child is reaped and logged asynchronously.
Service starts are asynchronous except for `exec_start`.

## Vendor process environment

The executor launches absolute executable paths with `execv`; it does not select an Android linker or synthesize an APEX library namespace.
Retained vendor programs therefore require their deployed Android filesystem paths, the target bionic/linker arrangement, inherited `PATH` for shell scripts, the Rediska property area, and any Binder, device-node, module, firmware, or socket prerequisites they declare.
These substrate services are external dependencies.
The executor does not provide Binder or Android SELinux domains.

## Failure policy and observability

Graph-load and structural validation errors abort startup.
Most command failures are logged with action, builtin, arguments, and source provenance and execution continues with the next command.
Foreground child failures are returned to the action but do not globally abort the action queue.
Service spawn failures are logged; child exits are logged when reaped.
Property backend failure during the direct runtime property path marks the runtime failed; a rejected `setprop` command is logged by the command handler and continues.

The executor writes human-readable diagnostics to stderr, captured by systemd's journal.
`TB330FU_TRACE_PATH` enables JSONL tracing, including graph load, event/action/command boundaries, property changes, waits, child spawn, and child exit records.
The default trace path is `/run/tb330fu-graph-executor.jsonl`; it is runtime output and is not tracked.

## Readiness

Graph process state is not Android runtime readiness.
The graph action in `product/etc/init/zz-tb330fu-boot-complete.rc` publishes `sys.boot_completed=1` and `dev.bootcomplete=1`.
The readiness unit only waits for `sys.boot_completed=1` through the target-bionic property ABI; it does not set either property and it does not equate executor exit with readiness.

The chain is:

```text
graph actions execute
    -> retained runtime publishes boot-complete properties
    -> wait-android-property observes sys.boot_completed=1
    -> native services ordered after tb330fu-android-boot-complete.service
```

The property seed removes these graph-owned transient properties before import, so a seed snapshot is not graph-completion evidence.
See [PROPERTIES.md](PROPERTIES.md) for the property-area implementation.

## Systemd integration

`tb330fu-connectivity-graph.service` is a resident `Type=simple` executor.
Its historical name reflects the original deployment and does not limit the current full-corpus graph ownership.
`tb330fu-connectivity-finalize.service` is a separate oneshot that runs after the readiness gate and writes one ASCII `1` to `/dev/wmtWifi`.
It is a post-graph hardware action, not a compiler action and not Rediska ownership.

The promoted graph units require only release-tree units that exist: Rediska property service, `tb330fu-devd.service`, and the readiness unit.
The native USB management gadget is now provided separately by `tb330fu-usb-gadget.service`; it is not a graph dependency.
The graph still contains known mixed-ownership actions such as Android USB modes, network, and generic sysfs policy.
Zram is excluded from the graph and owned by `tb330fu-zram.service`; see [ZRAM.md](ZRAM.md).
See [USB.md](USB.md) for the native USB boundary.

## Build and verification

The executor is an AArch64 Linux target.
Its small Makefile has no workspace, tablet, or build-host path requirement; set `CXX` to a C++20 AArch64 Linux compiler when the default cross compiler is unavailable:

```bash
make -C src/graph-executor
make -C src/graph-executor validate
make -C src/graph-executor test
```

`validate` consumes the small tracked schema fixture.
`test` additionally checks that an incompatible graph format is rejected.
Full-corpus validation uses a generated graph and does not start services:

```bash
src/graph-executor/build/tb330fu-graph-executor \
  --validate --graph generated/full-corpus/full-corpus-graph.json
```

Install the executor, graph, waiter, finalizer, and units using the target deployment procedure.
Do not run `--run` on a development host: it performs filesystem, property, module, and process actions.

## Known limitations

The runtime implements the reviewed compiler subset, not Android init.
It does not apply service capabilities, namespaces, SELinux labels, Android task profiles, or a bionic linker namespace.
The `interface_*` commands are adapter no-ops.
Command failures generally do not stop later commands, and the executor has no restart policy independent of systemd.
Connectivity and finalizer filenames remain historical until a broader unit migration is approved.