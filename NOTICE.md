# Contributions and third-party code

BFpilot is licensed under GPL-3.0-or-later; bundled third-party projects retain
their own licenses and notices.

- **oz5hk — PR #15, RAR extraction cancellation:** the Gemini baseline already
  contains both contribution hunks: early `RARX_USERBREAK` from `UnpWrite`,
  buffered-output reset before unwinding, and the explicit cancellation message.
  The contribution is retained and is reviewed/tested locally without changing
  its GitHub PR state. See https://github.com/ItsBlurf/BFpilot/pull/15.
- The UnRAR, 7-Zip/LZMA and miniz components under `third_party/` retain their
  shipped licenses. PS5 integration changes do not replace those licenses.
