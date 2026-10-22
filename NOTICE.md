# Contributions and third-party code

BFpilot is licensed under MIT; bundled third-party projects retain
their own licenses and notices.

- **oz5hk — PR #15, RAR extraction cancellation:** the Gemini baseline already
  contains both contribution hunks: early `RARX_USERBREAK` from `UnpWrite`,
  buffered-output reset before unwinding, and the explicit cancellation message.
  The contribution is retained and is reviewed/tested locally without changing
  its GitHub PR state. See https://github.com/ItsBlurf/BFpilot/pull/15.
- The UnRAR, 7-Zip/LZMA and miniz components under `third_party/` retain their
  shipped licenses. PS5 integration changes do not replace those licenses.
- **owendswang / WFM 7-Zip Helper:** the separately built helper source under
  `third_party/wfm-7zip-helper` is based on commit
  `131ef9d75ded2f1c3b3f886a6c06dde4d5cda82f` and 7-Zip 26.03. Most code is
  LGPL-2.1-or-later, with the per-file BSD, public-domain and unRAR terms in
  its `DOC/License.txt` and `CPP/7zip/Bundles/WfmHelper/THIRD_PARTY_NOTICES.md`.
  Its MIT host application does not replace those component licenses.
- **Ronnie Sahlberg and libsmb2 contributors:** `third_party/libsmb2` is pinned
  to 6.1.0, commit `d67e213a5c4e7e4969fd81f0b95e4ca5831fbba1`, under
  LGPL-2.1-or-later and its bundled per-file terms. Keep the corresponding
  library source, local changes and complete build/relink inputs available.
  The BFpilot SMB adapter is independently authored under BFpilot's MIT license.
