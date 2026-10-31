- **`contrib.host.python` gains the #904 bidirectional embedding tier.**
  Aether programs embedding CPython can now create persistent host-owned VMs
  (`python.vm_new`), register Aether callbacks callable from scripts
  (`python.vm_register`, with dotted namespace support such as `"app.math.add"`),
  inject globals (`python.vm_set_global_str`, `_int`, `_strlist`), evaluate code
  with expression-first fallback, read typed results (`python.vm_result_type`,
  `_int`, `_str`, `_bool`, `_double`, `_bytes`), and set instruction limits via
  `PyEval_SetTrace`. Callbacks can read 1-based typed arguments (`python.arg_*`),
  push results (`python.push_*`), and raise script-side exceptions
  (`python.raise_error`). Includes Python-specific length-aware binary safety for
  `bytes` (`PY_TBYTES`), float tagging (`PY_TFLOAT`), 64-bit integer overflow
  tagging as `PY_TOTHER` (reading 0 via `_int`), `bool` resolution preceding `int`,
  full thread safety across scheduler actors using the GIL, and per-call state
  preservation across nested evaluations.
