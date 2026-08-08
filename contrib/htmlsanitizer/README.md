# HtmlSanitizer for Aether

A pure-Aether HTML sanitizer designed to clean HTML documents and fragments from constructs that can lead to Cross-Site Scripting (XSS) attacks.

This module is a complete port of the popular Michael Ganss XSS `HtmlSanitizer` library from C#, adapted specifically for the ergonomics, safety, and manual memory discipline of Aether.

## Credits & Copyright

Portions copyright (c) 2013-2016 Michael Ganss and original C# HtmlSanitizer contributors.

## Features

- **Lenient HTML5 Tokenizer and DOM Parser**: Safely parses malformed/half-open HTML, Void tags, comments, CDATA, and switch raw-text scanning modes for sensitive elements like `<script>` or `<style>`.
- **CSS Inline-Style Parser**: Safely parses, filters, and sanitizes style attributes, custom CSS properties, and handles backslash hex-escape normalization.
- **RFC-compliant URL Resolver**: Resolves relative/absolute/protocol-relative URLs against a `base_url`.
- **Defaults and Customization**: Pre-configured with secure, extensive allowed lists for tags, attributes, CSS properties, schemes, and classes.
- **Hooks and Callbacks**: Exposes event-like callbacks (e.g. `on_removing_tag`, `on_removing_attribute`, `on_removing_comment`, `on_filter_url`, etc.) to control and customize the sanitization process.

## Usage

```aether
import contrib.htmlsanitizer

main() {
    s = htmlsanitizer.new()

    // Customize allowed tags/attributes
    set.add(s.allowed_tags, "my-custom-tag")

    // Sanitize an HTML fragment
    html = "<div>Hello <script>alert(1)</script> <a href='/page'>world</a>!</div>"
    sanitized = htmlsanitizer.sanitize(s, html, "https://example.com")
    println(sanitized) // "<div>Hello  <a href=\"https://example.com/page\">world</a>!</div>"

    htmlsanitizer.free(s)
}
```

## Memory Management Notes

Aether uses reference-counting and automatic scope-exit auto-cleanup for local variables. For complex structures like the parser's DOM tree, memory is allocated via `heap.new` to support dynamic, recursive DOM structures.

### LeakSanitizer / ASAN Behavior

In pure Aether, `heap.free` on a `ptr` releases the structure allocation, but because the structure's type information is erased at the raw C heap level, the nested reference-counted `string` fields on those heap-boxed structs (like `DomNode.name`/`DomNode.value` and `DomAttr.name`/`DomAttr.value`) are not automatically decremented. This can result in minor memory leak reports from LeakSanitizer / ASAN at process exit when cleaning up a parsed DOM tree.

To achieve 100% leak-free ASAN runs, any dynamic string fields can be explicitly reassigned to `""` (e.g., `nd.name = ""`, `nd.value = ""`) before calling `heap.free`, triggering Aether's automatic string-reassignment cleanup.
