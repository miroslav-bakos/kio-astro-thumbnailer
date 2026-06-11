# astro-thumb

A KDE Plasma 6 thumbnail plugin for astronomical image files. Adds preview thumbnails for **FITS** (`.fit`, `.fits`, `.fts`) and **XISF** (`.xisf`) files directly in Dolphin and any KIO-based file manager — no extra app needed.

## Screenshots / Demo

Open Dolphin, navigate to a folder containing `.fits` or `.xisf` files, and thumbnails appear automatically.

## How It Works

### Architecture

The plugin is a shared library (`astrothumbnail.so`) that KIO loads in-process when Dolphin asks for a thumbnail. It never forks a subprocess or calls `QProcess` — everything happens inside the thumbnailer worker.

### FITS decoding

FITS files are opened with [cfitsio](https://heasarc.gsfc.nasa.gov/fitsio/). The plugin walks the HDU list and picks the first image extension with ≥ 2 axes. It then reads pixel data with `fits_read_subset`, passing a stride so the decoded output is at most 1024 px on the longest side regardless of the original image size. This keeps memory bounded (~13 MB per request) even for large survey frames.

### XISF decoding

XISF (the native format of [PixInsight](https://pixinsight.com)) is parsed without any external library. The file starts with a fixed 16-byte signature and a little-endian XML block length, followed by the XML header. The plugin reads that header with `QXmlStreamReader`, extracts the `Image` element to find:

- pixel dimensions and channel count (`geometry` attribute)
- byte offset of the raw pixel data within the file (`location` attribute)
- sample format: `Float32`, `Float64`, `UInt16`, or `UInt8`
- pixel storage order: planar/CHW (PixInsight default) or interleaved/HWC

It then seeks directly to the pixel data and reads rows with the same stride-subsampling used for FITS.

### Stretch

Both paths feed a common stretch function:

1. **Percentile clip** — per-channel 0.5th and 99.5th percentile are computed to find black/white points.
2. **Normalise** — pixel values are linearly mapped to [0, 1] between those points.
3. **Arcsinh curve** — `asinh(x / a)` with `a = 0.6` maps the normalised value to a display value, compressing bright highlights while boosting faint detail. This is a standard technique in astronomical image processing.
4. **8-bit output** — the result is written into a `QImage` (Grayscale8 for mono, RGB888 for colour).

The final image is scaled to the requested thumbnail size with bilinear filtering before being returned to KIO.

## Supported Formats

| Extension | Format | Colour support |
|-----------|--------|---------------|
| `.fit`, `.fits`, `.fts` | FITS | Mono + RGB (3-plane) |
| `.xisf` | PixInsight XISF | Mono + RGB (up to 3 channels) |

## Tested On

| Component | Version |
|-----------|---------|
| OS | CachyOS (Arch-based) |
| KDE Plasma | 6.6.5 |
| Qt | 6.11.1 |
| KIO (KF6) | 6.26.0 |
| Extra CMake Modules | 6.26.0 |
| cfitsio | 4.6.4 |
| CMake | 4.3.3 |

It should work on any KDE Plasma 6 system with the dependencies listed below. Plasma 5 / KF5 is **not** supported.

## Dependencies

| Package | Notes |
|---------|-------|
| CMake ≥ 3.23 | Build system |
| Extra CMake Modules (ECM) | KDE build helpers |
| Qt6 Core + Gui | |
| KDE Frameworks 6 — KIO + CoreAddons | Thumbnail plugin API |
| cfitsio | FITS file reading |

### Install dependencies

**Arch / CachyOS / Manjaro**
```bash
sudo pacman -S cmake extra-cmake-modules qt6-base kio cfitsio
```

**Debian 13 / Ubuntu 24.04+**
```bash
sudo apt install cmake extra-cmake-modules qt6-base-dev \
    libkf6kio-dev libkf6coreaddons-dev libcfitsio-dev
```

**Fedora 40+**
```bash
sudo dnf install cmake extra-cmake-modules qt6-qtbase-devel \
    kf6-kio-devel kf6-kcoreaddons-devel cfitsio-devel
```

**openSUSE Tumbleweed**
```bash
sudo zypper install cmake extra-cmake-modules qt6-base-devel \
    kf6-kio-devel kf6-kcoreaddons-devel cfitsio-devel
```

## Build & Install

```bash
# 1. Clone
git clone https://github.com/miroslav-bakos/kio-astro-thumbnailer.git
cd kio-astro-thumbnailer

# 2. Configure
cmake -B build

# 3. Build
cmake --build build

# 4. Install (writes the .so into the Qt/KDE plugin directory)
sudo cmake --install build
```

### Tell KDE about the new plugin

After installing, run:
```bash
kbuildsycoca6
```

This rebuilds KDE's service database. Thumbnails should now appear automatically for astronomical image files in Dolphin. If you don't see them, try restarting Dolphin or logging out and back in.

### Uninstall

```bash
sudo cmake --build build --target uninstall
```

## Troubleshooting

**No thumbnails appear after install**

- Check that `kbuildsycoca6` was run after installing.
- Confirm the plugin was installed: `find /usr -name "astrothumbnail.so" 2>/dev/null`
- In Dolphin: Settings → Configure Dolphin → General → Previews — make sure "FITS/XISF Astronomical Image Thumbnailer" is enabled.
- Clear the thumbnail cache: `rm -rf ~/.cache/thumbnails`

**Build fails: cannot find cfitsio**

Install `cfitsio` / `libcfitsio-dev` / `cfitsio-devel` for your distro (see above).

**Build fails: Qt6 plugin directory mismatch**

On Arch the ECM default for `KDE_INSTALL_QTPLUGINDIR` sometimes points to `/usr/lib/qt/plugins` instead of the actual Qt6 path `/usr/lib/qt6/plugins`. The `CMakeLists.txt` already works around this automatically. If you still hit the issue, override manually:
```bash
cmake -B build -DKDE_INSTALL_QTPLUGINDIR=/usr/lib/qt6/plugins
```

## License

LGPL-2.0-or-later. See [LICENSES/LGPL-2.0-or-later.txt](LICENSES/LGPL-2.0-or-later.txt).

All dependencies are compatible:
- **Qt6** — LGPL 3 (dynamically linked)
- **KDE Frameworks 6** — LGPL 2.0+ (dynamically linked)
- **cfitsio** — NASA open-source permissive license (dynamically linked)

No dependency source code is bundled in this repository. Publishing this project on GitHub under LGPL-2.0-or-later raises no licensing conflicts.
