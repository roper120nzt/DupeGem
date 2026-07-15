# DupeGem

DupeGem is a Qt 6 / C++17 duplicate-image finder for Windows. It supports five
perceptual hashes for visually similar images and a staged XXH3/BLAKE3 pipeline
for byte-identical files.

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

DupeGem combines Qt's image readers with dedicated libheif and LibRaw decoders:

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
| HEIC / HEIF | `.heic`, `.heif`, `.hif` |
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

## Exact-file categories

Exact Files mode can scan three independently selectable categories:

- **Images** — enabled by default and includes every image format listed above.
- **Videos** — common formats including AVI, MKV, MOV, MP4, MPEG, MTS/M2TS,
  WebM, WMV, FLV, 3GP, OGV, VOB, and related extensions.
- **Other** — every readable file that is not classified as an image or video.

Any combination can be enabled. Perceptual algorithms remain image-only.
DupeGem's SQLite cache and its journal files are always excluded. File contents
are read only when another selected file has the same byte size.

Video cards display a static frame requested lazily from the Windows Explorer
thumbnail provider. This reuses Windows' thumbnail cache and adds no playback
runtime, so merely scanning large libraries does not decode every video. Preview
availability depends on the codecs and thumbnail handlers installed in Windows.

## Build prerequisites

Install Qt 6, libheif, thread-safe LibRaw, xxHash, and libjpeg-turbo in the same MSYS2 MinGW64
environment used by `qmake6`:

```bash
pacman -S --needed mingw-w64-x86_64-qt6-base mingw-w64-x86_64-libheif mingw-w64-x86_64-libraw mingw-w64-x86_64-xxhash mingw-w64-x86_64-libjpeg-turbo
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
- `portable\DupeGem-v0.4.0-portable.zip` for distribution.

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
each scanned folder. By default, one 64×64 grayscale decode calculates and saves
dHash, aHash, pHash, wHash, and bHash in one pass. Switching algorithms is then
immediate. **Cache all hashes** can be disabled for a lower-power, selected-only
scan.

Exact Files is the default algorithm. It groups by size, removes hardlinks using
Windows file IDs, checks the first/final 16 KiB with XXH3, then checks 16 KiB at
25/50/75% only for survivors. Full SIMD-dispatched BLAKE3 runs only after both
sample gates collide. Automatic bulk deletion does
a final byte-for-byte comparison before moving anything to the Recycle Bin.
The selector is ordered roughly from fastest to slowest: Exact Files, aHash,
dHash, wHash, bHash, then pHash. The internal legacy algorithm identifier remains
unchanged so older cache databases and command-line callers stay compatible.

Hash results are committed to SQLite every 1,000 images, including completed
work from the active batch when you press Cancel. Starting the scan again resumes
from the last checkpoint instead of recalculating those hashes.

Similarity groups use representative matching: every member must match the
group's first image at the selected threshold. This prevents long chains of
indirectly similar images from collapsing into one misleading group.

Exact Files mode includes a **Delete From All Groups** action for keeping one file
from every byte-identical group and moving the rest to the Windows Recycle Bin.
The keeper can be selected by newest or oldest modified time, filename order,
or shortest/longest filename. Bulk deletion runs in the background and can be
cancelled between files.

Discovery, decoding, hashing, cache writing, and grouping form a bounded streaming
pipeline; no 1,000-image batch barrier holds up faster work. JPEGs and small files
receive queue priority, while HEIF and RAW work have separate concurrency limits.
JPEG hashing uses EXIF thumbnails when suitable, otherwise libjpeg-turbo decoder
scaling and direct grayscale output. HEIF thumbnails and RAW embedded previews are
preferred before full decoding.

Exact-file reader limits adapt to the storage device: one reader on rotational
media, two on a conventional SSD, and up to four on NVMe. The decoder pool uses
up to eight workers, with lower defaults on slower storage. Override the limits
when benchmarking:

```powershell
$env:DUPEGEM_THREADS = 12
$env:DUPEGEM_EXACT_THREADS = 4
$env:DUPEGEM_HEIF_THREADS = 4
$env:DUPEGEM_RAW_PREVIEW_THREADS = 4
$env:DUPEGEM_RAW_FULL_THREADS = 2
```

Qt image allocations are limited to 128 MB per decoded image as a final guard
against corrupt or extreme files. This normally does not affect scanning because
decoders are asked for a 64×64 result. It can be adjusted when necessary:

```powershell
$env:DUPEGEM_IMAGE_LIMIT_MB = 256
```

The cache is safe to delete whenever you want a completely fresh scan; DupeGem
will recreate it. Older `.dupegem_cache.dat` files are no longer used.

For libraries of 50,000 or more hashes and thresholds up to 15, DupeGem uses a
parallel 16×16-bit sorted-band index with exact 256-bit Hamming verification.
Smaller libraries or broader thresholds use a compact contiguous BK-tree. Group
queries run in parallel chunks but group assignment remains deterministic and
preserves representative matching.

On NTFS, DupeGem stores volume/file IDs and a change-journal cursor. When Windows
grants journal access, an unchanged rescan can apply only USN changes instead of
walking the entire directory tree. Journal expiry, unavailable permissions,
network filesystems, and directory renames fall back automatically to the normal
streaming enumeration path.

## Benchmark and regression tests

The benchmark mode is headless and does not load a Qt platform plugin:

```powershell
.\release\main.exe --benchmark "D:\Photos" --algorithm dhash --output results.json
```

Use `--selected-only` to measure the low-power perceptual mode. The JSON report
includes discovery, cache lookup, decode/hash, SQLite, index/query, bytes-read,
cache-hit, per-format, JPEG-decoder comparison, throughput, and peak-memory data.
Use `--cancel-after N` in automated tests to stop after a controlled number of
new hashes and verify that the next run resumes from the saved cache.

Build and run the exact/index regression suite with:

```powershell
cd tests
qmake6 performance_tests.pro -o Makefile.performance
mingw32-make -f Makefile.performance
.\release\performance_tests.exe
.\release\performance_tests.exe --benchmark-size 50000
```

The second command compares the exact BK-tree and sorted-band candidate indexes
on 50,000 deterministic hashes. From the project root, run the full cache and
filesystem integration suite or a disposable 50,000-file cold/warm stress scan:

```powershell
.\tests\integration_tests.ps1
.\tests\stress_50000.ps1
```
