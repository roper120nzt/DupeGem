QT += widgets concurrent sql
VERSION = 0.4.1
SOURCES += main.cpp image_decoder.cpp exact_hash.cpp ntfs_journal.cpp \
    third_party/blake3/blake3.c \
    third_party/blake3/blake3_dispatch.c \
    third_party/blake3/blake3_portable.c \
    third_party/blake3/blake3_sse2_x86-64_windows_gnu.S \
    third_party/blake3/blake3_sse41_x86-64_windows_gnu.S \
    third_party/blake3/blake3_avx2_x86-64_windows_gnu.S \
    third_party/blake3/blake3_avx512_x86-64_windows_gnu.S
HEADERS += bk_tree.h band_index.h image_decoder.h exact_hash.h ntfs_journal.h \
    third_party/blake3/blake3.h third_party/blake3/blake3_impl.h
INCLUDEPATH += third_party/blake3
CONFIG += c++17
CONFIG += link_pkgconfig
CONFIG -= console
CONFIG += windows
DEFINES += _WIN32_WINNT=0x0602
PKGCONFIG += libheif libraw_r libxxhash libturbojpeg
QMAKE_LFLAGS += -static-libstdc++ -static-libgcc

CONFIG(debug, debug|release) {
    DESTDIR = debug
    OBJECTS_DIR = build/debug
    MOC_DIR = build/debug
} else {
    DESTDIR = release
    OBJECTS_DIR = build/release
    MOC_DIR = build/release
}

# Keep the normal Notepad++ development build cache-capable. The portable
# packaging script deploys all runtime plugins; this copies the one SQL plugin
# needed by release\main.exe during local build/test runs.
win32 {
    LIBS += -lole32 -lshell32 -lgdi32 -lpsapi
    SQLITE_PLUGIN = $$[QT_INSTALL_PLUGINS]/sqldrivers/qsqlite.dll
    SQLITE_DEST = $$DESTDIR/sqldrivers
    QMAKE_POST_LINK += $$QMAKE_MKDIR $$shell_path($$SQLITE_DEST) $$escape_expand(\n\t)
    QMAKE_POST_LINK += $$QMAKE_COPY $$shell_path($$SQLITE_PLUGIN) $$shell_path($$SQLITE_DEST)
    QMAKE_POST_LINK += $$escape_expand(\n\t) $$QMAKE_COPY $$shell_path($$PWD/icon.png) $$shell_path($$DESTDIR/icon.png)
}
