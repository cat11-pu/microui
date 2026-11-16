# Property test report

Command:

```sh
make -C tests test
```

Environment: GCC 15.2, x86-64 Linux, `-O2`.

## Summary

```text
cases:    200
passed:   200
failed:   0
layout:   800 frames, 71159 commands
features: panel=2304 clip=2320 scroll=2321 hidden=2311
controls: label=4350 button=4488 checkbox=4408 slider=4532 textbox=4502
```

Every case was rendered twice from fresh contexts. Both runs produced identical
per-frame command and container-state hashes. No API stack remained unbalanced,
and microui made no dynamic allocation.

## Counterexamples

None for the requested properties.

An additional UBSan run found that `mu_push_command()` casts the character
command buffer to `mu_Command`, which is under-aligned for the union's pointer
members. x86-64 tolerates this and the native/ASan runs pass, but strict
alignment builds report:

```text
src/microui.c:430:18: runtime error: member access within misaligned address
for type 'union mu_Command', which requires 8 byte alignment
```

The delta-debugged reproducer is one window:

```text
window "root" rect=(12,16,500,260) opt=NOSCROLL
```

This is a portability issue rather than a failure of the requested native-run
properties.
