# Graph compiler

## Purpose

The graph compiler transforms the canonical mutable Android init corpus into
the deterministic vendor-runtime graph consumed by the separate graph
executor. The graph is disposable build output. It is not source and is not
committed.

## Architecture

```text
vendor/init-corpus/mutable/
        |
Android-init parser
        |
ordered actions and services
        |
TB330FU transformations
        |
deterministic graph serializer
        |
generated/full-corpus/full-corpus-graph.json
```

The implementation is in `tools/graph-compiler/`. The parser produces an
ordered intermediate representation; the compiler transforms that IR and
serializes it as `tb330fu-full-corpus-graph-v1`.

## Inputs

The input root is `vendor/init-corpus/mutable/`. Its provenance and carve
boundary are defined by [INIT_CORPUS.md](INIT_CORPUS.md). The compiler parses
the regular files reachable from the normal-boot root and partition init
directories. It does not merge `immutable/`, parse ueventd files, or consume
property snapshots or service ledgers.

`config/graph/compiler.json` supplies the target values used for import
expansion:

| Property | Value |
|---|---|
| `ro.hardware` | `mt6768` |
| `ro.zygote` | `zygote64_32` |
| `ro.vendor.rc` | `/vendor/etc/init/hw/` |

The same configuration names the tracked stock first-stage module closure
manifest and its `H` set. No absolute workspace path or environment variable
is required.

## Parser

The parser supports actions with event and `property:name=value` triggers,
compound triggers joined by `&&`, services, service options, imports, quoted
arguments, comments, whitespace, and backslash or quoted-string line
continuations. Supported action builtins include `chmod`, `chown`,
`class_start`, `class_stop`, `copy`, `exec`, `exec_background`, `export`,
`fchown`, `fchmod`, `insmod`, `load_persist_props`, `mkdir`, `mount`,
`parse_apex_configs`, `remount_all`, `restorecon`, `restorecon_recursive`,
`rm`, `rmdir`, `setenv`, `setprop`, `start`, `stop`, `symlink`, `sysclktz`,
`trigger`, `umount`, `umount_all`, `wait`, `wait_for_prop`, `write`, and
`setrlimit`. The parser also recognizes the init builtins used by reviewed
native exclusions: `bootchart`, `enter_default_mount_ns`, `exec_start`,
`init_user0`, `installkey`, `load_exports`, `loglevel`, `mark_post_data`,
`perform_apex_config`, `remount_userdata`, and `verity_update_state`.
These builtins are recognized so reviewed native exclusions can be applied;
an occurrence without an exact reviewed exclusion remains fail-closed.

Service metadata preserves options including `class`, `disabled`, `oneshot`,
`user`, `group`, `socket`, `interface`, `onrestart`, `seclabel`, `namespace`,
`capabilities`, and the other recognized parser options. Duplicate service
declarations remain separate. Unknown syntax and builtins are retained as
diagnostics with source path and line; normal compilation fails closed on any
diagnostic.

Command argument property expansion is not a general parser feature. Import
paths use the configured parse-time property values and `${name:-default}`
defaults. Other command arguments remain tokenized source arguments.

## Import resolution

Import processing follows the AOSP r17 end-of-file rule. Local declarations
are closed before that file's imports are parsed. Directory imports expand
regular files in sorted path order. Explicit imports are exact paths and are
not globbed. Repeated imports are retained and parsed again. Missing imports
and unresolved required properties are diagnostics.

Initial file discovery follows the normal-boot order: the root
`/system/etc/init/hw/init.rc`, then sorted regular files in `/system/etc/init`,
`/system_ext/etc/init`, `/vendor/etc/init`, `/odm/etc/init`, and
`/product/etc/init`.

## Intermediate model

The IR contains ordered file and import records, ordered action records, and
ordered service records. Actions contain an event/property trigger, source
provenance, declaration order, and ordered command records. Commands contain
the builtin, string arguments, source provenance, and transformation
provenance when derived. Services contain executable/argv data, options, and
`onrestart` commands. Dependencies are implicit in action order, triggers,
imports, service metadata, and command arguments; the compiler does not infer
an additional dependency graph.

## Transformations

Transformations are canonical compiler behavior and run in this order:

1. Reviewed native-owned commands are removed.
2. The measured module-readiness wait is inserted before the first retained
   cpufreq consumer.
3. `insmod` and `modprobe` commands for modules in the stock first-stage `H`
   closure are suppressed.
4. `class_start <class>` is expanded over matching non-disabled services in
   service declaration order into derived `start` commands.

The resulting records retain source locations. Module suppressions,
class-start expansions, native exclusions, and the readiness barrier are
listed under `transformations` in the output graph.

## Module handling

[`BOOT.md`](BOOT.md) defines the early boot owner and
`manifests/stock-early-module-closure.json` is the stock-derived closure
manifest. The configured `H` set contains the 154 modules loaded by the
early native boot path. A graph `insmod` or `modprobe` command is suppressed
when its first argument basename matches a module in that set. The compiler
does not load modules, resolve new dependencies, or migrate boot-stage module
logic.

The compiler inserts one unbounded `wait_for_prop
vendor.all.modules.ready 1` immediately before the measured write of
`1800000` to
`/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq`. The condition is
published by the asynchronous `insmod_sh` loader after its `modprobe|*`
phase. An existing barrier or an ambiguous consumer is an error.

## Host-owned command filtering

Filtering is an explicit reviewed list matched by builtin, exact arguments,
and source file; the `/acct/uid` rule additionally retains an exact source
line guard. It removes the `/acct/uid` setup,
selected `/dev/memcg` setup, the reviewed APEX bootstrap/session lifecycle,
the conntrack-helper sysctl in `networksetting.rc`, and `update_verifier`.
Each removed record includes a reason and source provenance. Filtering occurs
before module suppression and class-start expansion. Ambiguous matches are not
removed. Zram setup and activation are excluded from the mutable corpus and
owned by `tb330fu-zram.service`; USB, network policy, and generic sysfs tuning
remain pending separate migrations.

## Output contract

The top-level graph contains `metadata`, `files`, `imports`, `actions`,
`services`, `unsupported`, `transformations`, and `counts`. Metadata records
the format, compiler path, source corpus name, ordering/property semantics,
dynamic import values, and a content hash. Actions and services preserve
execution order and source provenance. Property triggers preserve event and
property predicates. The executor may rely on these records and on the
transformation provenance; it must not treat the graph as canonical source.

## Determinism

File discovery, directory imports, declaration order, transformation order,
JSON key ordering, and two-space serialization are stable. The graph contains
no timestamp or host path. Identical tracked inputs and configuration produce
byte-identical output; this is verified by comparing two SHA-256 hashes.

## Build/run

From the repository root:

```bash
python3 tools/graph-compiler/compiler.py \
  --corpus vendor/init-corpus/mutable \
  --config config/graph/compiler.json \
  --out generated/full-corpus
```

The normal command exits nonzero on unsupported syntax or unresolved imports.
`--report-unsupported` is an explicit diagnostic mode that writes the graph
with its `unsupported` records and prints structural counts.

## Verification

Run the parser/compiler tests with:

```bash
python3 -m unittest discover -s tools/graph-compiler/tests -p 'test_*.py'
```

For a reproducibility check, compile with `--report-unsupported` to two
temporary output directories and compare `sha256sum` of their graph files.
The output directory is ignored by Git. The output directory is ignored by Git. Generated graphs are not hand-edited.

## Failure modes

Compilation fails for malformed tokenization or triggers, unresolved required
import properties, missing import targets, unsupported builtins or top-level
syntax, an invalid `class_start`, a missing or duplicated measured readiness
consumer, an existing readiness barrier, or an unreadable configuration or
module manifest. The compiler does not silently discard these conditions.

## Known limitations

The parser implements the documented subset, not the complete Android init
language. Command property expansion and full Android runtime semantics are
outside this compiler. The host-owned exclusion set and measured readiness
consumer are TB330FU-specific. The graph executor and boot-complete service
are separate subsystems.
