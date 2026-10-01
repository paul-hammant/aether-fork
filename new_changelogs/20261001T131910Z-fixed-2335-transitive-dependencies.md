- **A dependency's own `[dependencies]` resolve for the project using it.**
  Only the consumer's manifest was read, so `app -> pkga -> pkgb` failed with
  an unresolved import unless `app` declared `pkgb` and patched it to a path
  inside `pkga`'s checkout. Dependencies are now followed transitively; a
  dependency's `[patch]` resolves against its own root, the consumer's
  `[patch]` wins over it, and one package reached at two different
  directories is an error naming both (#2335).
