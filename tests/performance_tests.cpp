#include "../band_index.h"
#include "../bk_tree.h"
#include "../exact_hash.h"
#include "../image_decoder.h"
#include "../ntfs_journal.h"

#include <QElapsedTimer>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>

#include <algorithm>
#include <bitset>
#include <iostream>
#include <random>
#include <vector>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
using Hash=std::bitset<256>;

bool writeBytes(const QString& path,const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes)==bytes.size();
}

bool expect(bool condition,const char* message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

std::vector<int> brute(const std::vector<Hash>& hashes,const Hash& query,
                       int distance,int exclude) {
    std::vector<int> hits;
    for (int i=0;i<int(hashes.size());++i)
        if (i!=exclude && int((hashes[size_t(i)]^query).count())<=distance) hits.push_back(i);
    return hits;
}

void normalize(std::vector<int>& values) {
    std::sort(values.begin(),values.end());
    values.erase(std::unique(values.begin(),values.end()),values.end());
}

void appendLe16(QByteArray& bytes,quint16 value) {
    bytes.append(char(value&0xff)); bytes.append(char(value>>8));
}

void appendLe32(QByteArray& bytes,quint32 value) {
    for (int shift=0;shift<32;shift+=8) bytes.append(char((value>>shift)&0xff));
}

QByteArray jpegWithOrientation(const QByteArray& jpeg,quint16 orientation) {
    if (jpeg.size()<2 || uchar(jpeg[0])!=0xff || uchar(jpeg[1])!=0xd8) return {};
    QByteArray payload("Exif\0\0",6);
    payload.append("II",2); appendLe16(payload,42); appendLe32(payload,8);
    appendLe16(payload,1);
    appendLe16(payload,0x0112); appendLe16(payload,3); appendLe32(payload,1);
    appendLe16(payload,orientation); appendLe16(payload,0);
    appendLe32(payload,0);
    const quint16 length=quint16(payload.size()+2);
    QByteArray result=jpeg.left(2);
    result.append(char(0xff)); result.append(char(0xe1));
    result.append(char(length>>8)); result.append(char(length&0xff));
    result.append(payload); result.append(jpeg.mid(2));
    return result;
}

void fillRandomHashes(std::vector<Hash>& hashes,std::mt19937_64& random) {
    for (Hash& hash:hashes)
        for (size_t word=0;word<4;++word) {
            const quint64 value=random();
            for (size_t bit=0;bit<64;++bit) hash.set(word*64+bit,(value>>bit)&1u);
        }
}
}

int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    bool ok=true;
    QTemporaryDir directory;
    ok&=expect(directory.isValid(),"temporary directory");

    const QString empty=directory.filePath(QStringLiteral("empty.bin"));
    ok&=expect(writeBytes(empty,{}),"write empty file");
    const dg::ExactHashResult emptyHash=dg::hashFileBlake3(empty);
    ok&=expect(emptyHash.ok && emptyHash.blake3.toHex()==
        QByteArrayLiteral("af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"),
        "official BLAKE3 empty-input vector");

    QByteArray original(512*1024,Qt::Uninitialized);
    for (qsizetype i=0;i<original.size();++i) original[i]=char((i*131+17)&0xff);
    QByteArray different=original;
    different[different.size()/4+100]^=char(0x5a);
    const QString a=directory.filePath(QStringLiteral("a.bin"));
    const QString b=directory.filePath(QStringLiteral("b.bin"));
    const QString c=directory.filePath(QStringLiteral("c.bin"));
    ok&=expect(writeBytes(a,original)&&writeBytes(b,original)&&writeBytes(c,different),
               "write exact fixtures");
    const auto quickA=dg::quickSampleFile(a),quickB=dg::quickSampleFile(b),quickC=dg::quickSampleFile(c);
    ok&=expect(quickA.ok&&quickB.ok&&quickC.ok,"first/final XXH3 samples");
    ok&=expect(quickA.sampleHash==quickB.sampleHash,"identical end samples match");
    ok&=expect(quickA.sampleHash==quickC.sampleHash,"shared ends proceed to distributed gate");
    const auto sampleA=dg::sampleFile(a),sampleB=dg::sampleFile(b),sampleC=dg::sampleFile(c);
    ok&=expect(sampleA.ok&&sampleB.ok&&sampleC.ok,"distributed XXH3 samples");
    ok&=expect(sampleA.sampleHash==sampleB.sampleHash,"identical samples match");
    ok&=expect(sampleA.sampleHash!=sampleC.sampleHash,"25-percent sample rejects candidate");
    const auto fullA=dg::hashFileBlake3(a),fullB=dg::hashFileBlake3(b),fullC=dg::hashFileBlake3(c);
    ok&=expect(fullA.blake3==fullB.blake3&&fullA.blake3!=fullC.blake3,"BLAKE3 matching");
    ok&=expect(dg::filesByteEqual(a,b)&&!dg::filesByteEqual(a,c),"final byte verification");
    ok&=expect(dg::fileIdentity(a).isValid(),"Windows file identity");
#ifdef Q_OS_WIN
    const QString hardlink=directory.filePath(QStringLiteral("hardlink.bin"));
    const std::wstring linkName=QDir::toNativeSeparators(hardlink).toStdWString();
    const std::wstring target=QDir::toNativeSeparators(a).toStdWString();
    ok&=expect(CreateHardLinkW(linkName.c_str(),target.c_str(),nullptr)!=FALSE,"create hardlink");
    ok&=expect(dg::fileIdentity(a).cacheKey()==dg::fileIdentity(hardlink).cacheKey(),
               "hardlinks share file identity");
#endif

    QImage source(40,20,QImage::Format_RGB32);
    source.fill(QColor(30,120,210));
    const QString plainJpeg=directory.filePath(QStringLiteral("plain.jpg"));
    ok&=expect(source.save(plainJpeg,"JPEG",90),"write JPEG orientation fixture");
    QFile encodedFile(plainJpeg);
    ok&=expect(encodedFile.open(QIODevice::ReadOnly),"read JPEG orientation fixture");
    const QByteArray orientedBytes=jpegWithOrientation(encodedFile.readAll(),6);
    const QString orientedJpeg=directory.filePath(QStringLiteral("oriented.jpg"));
    ok&=expect(!orientedBytes.isEmpty()&&writeBytes(orientedJpeg,orientedBytes),
               "inject EXIF orientation");
    QSize qtSource,turboSource;
    const QImage qtOriented=dg::decodeImage(orientedJpeg,QSize(16,32),
                                             Qt::IgnoreAspectRatio,&qtSource);
    const QImage turboOriented=dg::decodeHashImage(orientedJpeg,QSize(16,32),&turboSource);
    ok&=expect(!qtOriented.isNull()&&qtOriented.size()==QSize(16,32)
               &&qtSource==QSize(20,40),"Qt decoder honors EXIF orientation and target size");
    ok&=expect(!turboOriented.isNull()&&turboOriented.size()==QSize(16,32)
               &&turboSource==QSize(20,40),"TurboJPEG path honors EXIF orientation and target size");

    dg::NtfsJournalCursor cursor;
    cursor.volumeSerial=0x11223344u; cursor.journalId=0xaabbccdd11223344ull; cursor.nextUsn=987654;
    const dg::NtfsJournalCursor restored=dg::NtfsJournalCursor::deserialize(cursor.serialize());
    ok&=expect(restored.volumeSerial==cursor.volumeSerial
               &&restored.journalId==cursor.journalId&&restored.nextUsn==cursor.nextUsn,
               "NTFS journal cursor round trip");
    QString journalError;
    (void)dg::currentNtfsJournal(directory.path(),&journalError);

    std::mt19937_64 random(0x4455504547454dull);
    std::vector<Hash> hashes(4000);
    fillRandomHashes(hashes,random);
    // Force duplicate and near-duplicate clusters.
    for (int i=1;i<=20;++i) {
        hashes[size_t(i)]=hashes[0];
        for (int bit=0;bit<i%16;++bit) hashes[size_t(i)].flip(size_t(bit));
    }

    BKTree<Hash> tree;
    tree.reserve(hashes.size());
    for (int i=0;i<int(hashes.size());++i) tree.add(hashes[size_t(i)],i);
    HashBandIndex<Hash> bands;
    bands.build(hashes,8);
    for (int threshold:{0,4,15}) for (int queryIndex:{0,1,7,20,103,2048,3999}) {
        auto expected=brute(hashes,hashes[size_t(queryIndex)],threshold,queryIndex);
        auto fromTree=tree.query(hashes[size_t(queryIndex)],threshold,queryIndex);
        auto fromBands=bands.query(hashes[size_t(queryIndex)],threshold,queryIndex);
        normalize(expected); normalize(fromTree); normalize(fromBands);
        ok&=expect(fromTree==expected,"compact BK-tree equals brute force");
        ok&=expect(fromBands==expected,"16-band index equals brute force");
    }

    const QStringList arguments=app.arguments();
    const int sizeOption=arguments.indexOf(QStringLiteral("--benchmark-size"));
    if (sizeOption>=0 && sizeOption+1<arguments.size()) {
        bool sizeOk=false;
        const int count=arguments[sizeOption+1].toInt(&sizeOk);
        if (!sizeOk || count<1000) return 2;
        std::vector<Hash> large(static_cast<size_t>(count));
        fillRandomHashes(large,random);
        for (int i=1;i<std::min(count,256);++i) {
            large[size_t(i)]=large[0];
            for (int bit=0;bit<i%5;++bit) large[size_t(i)].flip(size_t(bit));
        }
        QElapsedTimer timer;
        timer.start();
        BKTree<Hash> largeTree; largeTree.reserve(large.size());
        for (int i=0;i<count;++i) largeTree.add(large[size_t(i)],i);
        const qint64 bkBuild=timer.elapsed();
        timer.restart();
        quint64 bkHits=0;
        for (int i=0;i<count;++i) bkHits+=largeTree.query(large[size_t(i)],4,i).size();
        const qint64 bkQuery=timer.elapsed();
        timer.restart();
        HashBandIndex<Hash> largeBands; largeBands.build(large,8);
        const qint64 bandBuild=timer.elapsed();
        timer.restart();
        quint64 bandHits=0;
        for (int i=0;i<count;++i) bandHits+=largeBands.query(large[size_t(i)],4,i).size();
        const qint64 bandQuery=timer.elapsed();
        ok&=expect(bkHits==bandHits,"large BK-tree and band-index hit counts match");
        std::cout << "INDEX_BENCHMARK size=" << count
                  << " bk_build_ms=" << bkBuild << " bk_query_ms=" << bkQuery
                  << " band_build_ms=" << bandBuild << " band_query_ms=" << bandQuery
                  << " hits=" << bandHits << '\n';
    }

    if (ok) std::cout << "All performance architecture tests passed.\n";
    return ok ? 0 : 1;
}
