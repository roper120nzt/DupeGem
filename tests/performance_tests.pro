QT += core gui
CONFIG += console c++17 link_pkgconfig
CONFIG -= app_bundle
TARGET = performance_tests

SOURCES += performance_tests.cpp ../exact_hash.cpp ../image_decoder.cpp ../ntfs_journal.cpp \
    ../third_party/blake3/blake3.c \
    ../third_party/blake3/blake3_dispatch.c \
    ../third_party/blake3/blake3_portable.c \
    ../third_party/blake3/blake3_sse2_x86-64_windows_gnu.S \
    ../third_party/blake3/blake3_sse41_x86-64_windows_gnu.S \
    ../third_party/blake3/blake3_avx2_x86-64_windows_gnu.S \
    ../third_party/blake3/blake3_avx512_x86-64_windows_gnu.S
HEADERS += ../exact_hash.h ../bk_tree.h ../band_index.h ../image_decoder.h ../ntfs_journal.h \
    ../third_party/blake3/blake3.h ../third_party/blake3/blake3_impl.h
INCLUDEPATH += .. ../third_party/blake3
PKGCONFIG += libxxhash libheif libraw_r libturbojpeg
DEFINES += _WIN32_WINNT=0x0602

win32:LIBS += -lole32 -lshell32 -lgdi32
