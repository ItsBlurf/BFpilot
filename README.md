# BFpilot

Web-based file manager for the PS5. Runs as a single `bfpilot.elf`, accessible
from the console browser or another device on the same network.

[Downloads](https://github.com/ItsBlurf/BFpilot/releases) · [Release notes](docs/RELEASE_NOTES_NEXT.md) · [Changelog](CHANGELOG.md)

## Features

- Browse, copy, move, rename, delete and change permissions.
- Upload files and folders; download selections individually or as a TAR.
- Connect to SMB2/3 shares for file transfers, editing and PKG access.
- Extract ZIP/ZIPX, RAR, 7z and [other formats](docs/ARCHIVE_EXTRACTION.md),
  including supported encrypted and split archives.
- Queue multiple extractions, merge folders and review operation results.
- Search indexed paths, edit text files and view images.
- Load ELF/BIN/JS payloads and submit local or SMB-hosted PKGs for installation.

## Usage

Load `bfpilot.elf` through your ELF loader on port **9021**:

```sh
python3 payload_sender.py <PS5_IP> 9021 bfpilot.elf
```

Open **`http://<PS5_IP>:5905/`**. BFpilot registers a Media tile automatically;
no separate installer is needed. Reload the browser page after updating.

### Controller

| Button | Action |
| --- | --- |
| R1 | Open the highlighted item |
| Square | Toggle selection |
| X | Normal browser interaction |

R1 opens the highlighted item regardless of multi-selection.

## Compatibility

Tested on firmware **11.60**.

Direct SMB extraction is unsupported; copy the archive locally first. For SMB
installs, keep the server and source file available until installation finishes.
PKG queue acceptance does not confirm completed installation.

See [known limitations](docs/COMPATIBILITY_STRATEGY.md#known-limitations).
Keep backups of important files.

## Build

Built with [PS5 payload SDK 0.45](https://github.com/ps5-payload-dev/sdk/releases/tag/v0.45)
and LLVM 23.1.4. GNU Make and Python are also required.

```sh
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
make all
make inspect-imports
```

Windows: set `PS5_PAYLOAD_SDK` and run `build.cmd` from a configured toolchain
environment. Output: `bfpilot.elf`.

## Documentation

- [Archives](docs/ARCHIVE_EXTRACTION.md)
- [Installation and compatibility](docs/COMPATIBILITY_STRATEGY.md)
- [Temporary files and recovery](docs/PRIVATE_STAGING.md)
- [Transfers](docs/PERFORMANCE_STABILITY.md) and [upload troubleshooting](docs/UPLOAD_PERFORMANCE.md)
- [Folder-size API](docs/BACKGROUND_SIZE_API.md)

Settings and logs: `/data/BFpilot`. Binary checksums: [SHA256SUMS](SHA256SUMS).

## License

[MIT](LICENSE). See [NOTICE.md](NOTICE.md) for contributors and third-party licenses.

## Support

- [PayPal](https://paypal.me/nblurf)
- Bitcoin (BTC): `1CW8JWSnWc5w7yKhYk7ixAFG487nU5Jrzc`
- USDT (Tron / TRC20): `TRcrCeonXzgyxjR4LGzo4JdfEHYGGqD3gX`

## Disclaimer

BFpilot is independent homebrew, not affiliated with or endorsed by Sony
Interactive Entertainment. PlayStation and PS5 are trademarks of their respective
owners. Intended for lawful file management and homebrew use; piracy and
unauthorized distribution of copyrighted content are not endorsed. You are
responsible for complying with applicable laws and third-party terms and for
using only content you are authorized to access. Keep backups. Provided "as is",
without warranty; liability is limited as stated in [LICENSE](LICENSE), subject
to applicable law.
