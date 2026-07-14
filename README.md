# DupeGem

DupeGem is a Qt 6 / C++17 duplicate-image finder for Windows. It supports five
perceptual hashes for visually similar images and MD5 for byte-identical files.

DupeGem was originally started because many duplicate-image finders are paid,
too slow for large libraries, or unable to handle collections of 50,000+ images.
It was built to be free, fast, fully local, and capable of scanning huge image
collections while keeping the interface responsive.

## Download

The self-contained Windows build is available from
[GitHub Releases](https://github.com/roper120nzt/DupeGem/releases/latest). Extract
`DupeGem-portable.zip` and run `DupeGem.exe`; Qt and MSYS2 do not need to be
installed on the destination computer.

## Supported image formats

The current portable build reports these formats through Qt's
`QImageReader::supportedImageFormats()`:

| Format | Extensions |
| --- | --- |
| Windows Bitmap | `.bmp` |
| Windows Cursor | `.cur` |
| GIF | `.gif` |
| Windows Icon | `.ico` |
| JPEG / JFIF | `.jfif`, `.jpeg`, `.jpg` |
| Portable Bitmap | `.pbm` |
| Portable Graymap | `.pgm` |
| PNG | `.png` |
| Portable Pixmap | `.ppm` |
| X Bitmap | `.xbm` |
| X Pixmap | `.xpm` |

HEIC/HEIF and Canon RAW files (`.cr2` and `.cr3`) are **not currently
supported** by the codecs bundled with the portable release.

## Make a portable release

After the release build succeeds, run:

```powershell
powershell -ExecutionPolicy Bypass -File .\package-portable.ps1
```

The script creates:

- `portable\DupeGem\DupeGem.exe` with the required Qt/MinGW DLLs and plugins.
- `portable\DupeGem-portable.zip` for distribution.

Windows Qt applications cannot be reduced to a literal single EXE when using
the normal dynamic MSYS2 Qt packages. That requires compiling a separate static
Qt toolchain (and observing Qt's applicable license terms). The portable ZIP is
otherwise self-contained and does not require Qt or MSYS2 on the destination PC.

## Performance

Scanning, hashing, grouping, cache updates, and preview decoding run outside the
GUI thread. The Cancel button remains available during long scans, and thumbnail
cards are created in small batches as they enter the gallery rather than creating
tens of thousands of widgets at once.

DupeGem stores scan metadata and hashes in a `.dupegem_cache.sqlite` file inside
each scanned folder. Only the selected algorithm is calculated initially. When
you switch algorithms, its missing hashes are calculated once and persisted, so
subsequent switches and rescans reuse them. Exact MD5 also avoids reading file
contents until same-size candidates exist.

Hash results are committed to SQLite every 1,000 images, including completed
work from the active batch when you press Cancel. Starting the scan again resumes
from the last checkpoint instead of recalculating those hashes.

Similarity groups use representative matching: every member must match the
group's first image at the selected threshold. This prevents long chains of
indirectly similar images from collapsing into one misleading group.

The default decoder pool is capped at eight threads to prevent large or unusual
images from multiplying peak memory usage. Override it when benchmarking a fast
SSD and a machine with ample memory:

```powershell
$env:DUPEGEM_THREADS = 12
```

Qt image allocations are limited to 128 MB per decoded image as a final guard
against corrupt or extreme files. This normally does not affect scanning because
decoders are asked for a 64×64 result. It can be adjusted when necessary:

```powershell
$env:DUPEGEM_IMAGE_LIMIT_MB = 256
```

The cache is safe to delete whenever you want a completely fresh scan; DupeGem
will recreate it. Older `.dupegem_cache.dat` files are no longer used.
