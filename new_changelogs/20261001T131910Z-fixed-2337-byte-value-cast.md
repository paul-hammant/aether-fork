- **`byte as int` compiles in an assignment.** A value cast between `byte`
  and another numeric type was refused with E0200 ("cannot cast byte to
  int with `as`") wherever it was checked, while the same cast in a
  `return` compiled. `byte` now converts with `as` like the other numeric
  kinds: widening to `int` or `float`, narrowing as C's `unsigned char`
  (#2337).
