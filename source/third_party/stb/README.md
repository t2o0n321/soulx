# stb (vendored)

Single-file public-domain / MIT libraries by Sean Barrett, used by Soul's Lua
script engine (`source/script/image.c`) to read and write PNG files.

| file                | version | upstream path                                   |
|---------------------|---------|-------------------------------------------------|
| `stb_image.h`       | v2.30   | https://github.com/nothings/stb/blob/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_image.h |
| `stb_image_write.h` | v1.16   | https://github.com/nothings/stb/blob/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_image_write.h |
| `LICENSE`           | -       | https://github.com/nothings/stb/blob/2c980bb59875b0d32144a71867fbdebb2f77cd20/LICENSE |

- Repository: https://github.com/nothings/stb
- Pinned commit: `2c980bb59875b0d32144a71867fbdebb2f77cd20` (master, 2026-08-02)
- Fetched from `https://raw.githubusercontent.com/nothings/stb/<commit>/<file>` on 2026-10-12.
- SHA-256 of the vendored files:
  - `stb_image.h`       594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3
  - `stb_image_write.h` cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05
  - `LICENSE`           bebfe904b14301657e4e5d655c811d51fd31b97c455b9cc2d8600d6bac6cff63

The files are unmodified. The implementations are compiled exactly once, in
`source/script/image.c` (`STB_IMAGE_IMPLEMENTATION`, `STB_IMAGE_WRITE_IMPLEMENTATION`,
decoders restricted to PNG and JPEG with `STBI_ONLY_PNG` / `STBI_ONLY_JPEG`).
Include this directory with `-isystem` so the headers' own warnings stay quiet.

## License

Both libraries are dual-licensed: MIT (Copyright (c) 2017 Sean Barrett) or
public domain (Unlicense), at your choice. The full text is in `LICENSE` and
at the end of each header.
