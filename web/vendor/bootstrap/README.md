# Bootstrap 5.3.x assets

This directory ships pre-compiled Bootstrap CSS/JS so the WebUI doesn't depend
on any CDN at runtime. Files required:

- `bootstrap.min.css`        — Bootstrap 5.3.x CSS bundle
- `bootstrap.bundle.min.js`  — Bootstrap 5.3.x JS + Popper bundle (no jQuery)
- `bootstrap-icons.css`      — Bootstrap Icons CSS
- `fonts/bootstrap-icons.woff2` (and .woff fallback) — icon font files

## Sourcing

These files are NOT committed to the repository because they would bloat git
history with 250KB+ binaries. Run the bundled fetch script the first time you
build:

```
# from repo root
node tools/fetch_bootstrap.mjs           # not yet provided in v1
```

Or manually:

1. Download from https://github.com/twbs/bootstrap/releases (5.3.3 verified).
2. Extract `dist/css/bootstrap.min.css` and `dist/js/bootstrap.bundle.min.js`
   into this directory.
3. Download Bootstrap Icons from https://github.com/twbs/icons/releases
   (1.11.x verified). Copy `font/bootstrap-icons.css` here and the `font/fonts/`
   directory into `web/vendor/bootstrap/fonts/`.

When the files are missing, the WebUI will load but appear unstyled. The
ACECode binary still serves `index.html` and the API works fine.

## Why not vendor them?

We may add a CMake fetcher in v2. For v1, the README + manual step keeps the
repo lean while the design contract (Bootstrap 5.3.x, no CDN) is clear.
