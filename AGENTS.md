# AGENTS.md

Repository-wide rules for agent work.

## Canonical repository

The only authoritative source tree is:

```text
/Users/felix/tb330fu-release
````

Use this tree for both reading and writing.

Do not inspect, search, compare against, copy from, or derive implementation decisions from other `tb330fu` trees unless the task explicitly asks for historical comparison.

Treat other TB330FU directories as stale and potentially misleading.

## Source of truth

Before changing a subsystem, read the relevant documentation in this repository.

Subsystem documentation is authoritative for:

* architecture
* interfaces
* runtime contracts
* invariants
* validation expectations

Do not reconstruct architecture from historical code when current documentation exists.

Do not silently override documented contracts based on assumptions from similar Android/Linux projects.

## Task scope

Follow the task as written.

Do not broaden scope because adjacent work appears useful.

If the task is primarily:

* validation: validate first
* refactoring: preserve behavior unless instructed otherwise
* implementation: do not redesign unrelated interfaces
* cleanup: do not add features

Do not treat a successful build as completion when runtime validation is required.

## Working method

Prefer the simplest direct method that answers the task.

Do not invent a constraint and then build a workaround around it.

Before implementing a workaround, verify that the underlying constraint actually exists in:

* the canonical repository
* the relevant documentation
* or the live target system

If a task appears blocked:

1. identify the concrete blocker
2. collect evidence
3. try at most 2-3 genuinely different approaches
4. stop and report if still blocked

Avoid increasingly elaborate workaround chains.

## File inspection

Use normal repository/file inspection methods.

Do not implement ad-hoc file-reading mechanisms unless there is a concrete reason.

Prefer direct file reads or ordinary tools such as:

* `rg`
* `sed`
* `git`
* standard editor/repository tooling

Do not use unrelated historical files as implementation evidence.

## Build and execution environment

Use the build/deployment environment documented by the repository and the current task.

Do not invent alternate build environments unless required.

For this repository, the established build host is:

```text
entertainment.local
```

Do not change service enablement/lifecycle state merely to obtain root execution.
For manual runtime validation, invoke the target binary directly with sudo/root unless the task explicitly requires service-managed execution.

Primary access to the tablet is SSH unless the task explicitly requires another route.

Tablet possible access routes:
- ssh nevedoma-zverushka.local
- ssh 192.168.1.170
- ssh -J entertainment.local 192.168.7.2
- ssh 192.168.7.2

Do not assume ADB is the preferred access path.

## Runtime validation

When a task requires live validation, run it on the real target device.

Host-side checks such as:

* syntax validation
* compilation
* unit tests
* static analysis

are useful intermediate checks, not substitutes for target validation.

A completion report must distinguish clearly between:

* implemented
* built
* deployed
* validated

Do not report "done" when only an earlier stage is complete.

## Dependencies and libraries

Prefer maintained libraries and frameworks over reimplementing standard infrastructure.

Do not hand-roll functionality that is already adequately provided by a mature dependency unless:

* the dependency cannot be used in the target environment
* the task explicitly requires a minimal implementation
* or there is a measured reason to avoid it

Examples include:

* serialization
* HTTP servers
* WebSocket framing
* parsers
* image codecs
* protocol machinery

If avoiding an obvious library/framework, state the concrete reason.

## Repository hygiene

Keep changes inside the canonical repository.

Do not create parallel implementations in old worktrees.

Do not leave temporary experiments in production paths unless they are intentionally retained and documented.

Generated files should be reproducibly generated from their authoritative source where applicable.

Do not hand-edit generated files.

## Reporting

Completion reports should be factual and concise.

Report:

* files changed
* implementation result
* build result
* deployment result
* runtime validation result
* concrete remaining blockers

Do not present source review, compilation, or partial implementation as full completion.
