- **An uncaught panic prints a message built at run time.** `panic("${x}")`
  or a heap string local outside any `try`/`catch` printed the string
  header's magic bytes instead of the text, because the no-frame fallback
  `%s`-printed the `AetherString*` codegen hands over for a catch to adopt.
  The fallback and the actor death hook now unwrap it (#2340).
