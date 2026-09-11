# DSP transaction experiments

`variants.json` pins four QEMU binaries. They use frozen link objects from the
installed build and the same compiler flags, changing only the C674x core.
`build-inputs.json` records the frozen objects and Blackfin hash. Private
objects and executable variants remain in `build/performance/14/`.

`clear.patch` is the first source change. `copy.patch` adds bounded commit
copies on top of it. Copy-only measurements omit the first patch; combined
measurements contain both. The `.patch` files are source evidence, not a second
installation mechanism for the repository.

Run `build.py LABEL SOURCE` to relink against the preserved local
`build/performance/14/build-recipe.json` and link-inputs. Run `micro.py` only
when no connected trial is active. Its execute timing is synthetic CPU time,
not a firmware or player result.

For one connected run use `benchmark.py --label LABEL --trial UNUSED_NUMBER`.
It runs 85 seconds with fixed MENU/encoder interactions, records CPU usage in the
shared 10–80 second window, and validates named frame hashes and input identity.
The default suite uses forward/reverse ordering. A completed trial name cannot
be reused. All runs use `DEVELOPER_DIR=/Library/Developer/CommandLineTools`.
The GUI finishes before the launcher finishes checkpoint hashing; wait for the
launcher to complete. Postprocessing CPU is excluded from the shared window.

Recompute completed outcomes with:

```sh
.venv/bin/python analysis/iterations/14-dsp-transactions/summarize.py \
  --reference-summary analysis/iterations/11-connected/prefix-connected-summary.json
```

`excluded/` retains the SIGTERM-interrupted trial; it is not an observation in
any aggregate. `replay-equivalence.py` runs after all connected measurements
and compares fixed firmware continuations across the four production core
objects. It is a cross-variant state/trace gate, not a hardware timing oracle.
