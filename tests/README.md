# Layout property test

`property_layout.c` generates 200 deterministic random control trees. Each
case contains nested panels, clip regions, scrollable panels, and conditionally
hidden subtrees. A tree is rendered for four frames; hidden subtrees alternate
between frames and mouse-wheel input is applied to the first scroll container.

Every case is evaluated twice from fresh `mu_Context` instances. The test
compares the per-frame command stream and retained container state, checks that
all API stacks return to zero, and verifies that microui does not leave any
dynamic allocation behind.

Crash isolation and shrinking use `fork(2)`. On failure the harness performs
delta debugging (removing subtrees, splicing children upward, then simplifying
parameters) and prints the smallest tree that reproduces the same failure.

```sh
make -C tests test
./tests/property_layout --cases 200 --seed 0x6d6963726f7569
```

Useful options:

```text
--cases N      number of trees (default 200)
--seed N       base seed
--no-shrink    skip delta debugging
--verbose      print every passing case
```

Set `PROP_DEBUG=1` to keep the child process's stderr visible.

The checked-in run result is in [`REPORT.md`](REPORT.md).
