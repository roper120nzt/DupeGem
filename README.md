# DupeGem

DupeGem is a duplicate-image finder for Windows. It supports five
perceptual hashes for visually similar images and MD5 for byte-identical files.
It is the fastest perceptually similar image matching tool on windows.

DupeGem was originally started because many duplicate-image finders are paid,
too slow for large libraries, or unable to handle collections of 50,000+ images.
It was built to be free, fast af, fully local, and capable of scanning huge image
collections while keeping the interface responsive.

## Download

[DOWNLOAD HERE](https://github.com/roper120nzt/DupeGem/releases/latest)

The self-contained Windows build is available from
[GitHub Releases](https://github.com/roper120nzt/DupeGem/releases/latest). Extract
`DupeGem-portable.zip` and run `DupeGem.exe`; Qt and MSYS2 do not need to be
installed on the destination computer.

## Benchmarks

Given a 39,668 image dataset

| Tool and version       | Method                         |      Time | Relative time | Successfully processed | Images/sec |     Peak memory |
| ---------------------- | ------------------------------ | --------: | ------------: | ---------------------: | ---------: | --------------: |
| **DupeGem 0.4.7**      | 256-bit dHash, distance 4      | **9m37s** |     **1.00×** |             **39,668** |  **68.71** | **1,505.7 MiB** |
| Czkawka CLI 12.0.0     | 256-bit Gradient, distance 4   |    12m21s |         1.28× |                 37,454 |      50.56 |     1,162.4 MiB |
| imagededup 0.3.3.post2 | 64-bit dHash, distance 4       |    18m22s |         1.91× |                 39,664 |      36.00 |     5,160.0 MiB |
| photodedupe 1.0.7      | Colour-difference matcher      |    24m42s |         2.57× |                 39,652 |      26.75 |     1,145.2 MiB |
| dupeGuru 4.3.1         | 15×15 colour blocks, 98% match |    72m27s |         7.53× |                 39,649 |       9.12 |     1,413.5 MiB |



## Supported image formats

DupeGem combines Qt's image readers with dedicated libheif and LibRaw decoders:

| Format | Extensions |
| --- | --- |
| Windows Bitmap | `.bmp` |
| Windows Cursor | `.cur` |
| GIF | `.gif` |
| Windows Icon | `.ico` |
| JPEG / JFIF | `.jfif`, `.jpeg`, `.jpg` |
| JPEG aliases | `.jif`, `.jpe` |
| Portable Bitmap | `.pbm` |
| Portable Graymap | `.pgm` |
| PNG / Animated PNG | `.apng`, `.png` |
| Portable Pixmap | `.ppm` |
| X Bitmap | `.xbm` |
| X Pixmap | `.xpm` |
| WebP | `.webp` |
| AVIF | `.avif` |
| HEIC / HEIF | `.heic`, `.heif`, `.hif` |
| TIFF | `.tif`, `.tiff` |
| JPEG 2000 | `.j2c`, `.j2k`, `.jp2`, `.jpc`, `.jpf`, `.jpx` |
| SVG / Compressed SVG | `.svg`, `.svgz` |
| Truevision TGA | `.tga` |
| Apple Icon Image | `.icns` |
| Multiple-image Network Graphics | `.mng` |
| Wireless Bitmap | `.wbmp` |
| Device-independent Bitmap | `.dib` |
| Adobe Digital Negative | `.dng` |
| Canon RAW | `.cr2`, `.cr3`, `.crw` |
| Nikon RAW | `.nef`, `.nrw` |
| Sony RAW | `.arw`, `.sr2`, `.srf` |
| Fujifilm RAW | `.raf` |
| Olympus / OM System RAW | `.orf` |
| Panasonic / Leica RAW | `.raw`, `.rw2`, `.rwl` |
| Pentax RAW | `.pef`, `.ptx` |
| Samsung RAW | `.srw` |
| Sigma RAW | `.x3f` |
| Other LibRaw camera formats | `.3fr`, `.ari`, `.bay`, `.cap`, `.dcr`, `.dcs`, `.drf`, `.eip`, `.erf`, `.fff`, `.gpr`, `.iiq`, `.k25`, `.kdc`, `.mdc`, `.mef`, `.mos`, `.mrw`, `.pxn`, `.r3d`, `.rwz` |

RAW hashing uses the camera's embedded preview when one is available. If a RAW
file has no usable preview, DupeGem develops it with LibRaw. This fallback is
limited to two concurrent images to prevent large RAW files from exhausting
memory.

## Exact MD5 file categories

Exact MD5 mode can scan three independently selectable categories:

- **Images** — enabled by default and includes every image format listed above.
- **Videos** — common formats including AVI, MKV, MOV, MP4, MPEG, MTS/M2TS,
  WebM, WMV, FLV, 3GP, OGV, VOB, and related extensions.
- **Other** — every readable file that is not classified as an image or video.

Any combination can be enabled. Perceptual algorithms remain image-only.
DupeGem's SQLite cache and its journal files are always excluded. MD5 content
is still read only for files sharing the same byte size, regardless of category.

Video cards display a static frame requested lazily from the Windows Explorer
thumbnail provider. This reuses Windows' thumbnail cache and adds no playback
runtime, so merely scanning large libraries does not decode every video. Preview
availability depends on the codecs and thumbnail handlers installed in Windows.

## Build prerequisites

Install Qt 6, the additional Qt image and SVG plugins, libheif, and thread-safe
LibRaw in the same MSYS2 MinGW64 environment used by `qmake6`:

```bash
pacman -S --needed mingw-w64-x86_64-qt6-base mingw-w64-x86_64-qt6-imageformats mingw-w64-x86_64-qt6-svg mingw-w64-x86_64-libheif mingw-w64-x86_64-libraw
```

The existing Notepad++ build command remains unchanged. Run `qmake6 main.pro`
again after installing the new libraries so qmake discovers them through
`pkg-config`.

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

Click a thumbnail preview to select or unselect that file. The checkbox remains
available and always reflects the same selection state.

DupeGem stores scan metadata and hashes in a `.dupegem_cache.sqlite` file inside
each scanned folder. Only the selected algorithm is calculated initially. When
you switch algorithms, its missing hashes are calculated once and persisted, so
subsequent switches and rescans reuse them. Exact MD5 also avoids reading file
contents until same-size candidates exist.

Exact MD5 is the default algorithm. The selector is ordered roughly from fastest
to slowest: MD5, aHash, dHash, wHash, bHash, then pHash. Existing cache algorithm
identifiers remain unchanged, so upgrades continue to reuse saved hashes.

Hash results are committed to SQLite every 1,000 images, including completed
work from the active batch when you press Cancel. Starting the scan again resumes
from the last checkpoint instead of recalculating those hashes.

Similarity groups use representative matching: every member must match the
group's first image at the selected threshold. This prevents long chains of
indirectly similar images from collapsing into one misleading group.

Every algorithm provides both **Delete Selected From Group** and **Delete
Selected From All Groups**. Manual checkmarks persist while you move between
groups, and the selection counter shows the current-group and library-wide
totals. Cross-group deletion confirms the number of selected files and warns
when every remaining file in a group is selected.

Exact MD5 also includes **Auto Select/Delete From All Groups** for keeping one
file from every byte-identical group and moving the rest to the Windows Recycle
Bin. The keeper can be selected by newest or oldest modified time, filename
order, or shortest/longest filename. Bulk deletion runs in the background and
can be cancelled between files.

The default decoder pool is capped at eight threads to prevent large or unusual
images from multiplying peak memory usage. Override it when benchmarking a fast
SSD and a machine with ample memory:

```powershell
$env:DUPEGEM_THREADS = 12
```

Qt image allocations are limited to 1,536 MB per decoded image so very large PNGs
can still be scanned. PNGs whose estimated decoded raster exceeds 128 MB are
automatically decoded one at a time to prevent the worker pool from multiplying
large allocations. The limit can still be adjusted when necessary:

```powershell
$env:DUPEGEM_IMAGE_LIMIT_MB = 2048
```

The cache is safe to delete whenever you want a completely fresh scan; DupeGem
will recreate it. Older `.dupegem_cache.dat` files are no longer used.
