QT += widgets concurrent sql
VERSION = 0.4.9
SOURCES += main.cpp image_decoder.cpp
HEADERS += bk_tree.h image_decoder.h
CONFIG += c++17
CONFIG += link_pkgconfig
CONFIG -= console
CONFIG += windows
PKGCONFIG += libheif libraw_r
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
    LIBS += -lole32 -lshell32 -lgdi32
    SQLITE_PLUGIN = $$[QT_INSTALL_PLUGINS]/sqldrivers/qsqlite.dll
    SQLITE_DEST = $$DESTDIR/sqldrivers
    QMAKE_POST_LINK += $$QMAKE_MKDIR $$shell_path($$SQLITE_DEST) $$escape_expand(\n\t)
    QMAKE_POST_LINK += $$QMAKE_COPY $$shell_path($$SQLITE_PLUGIN) $$shell_path($$SQLITE_DEST)
    QMAKE_POST_LINK += $$escape_expand(\n\t) $$QMAKE_COPY $$shell_path($$PWD/icon.png) $$shell_path($$DESTDIR/icon.png)
}
