# Server dependency pins

The core static library has no third-party runtime dependency beyond the C/POSIX
platform and math library. The HTTP adapter uses these dependencies:

| Component | Version | Integration | License |
| --- | --- | --- | --- |
| GNU libmicrohttpd | 1.0.10 | Shared system library via pkg-config, version checked at build time | LGPL-2.1-or-later |
| yyjson | 0.12.0 | Unmodified vendored source in `vendor/yyjson` | MIT |

libmicrohttpd provides the HTTP parser, an internal polling thread, streaming
responses, connection/memory limits and suspend/resume for offloading database
work. These mechanisms are described in the [official manual](https://www.gnu.org/software/libmicrohttpd/manual/libmicrohttpd.html).
Use a bounded database worker pool; the HTTP callback must not wait on durable
appends. Suspended connections must be resumed before stopping the daemon.

The installed macOS package/header and pkg-config metadata report 1.0.10. Its
Homebrew source recipe pins this upstream archive and SHA-256:

- Source: `https://ftpmirror.gnu.org/gnu/libmicrohttpd/libmicrohttpd-1.0.10.tar.gz`
- SHA-256: `04bfe8ef75db7d629a33de767599765cecadc56274a39822d5d081030d577685`

Preserve the dependency license and the ability to use a compatible shared
library when packaging the executable. Future upgrades must be deliberate and
must rerun HTTP framing, disconnect, overload, streaming and authentication tests.

The [yyjson release](https://github.com/ibireme/yyjson/releases/tag/0.12.0) provides
length-aware strings and native signed/unsigned 64-bit values. Its [allocator API](https://ibireme.github.io/yyjson/doc/doxygen/html/api.html)
allows per-request memory budgets. Use strict JSON mode, reject ambiguous
arguments and duplicate keys, preserve embedded zero bytes in data strings, and
encode large integers/timestamps as decimal strings for browser clients.
`vendor/yyjson/README.md` records the exact vendored file hashes and license.

The server object checks both pkg-config's exact version and the header's
MHD_VERSION value. `make build/libwebanalyticsdb.a` builds only the core and does
not require libmicrohttpd. Python 3 embeds the three local web assets; it adds no
runtime Python dependency. The standard build dynamically links libmicrohttpd.
