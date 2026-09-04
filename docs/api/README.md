# docs/api/

Generated API reference documentation (Doxygen output).

This directory is populated only when `NIMRTC_BUILD_DOCS=ON` is set at
configure time and Doxygen is found on the system.

```
doxygen Doxyfile
```

The generated HTML lands in `docs/api/html/`.

## Public API stability

Only headers under `src/core/include/nimrtc/` and
`src/modules/*/include/nimrtc/` are included in the Doxygen input set.
Internal headers (those under `src/`) are excluded via `EXCLUDE_PATTERNS`.
