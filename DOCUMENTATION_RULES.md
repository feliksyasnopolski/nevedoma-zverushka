# Documentation rules

These rules apply to all primary TB330FU documentation.

The target reader is a competent Linux, kernel, or embedded-systems
engineer who has no prior knowledge of this project.

Primary documentation must explain the current system clearly enough that
the reader does not need to reconstruct it from implementation reports,
chat transcripts, audits, or historical experiments.

## 1. Write for humans

Use direct technical prose.

Prefer short declarative sentences.

State the fact first.

Do not add conversational filler, motivational prose, rhetorical
questions, or decorative introductions.

Do not write phrases such as:

- "Let's delve into..."
- "It is important to note..."
- "In today's..."
- "This powerful solution..."
- "As we can see..."
- "Interestingly..."
- "In conclusion..."

Do not expand a simple fact into a paragraph.

If three words are sufficient, use three words.

## 2. Describe the current system

Primary documentation describes how the system works now.

Do not narrate the sequence of experiments that produced the current
implementation.

Bad:

> Initially the device did not expose the stylus correctly. After several
> experiments we discovered that...

Good:

> The stylus requires a synthetic udev add event during coldboot.

Historical investigation belongs in archived evidence, not in the
primary documentation.

Include history only when it explains a current constraint or prevents a
known failure from being repeated.

## 3. Use precise names

Name actual:

- systemd units;
- executables;
- source paths;
- device nodes;
- kernel modules;
- sysfs paths;
- interfaces;
- protocols.

Do not infer a component's purpose from its historical filename or unit
name.

If a name is misleading, state its actual function.

## 4. Separate ownership layers

Every subsystem document must make ownership clear.

Distinguish:

- kernel/BSP responsibility;
- Android/vendor compatibility responsibility;
- native Linux service responsibility;
- desktop/session policy.

Do not describe vendor compatibility code as part of the Linux policy
layer.

Do not preserve Android policy when a standard Linux subsystem owns the
same function.

## 5. Prefer structure over repetition

A primary subsystem document should normally contain:

1. Purpose
2. Architecture
3. Components
4. Startup and dependencies
5. Runtime operation
6. Interfaces
7. Verification
8. Failure modes
9. Recovery or rollback
10. Known limitations

Omit sections that are not useful.

Do not repeat the same fact in several sections.

Use tables for mappings and inventories.

Use diagrams for control or data flow when they communicate the structure
better than prose.

## 6. Commands are procedures

Include commands only when the reader may need to execute them.

Do not paste shell transcripts as documentation.

Bad:

```text
root@tablet:~# command
lots of output
...
````

Good:

```bash
command
```

Then state the expected result.

Include only the output needed to recognize success or failure.

## 7. Logs are evidence, not prose

Do not fill primary documentation with logs.

Quote a log line only when its exact wording is useful for diagnosis.

Large traces, journals, pstore captures, strace output, and raw command
sessions belong in archived evidence.

## 8. State uncertainty accurately

Do not invent an explanation because it makes the document feel complete.

Use terms such as `unknown`, `unverified`, `expected`, or `observed` when
they are technically correct.

Use `may`, `can`, and `typically` when behavior is genuinely conditional.

Do not turn observations into guarantees.

## 9. Warnings must be specific

Do not write generic warnings such as:

> Be careful when changing this.

State what can fail and why.

Example:

> Module BTF must remain enabled. Modules built without matching split-BTF
> metadata have caused kernel panics on this device.

Place critical constraints near the procedure they affect.

Do not rely on one global warning document.

## 10. Distinguish source from generated material

Documentation must identify whether an artifact is:

* source;
* generated;
* deployed;
* stock-derived;
* proprietary;
* recovery-only;
* historical.

Do not instruct the reader to edit generated files when a canonical
source or generator exists.

## 11. Avoid artificial headings

Headings describe content.

Bad:

* The Hidden Complexity of Device Management
* Why the GPU Almost Broke Everything
* A Deep Dive into Boot

Good:

* Device node lifecycle
* Panfrost handoff
* Boot sequence

## 12. No synthetic conclusions

Do not add a conclusion that merely repeats the document.

End when the useful information ends.

Use a final section only when it contains operationally useful information,
such as known limitations, recovery instructions, or unresolved work.

## 13. Primary docs and archive have different roles

Primary documentation answers:

> How does this work now?

Archived evidence answers:

> How did we discover this?

Do not require the reader to consult archived evidence to understand the
normal system.

A subsystem should normally have one primary document.

## 14. Preserve project vocabulary

Use established project names when they are technically meaningful.

Do not invent new terminology for concepts that already have names.

Do not sanitize technical language into corporate language.

Primary documentation should remain professional and durable. Internal
notes may use a less formal register.

## 15. Optimize for information density

Remove sentences that do not add information.

Remove repeated context.

Prefer concrete nouns and verbs.

Avoid adjectives unless they distinguish technical behavior.

Avoid statements whose only purpose is to make the text sound polished.

The objective is not minimal word count.

The objective is maximum useful information per sentence without making
the document difficult to read.

## 16. Agent requirements

When an agent writes or updates documentation:

* read this file first;
* inspect current source and deployed behavior;
* use historical reports only as evidence;
* reconcile conflicting historical claims against the current system;
* do not copy old reports into new documentation;
* do not preserve chronology unless required;
* do not add generic introductory or concluding prose;
* do not fabricate missing details;
* mark unresolved facts explicitly;
* keep primary documentation self-contained.

Documentation is considered incomplete if a competent engineer must read
multiple historical reports to understand one subsystem.
