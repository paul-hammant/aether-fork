# Explainer: Compiler Typecheck and Codegen Resolution for the Ash-inspired Router DSL

This document details the compile-time type-checking and code generation intricacies discovered and solved while implementing the declarative, Ash-inspired Router DSL (`std.http.server.router`) in Aether.

---

## 1. Struct Member Lookup Type Mismatches

### The Problem
During development, the JSON:API route group generator (`json_api`) accepted the resource pointer as a generic `ptr` type to align with standard Aether collection-compatible interfaces:
```aether
json_api(_ctx: ptr, res: ptr) -> ptr {
    ...
    g.prefix = string_concat("/", res.name) // error[E0200]: Type mismatch / member lookup failure
}
```
In Aether, fields are nominally typed. When a variable has the generic `ptr` type, the compiler's semantic analyzer cannot resolve dot-access member fields (like `.name`) directly on it, throwing `Undefined member` or type mismatch errors because `ptr` lacks nominal structure details.

### The Solution
We resolve this by explicitly casting the generic `ptr` to the correct Nominal Pointer type (`*Resource`) using the `as` operator before accessing any fields:
```aether
json_api(_ctx: ptr, res: ptr) -> ptr {
    router = _ctx as *Router
    g = heap.new(RouteGroup)
    g.router = router

    resource_ptr = res as *Resource
    g.prefix = string_concat("/", resource_ptr.name)
    ...
}
```
This is fully nominal, type-safe, and compile-time checked.

---

## 2. Closure Dispatch, Bare-fn Coercions, and `unbox_closure`

### The Problem
In Aether, calling/invoking a closure using `call(f, args...)` requires `f` to be of the nominal `fn` type. Under the hood, the C compiler represents `fn` as an `_AeClosure` struct containing:
- `.fn`: The C function pointer.
- `.env`: The environment block holding captured variables.

If route handlers and connection modifiers are stored in a struct field of type `ptr` (which allows raw pointer storage in lists/maps):
```aether
struct Route {
    handler: ptr
    modify_conn_fn: ptr
}
```
Attempting to invoke the pointer directly via `call(rt.handler, req, res)` causes the C compiler to fail:
```
error: request for member ‘fn’ in something not a structure or union
```
This is because Aether's `call` codegen expects a closure struct, but since `rt.handler` has the C type `void*` (ptr), it incorrectly attempts to emit member accesses (`.fn` and `.env`) directly on the `void*` pointer.

### Resolving with `unbox_closure`
To invoke a closure stored in a `ptr` field, it must first be unboxed back to the `fn` struct type using Aether's built-in `unbox_closure(p)` utility:
```aether
h_fn = unbox_closure(rt.handler)
call(h_fn, req, res)
```
This tells the Aether compiler to treat the value as a nominal `_AeClosure` struct, generating the correct closure invocation in C.

---

## 3. Coercing Bare Named Functions vs. Closures

### The Challenge
When registering routes, users write:
```aether
router.post_route("create", handle_create_author)
```
If `post_route` accepts `handler: ptr`, the compiler passes `handle_create_author` (a bare named C callback function pointer) as a raw `void*` pointer without closure wrapping (no `.env` block is created, and no heap allocation occurs).
However, if we then call `unbox_closure(rt.handler)` on this raw C function pointer in `generic_dispatch`, it attempts to read `.env` from the code address, immediately triggering a **Segmentation Fault (Signal 11)**!

### The Solution: Compiler-Enforced Autoboxing via the `fn` Type
To resolve this, we modified all builder routing functions to take parameter inputs of type `fn` (closure type) instead of `ptr`:
```aether
post_route(_ctx: ptr, action_name: string, handler: fn)
```
Because the parameter has type `fn`, Aether's compiler automatically intercepts bare-function passing at the call site and **coerces/boxes** it into a heap-allocated closure.
When this closure is then assigned to the struct's `ptr` field:
```aether
rt.handler = handler // fn -> ptr assignment
```
Aether's compiler automatically generates a heap box (`_aether_box_closure`), keeping it safe for subsequent `unbox_closure` calls. This guarantees that `rt.handler` is always a valid boxed closure, preventing Segmentation Faults and allowing seamless, transparent invocation of both raw C callbacks and capturing closures alike!
