#define _USE_MATH_DEFINES

// Qt
#include <QApplication>
#include <QByteArrayView>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QColorSpace>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QFutureWatcher>
#include <QHash>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QImageReader>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMetaObject>
#include <QMouseEvent>
#include <QMovie>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QRunnable>
#include <QSaveFile>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QSlider>
#include <QSplitter>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStyle>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <QMainWindow>
#include <QAbstractItemView>
#include <QMenu>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QSet>
#include <QVector>
#include <QtConcurrent>

// STL
#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <cmath>
#include <cstdio>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <numeric>
#include <utility>
#include <vector>

// Local
#include "bk_tree.h"
#include "image_decoder.h"

namespace dg {

// ---------- Constants & Types ----------

constexpr int kHashBits = 256;
using BitHash = std::bitset<kHashBits>;

// Added MD5 to the list
enum class HashAlgo : int { DHash = 0, PHash, AHash, WHash, BHash, MD5, Count };
enum class Md5KeepRule : int { Newest, Oldest, FilenameFirst, FilenameLast, ShortestName, LongestName };

// Keep HashAlgo's numeric values stable for existing SQLite caches and CLI
// callers. The selector can evolve independently through this display order.
constexpr std::array<HashAlgo, 6> kAlgorithmDisplayOrder{
    HashAlgo::MD5, HashAlgo::AHash, HashAlgo::DHash,
    HashAlgo::WHash, HashAlgo::BHash, HashAlgo::PHash};

inline const char* algoName(HashAlgo a) {
    switch (a) {
        case HashAlgo::DHash: return "Difference Hash (dHash)";
        case HashAlgo::PHash: return "Perceptual Hash (pHash)";
        case HashAlgo::AHash: return "Average Hash (aHash)";
        case HashAlgo::WHash: return "Wavelet Hash (wHash)";
        case HashAlgo::BHash: return "BlockMean Hash (bHash)";
        case HashAlgo::MD5:   return "Exact MD5 (byte-identical)";
        default: return "Unknown";
    }
}

// ---------- Logger (kept minimal; no debug UI) ----------

class Logger {
public:
    static Logger& instance() { static Logger L; return L; }
    void enable(bool on) { showDebug_.store(on); }
    bool enabled() const { return showDebug_.load(); }
    void setCurrentFile(QString f) { currentFile_ = std::move(f); }
    const QString& currentFile() const { return currentFile_; }
private:
    Logger() = default;
    std::atomic<bool> showDebug_{false}; // default off
    thread_local static inline QString currentFile_;
};

void qtMessageHandler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg) {
    // libpng reports malformed-but-usable embedded ICC profiles as warnings.
    // They do not invalidate the decoded pixels and can flood stderr during a
    // large scan. Keep every other warning visible.
    if ((type == QtInfoMsg || type == QtWarningMsg) &&
        (msg.contains(QStringLiteral("iCCP:"), Qt::CaseInsensitive) ||
         msg.contains(QStringLiteral("Invalid ICC illuminant"), Qt::CaseInsensitive))) {
        return;
    }

    auto& L = Logger::instance();
    if (type == QtDebugMsg && !L.enabled()) return;

    QString modified = msg;

    const QByteArray local = modified.toLocal8Bit();
    const char* file = ctx.file ? ctx.file : "";
    const char* func = ctx.function ? ctx.function : "";
    const char* level =
        type == QtDebugMsg   ? "DEBUG"   :
        type == QtInfoMsg    ? "INFO"    :
        type == QtWarningMsg ? "WARNING" :
        type == QtCriticalMsg? "CRITICAL":
                               "FATAL";

    fprintf(stderr, "%s: %s (%s:%u, %s)\n", level, local.constData(), file, ctx.line, func);
    fflush(stderr);
}

// ---------- BitHash (de)serialization ----------

inline void writeHash(QDataStream& ds, const BitHash& h) {
    QByteArray bytes(kHashBits / 8, 0);
    for (size_t i = 0; i < kHashBits; ++i)
        if (h.test(i)) bytes[int(i / 8)] |= (1 << (i % 8));
    ds << bytes;
}
inline BitHash readHash(QDataStream& ds) {
    QByteArray bytes; ds >> bytes;
    if (bytes.size() != kHashBits / 8) { qWarning() << "Invalid hash size"; return {}; }
    BitHash h;
    for (size_t i = 0; i < kHashBits; ++i)
        h.set(i, (uchar(bytes[int(i/8)]) & (1 << (i%8))) != 0);
    return h;
}
inline int hamming(const BitHash& a, const BitHash& b) { return (a ^ b).count(); }

// ---------- Hashes ----------

inline BitHash pHash(const QImage& in) {
    if (in.isNull()) return {};
    QImage img = (in.width()==32 && in.height()==32 && in.format()==QImage::Format_Grayscale8)
        ? in
        : in.scaled(32,32,Qt::IgnoreAspectRatio,Qt::FastTransformation)
             .convertToFormat(QImage::Format_Grayscale8);
    if (img.isNull()) return {};

    std::array<float,1024> data{};
    for (int y=0;y<32;++y) {
        const uchar* line = img.constScanLine(y);
        for (int x=0;x<32;++x) data[y*32+x]=float(line[x]);
    }
    if (std::all_of(data.begin(), data.end(), [](float v){return v==0.f;})) return {};

    // Only the low 16x16 DCT coefficients are needed. Precompute the basis
    // once and avoid calling std::cos tens of thousands of times per image.
    static const std::array<float,16*32> basis = [] {
        std::array<float,16*32> b{};
        for (int u=0;u<16;++u) {
            const double scale = (u==0) ? std::sqrt(1.0/32.0) : std::sqrt(2.0/32.0);
            for (int x=0;x<32;++x)
                b[u*32+x] = float(scale * std::cos(M_PI*u*(2*x+1)/64.0));
        }
        return b;
    }();

    std::array<float,32*16> rows{};
    for (int y=0;y<32;++y) {
        for (int u=0;u<16;++u) {
            float sum=0.f;
            for (int x=0;x<32;++x) sum += data[y*32+x] * basis[u*32+x];
            rows[y*16+u]=sum;
        }
    }
    std::array<float,16*16> coeff{};
    for (int v=0;v<16;++v) {
        for (int u=0;u<16;++u) {
            float sum=0.f;
            for (int y=0;y<32;++y) sum += rows[y*16+u] * basis[v*32+y];
            coeff[v*16+u]=sum;
        }
    }

    float sum=0; int cnt=0;
    for (int i=1;i<256;++i) { sum+=coeff[i]; ++cnt; }
    const float avg=cnt?sum/cnt:0.f;
    BitHash out;
    for (int i=0;i<256;++i) out.set(i, coeff[i]>avg);
    return out;
}
inline QByteArray hashBytes(const BitHash& h) {
    QByteArray bytes(kHashBits / 8, 0);
    for (size_t i=0;i<kHashBits;++i)
        if (h.test(i)) bytes[int(i/8)] |= char(1u << (i%8));
    return bytes;
}
inline BitHash hashFromBytes(const QByteArray& bytes) {
    if (bytes.size()!=kHashBits/8) return {};
    BitHash h;
    for (size_t i=0;i<kHashBits;++i)
        h.set(i, (uchar(bytes[int(i/8)]) & (1u << (i%8)))!=0);
    return h;
}

inline const char* algoSpeed(HashAlgo a) {
    switch (a) {
        case HashAlgo::AHash: return "Very fast";
        case HashAlgo::DHash: return "Very fast";
        case HashAlgo::WHash: return "Fast";
        case HashAlgo::BHash: return "Fast";
        case HashAlgo::PHash: return "Moderate";
        case HashAlgo::MD5:   return "Usually fastest";
        default: return "Unknown";
    }
}

inline QString algoDescription(HashAlgo a) {
    switch (a) {
        case HashAlgo::DHash:
            return QStringLiteral("Compares neighboring brightness values to capture edges and gradients. "
                                  "Excellent default for resized or recompressed copies; sensitive to crops and rotation.");
        case HashAlgo::PHash:
            return QStringLiteral("Compares low-frequency image structure using a DCT. Usually the most robust visual matcher "
                                  "for compression and color changes, with the highest hashing cost.");
        case HashAlgo::AHash:
            return QStringLiteral("Compares every region with the image's average brightness. The cheapest visual hash, "
                                  "but more likely to group unrelated images with similar light/dark layouts.");
        case HashAlgo::WHash:
            return QStringLiteral("Uses a Haar wavelet transform to summarize coarse structure. A good balance of speed and "
                                  "robustness for resized and mildly edited images.");
        case HashAlgo::BHash:
            return QStringLiteral("Compares median brightness across local blocks. Handles broad brightness changes well and "
                                  "is more spatially detailed than aHash.");
        case HashAlgo::MD5:
            return QStringLiteral("Finds byte-for-byte identical files only. Files are bucketed by size first, so disk data is "
                                  "read only for size collisions; visually identical re-encodes will not match.");
        default:
            return {};
    }
}

inline BitHash aHash(const QImage& in){
    if (in.isNull()) return {};
    constexpr int n=int(std::sqrt(kHashBits));
    QImage img=(in.width()==n&&in.height()==n&&in.format()==QImage::Format_Grayscale8)
        ? in
        : in.scaled(n,n,Qt::IgnoreAspectRatio,Qt::FastTransformation)
             .convertToFormat(QImage::Format_Grayscale8);
    if (img.isNull()) return {};
    const uchar* p=img.constBits();
    long long sum=0; for (int i=0;i<kHashBits;++i) sum+=p[i];
    const uchar avg=uchar(sum/kHashBits);
    BitHash out; for (int i=0;i<kHashBits;++i) out.set(i, p[i]>avg); return out;
}
inline BitHash dHash(const QImage& in){
    if (in.isNull()) return {};
    constexpr int n=int(std::sqrt(kHashBits));
    QImage img=(in.width()==n+1&&in.height()==n&&in.format()==QImage::Format_Grayscale8)
        ? in
        : in.scaled(n+1,n,Qt::IgnoreAspectRatio,Qt::FastTransformation)
             .convertToFormat(QImage::Format_Grayscale8);
    if (img.isNull()) return {};
    BitHash out; int idx=0;
    for (int y=0;y<n;++y){ const uchar* line=img.constScanLine(y);
        for (int x=0;x<n;++x) out.set(idx++, line[x]>line[x+1]);}
    return out;
}
inline BitHash bHash(const QImage& in){
    if (in.isNull()) return {};
    constexpr int n=int(std::sqrt(kHashBits)), blk=4, scaled=n*blk;
    QImage img=(in.width()==scaled&&in.height()==scaled&&in.format()==QImage::Format_Grayscale8)
        ? in
        : in.scaled(scaled,scaled,Qt::IgnoreAspectRatio,Qt::FastTransformation)
             .convertToFormat(QImage::Format_Grayscale8);
    if (img.isNull()) return {};
    std::array<double,kHashBits> means{};
    for (int by=0;by<n;++by) for (int bx=0;bx<n;++bx){
        double s=0;
        for (int dy=0;dy<blk;++dy){
            const uchar* line=img.constScanLine(by*blk+dy)+(bx*blk);
            for (int dx=0;dx<blk;++dx) s+=line[dx];
        }
        means[by*n+bx]=s/double(blk*blk);
    }
    auto sorted=means;
    std::nth_element(sorted.begin(),sorted.begin()+kHashBits/2-1,sorted.end());
    std::nth_element(sorted.begin()+kHashBits/2,sorted.begin()+kHashBits/2,sorted.end());
    const double med=(sorted[kHashBits/2-1]+sorted[kHashBits/2])*0.5;
    BitHash out; for (int i=0;i<kHashBits;++i) out.set(i, means[i]>med); return out;
}
inline BitHash wHash(const QImage& in){
    if (in.isNull()) return {};
    constexpr int n=int(std::sqrt(kHashBits)), scaled=n*2;
    QImage img=(in.width()==scaled&&in.height()==scaled&&in.format()==QImage::Format_Grayscale8)
        ? in
        : in.scaled(scaled,scaled,Qt::IgnoreAspectRatio,Qt::FastTransformation)
             .convertToFormat(QImage::Format_Grayscale8);
    if (img.isNull()) return {};
    std::vector<std::vector<double>> A(scaled,std::vector<double>(scaled));
    for (int y=0;y<scaled;++y){ const uchar* line=img.constScanLine(y);
        for (int x=0;x<scaled;++x) A[y][x]=line[x]; }
    const double invS2=1.0/std::sqrt(2.0);
    // Use a temporary row/column. The old in-place transform overwrote samples
    // that later iterations still needed, producing incorrect hashes.
    std::array<double,scaled> tmp{};
    for (int y=0;y<scaled;++y) {
        for (int x=0;x<n;++x){
            const double a=A[y][2*x], b=A[y][2*x+1];
            tmp[x]=(a+b)*invS2; tmp[x+n]=(a-b)*invS2;
        }
        std::copy(tmp.begin(), tmp.end(), A[y].begin());
    }
    for (int x=0;x<scaled;++x) {
        for (int y=0;y<n;++y){
            const double a=A[2*y][x], b=A[2*y+1][x];
            tmp[y]=(a+b)*invS2; tmp[y+n]=(a-b)*invS2;
        }
        for (int y=0;y<scaled;++y) A[y][x]=tmp[y];
    }
    double sum=0; for (int y=0;y<n;++y) for (int x=0;x<n;++x) sum+=A[y][x];
    const double avg=sum/double(kHashBits);
    BitHash out; int bit=0;
    for (int y=0;y<n;++y) for (int x=0;x<n;++x) out.set(bit++, A[y][x]>avg);
    return out;
}

// ---------- Image Meta ----------

struct ImgMeta {
    QString file;
    BitHash phash,ahash,dhash,bhash,whash;
    QString md5;
    QSize   resolution;
    qint64  size = 0;
    qint64  mtime = 0;
    quint8  hashMask = 0;
    bool    selected = false;
    int     group = -1;
    bool    valid = true;
};

inline quint8 hashBit(HashAlgo a) {
    return a==HashAlgo::MD5 ? 0 : quint8(1u << unsigned(a));
}
inline bool hasHash(const ImgMeta& m, HashAlgo a) {
    return a!=HashAlgo::MD5 && (m.hashMask & hashBit(a));
}

inline const BitHash& pickHash(const ImgMeta& m, HashAlgo a) {
    switch (a) {
        case HashAlgo::DHash: return m.dhash;
        case HashAlgo::PHash: return m.phash;
        case HashAlgo::AHash: return m.ahash;
        case HashAlgo::WHash: return m.whash;
        case HashAlgo::BHash: return m.bhash;
        default:               return m.dhash;
    }
}

// ---------- Background work & SQLite cache ----------

class FunctionTask final : public QRunnable {
public:
    explicit FunctionTask(std::function<void()> fn) : fn_(std::move(fn)) { setAutoDelete(true); }
    void run() override { fn_(); }
private:
    std::function<void()> fn_;
};

inline int workerCount() {
    int threads=QThread::idealThreadCount();
    if (threads<1) threads=4;
    threads=std::clamp(threads,1,8);
    bool ok=false;
    const int requested=qEnvironmentVariableIntValue("DUPEGEM_THREADS", &ok);
    return ok && requested>0 ? std::clamp(requested,1,64) : threads;
}

inline bool readAndHash(ImgMeta& m, HashAlgo algo) {
    if (algo==HashAlgo::MD5) {
        const QFileInfo info(m.file);
        return info.isFile() && info.isReadable();
    }
    QSize source;
    QImage img=decodeImage(m.file,QSize(64,64),Qt::IgnoreAspectRatio,&source);
    if (img.isNull()) return false;
    if (!m.resolution.isValid()) m.resolution=source;
    img.setColorSpace(QColorSpace());
    const QImage gray=img.convertToFormat(QImage::Format_Grayscale8);
    switch (algo) {
        case HashAlgo::DHash: m.dhash=dHash(gray); break;
        case HashAlgo::PHash: m.phash=pHash(gray); break;
        case HashAlgo::AHash: m.ahash=aHash(gray); break;
        case HashAlgo::WHash: m.whash=wHash(gray); break;
        case HashAlgo::BHash: m.bhash=bHash(gray); break;
        default: return false;
    }
    m.hashMask |= hashBit(algo);
    return true;
}

class SqliteCache {
public:
    explicit SqliteCache(const QString& path) {
        static std::atomic<quint64> sequence{0};
        connectionName_=QStringLiteral("dupegem_%1_%2")
            .arg(quintptr(QThread::currentThreadId())).arg(++sequence);
        db_=QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName_);
        db_.setDatabaseName(path);
        if (!db_.open()) { error_=db_.lastError().text(); return; }
        QSqlQuery q(db_);
        q.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
        q.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
        q.exec(QStringLiteral("PRAGMA temp_store=MEMORY"));
        if (!q.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS images("
            "path TEXT PRIMARY KEY, mtime INTEGER NOT NULL, size INTEGER NOT NULL, "
            "width INTEGER, height INTEGER, dhash BLOB, phash BLOB, ahash BLOB, "
            "whash BLOB, bhash BLOB, md5 TEXT)"))) {
            error_=q.lastError().text(); db_.close();
        }
    }
    ~SqliteCache() {
        if (db_.isValid()) db_.close();
        db_=QSqlDatabase();
        QSqlDatabase::removeDatabase(connectionName_);
    }
    bool isOpen() const { return db_.isOpen() && error_.isEmpty(); }
    QString error() const { return error_; }

    QHash<QString,ImgMeta> loadAll() {
        QHash<QString,ImgMeta> out;
        if (!isOpen()) return out;
        QSqlQuery q(db_);
        if (!q.exec(QStringLiteral(
            "SELECT path,mtime,size,width,height,dhash,phash,ahash,whash,bhash,md5 FROM images"))) {
            error_=q.lastError().text(); return out;
        }
        while (q.next()) {
            ImgMeta m;
            const QString rel=q.value(0).toString();
            m.mtime=q.value(1).toLongLong(); m.size=q.value(2).toLongLong();
            m.resolution=QSize(q.value(3).toInt(),q.value(4).toInt());
            const std::array<HashAlgo,5> algos{HashAlgo::DHash,HashAlgo::PHash,HashAlgo::AHash,HashAlgo::WHash,HashAlgo::BHash};
            for (int i=0;i<5;++i) {
                const QByteArray bytes=q.value(5+i).toByteArray();
                if (bytes.size()!=kHashBits/8) continue;
                switch (algos[i]) {
                    case HashAlgo::DHash: m.dhash=hashFromBytes(bytes); break;
                    case HashAlgo::PHash: m.phash=hashFromBytes(bytes); break;
                    case HashAlgo::AHash: m.ahash=hashFromBytes(bytes); break;
                    case HashAlgo::WHash: m.whash=hashFromBytes(bytes); break;
                    case HashAlgo::BHash: m.bhash=hashFromBytes(bytes); break;
                    default: break;
                }
                m.hashMask|=hashBit(algos[i]);
            }
            m.md5=q.value(10).toString();
            out.insert(rel,std::move(m));
        }
        return out;
    }

    bool saveRows(const QString& rootPath, const std::vector<ImgMeta>& images,
                  const std::vector<int>& rows) {
        if (!isOpen()) return false;
        if (!db_.transaction()) { error_=db_.lastError().text(); return false; }
        QSqlQuery q(db_);
        q.prepare(QStringLiteral(
            "INSERT INTO images(path,mtime,size,width,height,dhash,phash,ahash,whash,bhash,md5) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(path) DO UPDATE SET "
            "mtime=excluded.mtime,size=excluded.size,width=excluded.width,height=excluded.height,"
            "dhash=excluded.dhash,phash=excluded.phash,ahash=excluded.ahash,"
            "whash=excluded.whash,bhash=excluded.bhash,md5=excluded.md5"));
        const QDir root(rootPath);
        for (int row: rows) {
            if (row<0 || row>=int(images.size())) continue;
            const ImgMeta& m=images[size_t(row)];
            if (!m.valid || m.file.isEmpty()) continue;
            int bind=0;
            q.bindValue(bind++,root.relativeFilePath(m.file));
            q.bindValue(bind++,m.mtime); q.bindValue(bind++,m.size);
            q.bindValue(bind++,m.resolution.width()); q.bindValue(bind++,m.resolution.height());
            const std::array<HashAlgo,5> algos{HashAlgo::DHash,HashAlgo::PHash,HashAlgo::AHash,HashAlgo::WHash,HashAlgo::BHash};
            for (HashAlgo a: algos) q.bindValue(bind++,hasHash(m,a) ? QVariant(hashBytes(pickHash(m,a))) : QVariant());
            q.bindValue(bind++,m.md5.isEmpty() ? QVariant() : QVariant(m.md5));
            if (!q.exec()) { error_=q.lastError().text(); db_.rollback(); return false; }
            q.finish();
        }
        if (!db_.commit()) { error_=db_.lastError().text(); return false; }
        return true;
    }
    bool removePaths(const QStringList& relativePaths) {
        if (!isOpen() || relativePaths.isEmpty()) return isOpen();
        if (!db_.transaction()) return false;
        QSqlQuery q(db_); q.prepare(QStringLiteral("DELETE FROM images WHERE path=?"));
        for (const QString& path:relativePaths) {
            q.bindValue(0,path);
            if (!q.exec()) { error_=q.lastError().text(); db_.rollback(); return false; }
        }
        return db_.commit();
    }
private:
    QString connectionName_;
    QSqlDatabase db_;
    QString error_;
};

struct ScanResult {
    std::vector<ImgMeta> images;
    QString error;
    QString cacheWarning;
    int discovered=0;
    int cacheHits=0;
    bool cancelled=false;
};

struct GroupResult {
    std::vector<ImgMeta> images;
    std::vector<std::vector<int>> groups;
    std::shared_ptr<BKTree<BitHash>> tree;
    QString error;
    QString cacheWarning;
    HashAlgo algo=HashAlgo::MD5;
    bool cancelled=false;
};

struct BulkDeleteResult {
    QStringList deleted;
    QStringList failed;
    bool cancelled=false;
};

// ---------- FlowLayout (image grid layout) ----------

class FlowLayout : public QLayout {
public:
    explicit FlowLayout(QWidget* parent,int margin=-1,int h=-1,int v=-1)
        : QLayout(parent), h_(h), v_(v) { setContentsMargins(margin,margin,margin,margin); }
    explicit FlowLayout(int margin=-1,int h=-1,int v=-1)
        : h_(h), v_(v) { setContentsMargins(margin,margin,margin,margin); }
    ~FlowLayout() override { while(auto* it=takeAt(0)) delete it; }
    void addItem(QLayoutItem* it) override { items_.append(it); }
    int count() const override { return items_.size(); }
    QLayoutItem* itemAt(int i) const override { return items_.value(i); }
    QLayoutItem* takeAt(int i) override { return (i>=0 && i<items_.size()) ? items_.takeAt(i) : nullptr; }
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int  heightForWidth(int w) const override { return doLayout(QRect(0,0,w,0),true); }
    void setGeometry(const QRect& r) override { QLayout::setGeometry(r); doLayout(r,false); }
    QSize sizeHint() const override { return minimumSize(); }
    QSize minimumSize() const override {
        QSize s; for (const QLayoutItem* it: items_) s = s.expandedTo(it->minimumSize());
        const auto m = contentsMargins(); s += QSize(m.left()+m.right(), m.top()+m.bottom()); return s;
    }
private:
    int smart(QStyle::PixelMetric pm) const {
        const QObject* p=parent();
        if(!p) return -1;
        if(p->isWidgetType()){ auto* w=static_cast<const QWidget*>(p); return w->style()->pixelMetric(pm,nullptr,w); }
        return static_cast<const QLayout*>(p)->spacing();
    }
    int hs() const { return h_>=0 ? h_ : smart(QStyle::PM_LayoutHorizontalSpacing); }
    int vs() const { return v_>=0 ? v_ : smart(QStyle::PM_LayoutVerticalSpacing); }
    int doLayout(const QRect& r,bool test) const {
        int l,t,rr,b; getContentsMargins(&l,&t,&rr,&b);
        QRect eff=r.adjusted(+l,+t,-rr,-b);
        int x=eff.x(), y=eff.y(), lineH=0;
        for (QLayoutItem* it: items_) {
            const QWidget* w=it->widget();
            int sx=hs(); if(sx==-1) sx=w->style()->layoutSpacing(QSizePolicy::PushButton,QSizePolicy::PushButton,Qt::Horizontal);
            int sy=vs(); if(sy==-1) sy=w->style()->layoutSpacing(QSizePolicy::PushButton,QSizePolicy::PushButton,Qt::Vertical);
            int nextX=x+it->sizeHint().width()+sx;
            if(nextX>eff.right() && lineH>0){ x=eff.x(); y+=lineH+sy; nextX=x+it->sizeHint().width()+sx; lineH=0; }
            if(!test) it->setGeometry(QRect(QPoint(x,y), it->sizeHint()));
            x=nextX; lineH=qMax(lineH, it->sizeHint().height());
        }
        return y + lineH - r.y() + b;
    }
    QList<QLayoutItem*> items_;
    int h_, v_;
};

// ---------- ThumbWidget ----------

class ThumbWidget : public QWidget {
    Q_OBJECT
public:
    ThumbWidget(ImgMeta& meta, ImgMeta& leader, HashAlgo activeAlgo, int thumbH, QWidget* parent=nullptr)
        : QWidget(parent), meta_(meta), leader_(leader), activeAlgo_(activeAlgo), thumbH_(thumbH) {

        setObjectName(QStringLiteral("thumbCard"));

        auto* v = new QVBoxLayout(this);
        v->setSpacing(2); v->setContentsMargins(2,2,2,2);

        check_ = new QCheckBox(this);
        check_->setChecked(meta_.selected);

        img_ = new QLabel(this);
        img_->setAlignment(Qt::AlignCenter);
        img_->setCursor(Qt::PointingHandCursor);
        img_->setToolTip(QStringLiteral("Click the thumbnail to select or unselect this file."));
        img_->installEventFilter(this);

        int w = isSupportedVideoFile(meta_.file) ? qRound(thumbH_ * 16.0 / 9.0) : thumbH_;
        if (meta_.resolution.isValid() && meta_.resolution.height() > 0) {
            const double aspect = double(meta_.resolution.width()) / double(meta_.resolution.height());
            w = qRound(aspect * thumbH_);
        }
        img_->setFixedSize(w, thumbH_);

        img_->setText(QStringLiteral("Loading preview…"));
        loadImage();

        v->addWidget(img_);
        v->addSpacing(6);
        v->addWidget(check_);

        auto* name = new QLabel(QFileInfo(meta_.file).fileName(), this);
        name->setAlignment(Qt::AlignCenter);
        name->setTextInteractionFlags(Qt::TextSelectableByMouse);
        name->setObjectName(QStringLiteral("thumbName"));
        v->addWidget(name);

        info_ = new QLabel(this);
        info_->setAlignment(Qt::AlignCenter);
        info_->setWordWrap(true);
        info_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        info_->setObjectName(QStringLiteral("thumbInfo"));
        updateInfo();
        v->addWidget(info_);

        setContextMenuPolicy(Qt::CustomContextMenu);
        connect(this, &QWidget::customContextMenuRequested, this, &ThumbWidget::showMenu);
        connect(check_, &QCheckBox::toggled, this, &ThumbWidget::onChecked);
    }

    ~ThumbWidget() override = default;

    bool isChecked() const { return check_->isChecked(); }
    QString filePath() const { return meta_.file; }
    void setChecked(bool b) { check_->setChecked(b); }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched==img_ && event->type()==QEvent::MouseButtonRelease) {
            const auto* mouseEvent=static_cast<QMouseEvent*>(event);
            if (mouseEvent->button()==Qt::LeftButton
                && img_->rect().contains(mouseEvent->position().toPoint())) {
                check_->toggle();
                return true;
            }
        }
        return QWidget::eventFilter(watched,event);
    }

public:
    void updateInfo() {
        if (leader_.file.isEmpty()) { info_->clear(); return; }

        const QString mediaInfo = meta_.resolution.isValid()
            ? QStringLiteral("Res: %1x%2").arg(meta_.resolution.width()).arg(meta_.resolution.height())
            : isSupportedVideoFile(meta_.file) ? QStringLiteral("Video thumbnail")
                                                : QStringLiteral("Resolution unavailable");
        const QString size = QStringLiteral("Size: %1 MB").arg(QString::number(meta_.size / 1024.0 / 1024.0, 'f', 2));

        QString similarity=QStringLiteral("—");
        if (meta_.file==leader_.file) similarity=QStringLiteral("100% • representative");
        else if (activeAlgo_==HashAlgo::MD5 && !meta_.md5.isEmpty() && !leader_.md5.isEmpty())
            similarity=(meta_.md5==leader_.md5) ? QStringLiteral("100% • exact") : QStringLiteral("0%");
        else if (hasHash(meta_,activeAlgo_) && hasHash(leader_,activeAlgo_))
            similarity=QStringLiteral("%1% similar").arg(100-(hamming(pickHash(meta_,activeAlgo_),pickHash(leader_,activeAlgo_))*100/kHashBits));

        QStringList parts;
        parts << QStringLiteral("%1 • %2").arg(mediaInfo,size)
              << QStringLiteral("%1: %2").arg(QString::fromLatin1(algoName(activeAlgo_)),similarity);

        if (!gifInfo_.isEmpty()) parts << gifInfo_;
        info_->setText(parts.join('\n'));
    }

    void startMovie(){ if(movie_) movie_->start(); }
    void stopMovie(){ if(movie_) movie_->stop(); }
    void unloadMovie(){
        if (movie_) { movie_->stop(); img_->setMovie(nullptr); delete movie_; movie_ = nullptr; }
        else { img_->clear(); }
    }

signals:
    void checkedChanged(bool);
    void requestDelete(const QString&);

private slots:
    void onChecked(bool on){ emit checkedChanged(on); }
    void showMenu(const QPoint& pos){
        QMenu m(this);
        m.addAction(QStringLiteral("Open file"), [this]{ QDesktopServices::openUrl(QUrl::fromLocalFile(meta_.file)); });
        m.addAction(QStringLiteral("Open folder"), [this]{
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(meta_.file).absolutePath()));
        });
        m.addAction(QStringLiteral("Delete file"), [this]{
            if (QMessageBox::question(this, QStringLiteral("Confirm Delete"), QStringLiteral("Delete this file?"),
                                      QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
                emit requestDelete(meta_.file);
            }
        });
        m.exec(mapToGlobal(pos));
    }

private:
    QString gifPlaytimeInfo(QMovie* mv){
        if (!mv) return {};
        const int n=mv->frameCount();
        return n>1 ? QStringLiteral("Animated GIF: %1 frames").arg(n)
                   : QStringLiteral("Animated GIF");
    }
    void loadImage(){
        const bool isGif = meta_.file.endsWith(QStringLiteral(".gif"), Qt::CaseInsensitive);
        const bool bigGif = isGif && meta_.size > (120ll<<20);

        if (isGif && !bigGif){
            movie_ = new QMovie(meta_.file, QByteArray(), this);
            if (movie_->isValid()){
                movie_->setCacheMode(QMovie::CacheNone);
                movie_->setScaledSize(img_->size());
                img_->setMovie(movie_);
                gifInfo_ = gifPlaytimeInfo(movie_);
                return;
            }
            delete movie_; movie_=nullptr;
        }
        const QString path=meta_.file;
        const int height=thumbH_;
        auto* watcher=new QFutureWatcher<QImage>(this);
        connect(watcher,&QFutureWatcher<QImage>::finished,this,[this,watcher,bigGif]{
            const QImage image=watcher->result(); watcher->deleteLater();
            if (image.isNull()) {
                img_->setText(isSupportedVideoFile(meta_.file)
                                  ? QStringLiteral("Video thumbnail\nunavailable")
                                  : QStringLiteral("Preview unavailable"));
                img_->setStyleSheet(QStringLiteral("color:#f87171;"));
            }
            else { img_->setText({}); img_->setPixmap(QPixmap::fromImage(image)); }
            if (bigGif) { gifInfo_=QStringLiteral("Large GIF • animation disabled"); updateInfo(); }
        });
        watcher->setFuture(QtConcurrent::run([path,height]{
            return decodeImage(path,QSize(0,height),Qt::KeepAspectRatio);
        }));
    }
    ImgMeta& meta_;
    ImgMeta& leader_;
    HashAlgo activeAlgo_;
    int thumbH_;
    QLabel* img_{};
    QLabel* info_{};
    QCheckBox* check_{};
    QMovie* movie_ = nullptr;
    QString gifInfo_;
};

// ---------- Main Window (reworked to split left controls + right grid) ----------

class DupeGemMainWindow : public QMainWindow {
    Q_OBJECT
public:
    DupeGemMainWindow() {
        setWindowTitle(QStringLiteral("DupeGem — Duplicate Image Finder"));
        const QString iconPath=QCoreApplication::applicationDirPath()+QStringLiteral("/icon.png");
        if (QFileInfo::exists(iconPath)) setWindowIcon(QIcon(iconPath));
        setMinimumSize(1280, 980);
        resize(1600, 1000);

        // ==== TOP-LEVEL SPLITTER (Left controls | Right grid) ====
        auto* mainSplit = new QSplitter(Qt::Horizontal, this);
        setCentralWidget(mainSplit);

        // ==== LEFT: vertical splitter so user can size sections ====
        leftSplit_ = new QSplitter(Qt::Vertical, mainSplit);
        leftSplit_->setObjectName(QStringLiteral("leftSplit"));

        // Controls panel (all rows)
        controlsW_ = new QFrame;
        controlsW_->setObjectName(QStringLiteral("sidePanel"));
        auto* ctl = new QVBoxLayout(controlsW_);
        ctl->setContentsMargins(18,18,18,18);
        ctl->setSpacing(10);

        auto* brand=new QLabel(QStringLiteral("DupeGem"));
        brand->setObjectName(QStringLiteral("brandTitle"));
        auto* tagline=new QLabel(QStringLiteral("Fast, local duplicate image review"));
        tagline->setObjectName(QStringLiteral("brandSubtitle"));
        ctl->addWidget(brand); ctl->addWidget(tagline);

        auto addSection=[ctl](const QString& text){
            auto* label=new QLabel(text.toUpper()); label->setObjectName(QStringLiteral("sectionTitle"));
            ctl->addSpacing(8); ctl->addWidget(label);
        };
        addSection(QStringLiteral("Library"));

        // Row 1: Select folder + scan subfolders
        {
            auto* row = new QHBoxLayout;
            btnSelect_ = new QPushButton(QStringLiteral("Select Folder…"));
            btnSelect_->setObjectName(QStringLiteral("primaryButton"));
            scanSubs_  = new QCheckBox(QStringLiteral("Scan Subfolders"));
            row->addWidget(btnSelect_, 1);
            row->addWidget(scanSubs_, 0);
            ctl->addLayout(row);
        }
        folderLabel_=new QLabel(QStringLiteral("No folder selected"));
        folderLabel_->setObjectName(QStringLiteral("mutedLabel"));
        folderLabel_->setWordWrap(true); ctl->addWidget(folderLabel_);

        // Row 2: Algorithm selector
        addSection(QStringLiteral("Matching"));
        {
            auto* row = new QHBoxLayout;
            row->addWidget(new QLabel(QStringLiteral("Algorithm:")));
            algoCombo_ = new QComboBox;
            for (const HashAlgo algo:kAlgorithmDisplayOrder) {
                algoCombo_->addItem(QStringLiteral("%1 — %2")
                                    .arg(QString::fromLatin1(algoName(algo)),
                                         QString::fromLatin1(algoSpeed(algo))),
                                    int(algo));
            }
            row->addWidget(algoCombo_, 1);
            ctl->addLayout(row);
        }

        algoInfo_ = new QLabel;
        algoInfo_->setObjectName(QStringLiteral("infoCard"));
        algoInfo_->setWordWrap(true);
        algoInfo_->setTextFormat(Qt::RichText);
        ctl->addWidget(algoInfo_);

        // Row 3: perceptual threshold, replaced by file categories in MD5 mode.
        {
            thresholdRow_ = new QWidget;
            auto* row = new QHBoxLayout(thresholdRow_);
            row->setContentsMargins(0,0,0,0);
            row->addWidget(new QLabel(QStringLiteral("Threshold:")));
            thrSlider_ = new QSlider(Qt::Horizontal);
            thrSlider_->setRange(0,64);
            thrSlider_->setValue(4);
            thrSlider_->setToolTip(QStringLiteral(
                "Maximum bit differences allowed. Lower values are stricter; 0 requires an identical visual hash."));
            row->addWidget(thrSlider_, 1);
            thrLabel_ = new QLabel(QStringLiteral("4 (min 98%)"));
            row->addWidget(thrLabel_);
            ctl->addWidget(thresholdRow_);
        }
        {
            md5TypesRow_ = new QWidget;
            auto* row = new QHBoxLayout(md5TypesRow_);
            row->setContentsMargins(0,0,0,0);
            row->addWidget(new QLabel(QStringLiteral("Include:")));
            md5Images_ = new QCheckBox(QStringLiteral("Images"));
            md5Videos_ = new QCheckBox(QStringLiteral("Videos"));
            md5Other_  = new QCheckBox(QStringLiteral("Other"));
            md5Images_->setChecked(true);
            const QString tip=QStringLiteral(
                "Choose which file categories Exact MD5 scans. Any combination is allowed.");
            for (QCheckBox* box:{md5Images_,md5Videos_,md5Other_}) box->setToolTip(tip);
            md5Other_->setToolTip(QStringLiteral(
                "Files that are neither recognized images nor common video formats. DupeGem cache files are excluded."));
            row->addWidget(md5Images_);
            row->addWidget(md5Videos_);
            row->addWidget(md5Other_);
            row->addStretch(1);
            md5TypesRow_->setVisible(false);
            ctl->addWidget(md5TypesRow_);
        }

        // Row 4: Thumbnail size slider (own row)
        {
            auto* row = new QHBoxLayout;
            row->addWidget(new QLabel(QStringLiteral("Thumb Height:")));
            sizeSlider_ = new QSlider(Qt::Horizontal);
            sizeSlider_->setRange(140, 720);
            sizeSlider_->setValue(240);
            row->addWidget(sizeSlider_, 1);
            thumbLabel_ = new QLabel(QStringLiteral("240 px"));
            row->addWidget(thumbLabel_);
            ctl->addLayout(row);
        }

        // Row 5: Regroup + show singles
        {
            auto* row = new QHBoxLayout;
            btnRegroup_ = new QPushButton(QStringLiteral("Rescan/Regroup Images"));
            showSingles_ = new QCheckBox(QStringLiteral("Show Singles"));
            row->addWidget(btnRegroup_, 1);
            row->addWidget(showSingles_, 0);
            ctl->addLayout(row);
        }

        // Row 6: Status + progress
        {
            auto* row = new QHBoxLayout;
            statusTxt_ = new QLabel(QStringLiteral("Ready."));
            statusTxt_->setObjectName(QStringLiteral("statusText"));
            prog_ = new QProgressBar;
            prog_->setVisible(false);
            prog_->setMaximum(0); // indeterminate when shown w/out max
            prog_->setTextVisible(true);
            btnCancel_=new QPushButton(QStringLiteral("Cancel"));
            btnCancel_->setObjectName(QStringLiteral("quietButton"));
            btnCancel_->setVisible(false);
            row->addWidget(statusTxt_, 1); row->addWidget(btnCancel_); row->addWidget(prog_, 1);
            ctl->addLayout(row);
        }

        // Row 7: Delete button (emphasized, padded, tall)
{
    // Wrap the button in a small container so we can add vertical padding
    auto* deleteWrap = new QWidget;
    auto* deleteVL   = new QVBoxLayout(deleteWrap);
    deleteVL->setContentsMargins(0, 8, 0, 4);
    deleteVL->setSpacing(0);

    auto* deleteRow = new QHBoxLayout;
    deleteRow->setContentsMargins(0, 0, 0, 0);
    deleteRow->setSpacing(8);

    btnDelete_ = new QPushButton(QStringLiteral("Delete Selected"));
    btnDelete_->setObjectName(QStringLiteral("dangerButton"));
    btnDelete_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    // Make it visually larger & bolder
    QFont df = btnDelete_->font();
    df.setBold(true);
    df.setPointSizeF(df.pointSizeF() * 1.15); // slightly larger text
    btnDelete_->setFont(df);

    // Double the button's height
    btnDelete_->setMinimumHeight(btnDelete_->sizeHint().height() * 1.2);

    btnDeleteAllGroups_ = new QPushButton(QStringLiteral("Delete From All Groups…"));
    btnDeleteAllGroups_->setObjectName(QStringLiteral("secondaryDangerButton"));
    btnDeleteAllGroups_->setToolTip(QStringLiteral(
        "MD5 only: keep one file in every exact-match group and move the rest to the Recycle Bin."));
    btnDeleteAllGroups_->setFont(df);
    btnDeleteAllGroups_->setMinimumHeight(btnDelete_->minimumHeight());
    btnDeleteAllGroups_->setVisible(false);

    deleteRow->addWidget(btnDelete_, 1);
    deleteRow->addWidget(btnDeleteAllGroups_, 1);
    deleteVL->addLayout(deleteRow);
    ctl->addWidget(deleteWrap);
}

        // Row 8: Search & Filter help text
        addSection(QStringLiteral("Find & organize"));
        {
            auto* help = new QLabel(
                QStringLiteral("<b>Search:</b> filters filenames in current group. "
                               "<br><b>Filter:</b> use <code>min:N</code>, <code>minmb:MB</code>, <code>size&gt;=MB</code>, and free text."));
            help->setWordWrap(true);
            help->setTextFormat(Qt::RichText);
            ctl->addWidget(help);
        }

        // Row 9: Search box
        {
            auto* row = new QHBoxLayout;
            search_ = new QLineEdit;
            search_->setPlaceholderText(QStringLiteral("Search filenames…  (Ctrl+F)"));
            search_->setClearButtonEnabled(true);
            row->addWidget(search_);
            ctl->addLayout(row);
        }

        // Row 10: Filter groups box
        {
            auto* row = new QHBoxLayout;
            groupFilter_ = new QLineEdit;
            groupFilter_->setPlaceholderText(QStringLiteral("Filter groups: e.g. min:3 size>=50 cat"));
            groupFilter_->setClearButtonEnabled(true);
            row->addWidget(groupFilter_);
            ctl->addLayout(row);
        }

        // Row 11: Sort selector
        {
            auto* row = new QHBoxLayout;
            row->addWidget(new QLabel(QStringLiteral("Sort groups by:")));
            sortCombo_ = new QComboBox;
            sortCombo_->addItem(QStringLiteral("Group #"));
            sortCombo_->addItem(QStringLiteral("File Count"));
            sortCombo_->addItem(QStringLiteral("Total Size (MB)"));
            row->addWidget(sortCombo_, 1);
            ctl->addLayout(row);
        }

        // Add a tiny spacer to avoid vertical stretching of the last control
        ctl->addStretch(1);

        // Groups list section (its own resizable pane)
        groupsW_ = new QFrame;
        groupsW_->setObjectName(QStringLiteral("sideSection"));
        auto* groupsVL = new QVBoxLayout(groupsW_);
        groupsVL->setContentsMargins(6,6,6,6);
        groupsVL->setSpacing(6);

        headerLabel_ = new QLabel(QStringLiteral("Duplicate groups"));
        headerLabel_->setObjectName(QStringLiteral("panelTitle"));
        headerLabel_->setWordWrap(true);
        groupsVL->addWidget(headerLabel_);
        libraryStats_=new QLabel(QStringLiteral("Scan a library to begin"));
        libraryStats_->setObjectName(QStringLiteral("mutedLabel"));
        groupsVL->addWidget(libraryStats_);

        groupsList_ = new QListWidget;
        groupsList_->setSelectionMode(QAbstractItemView::SingleSelection);
        groupsVL->addWidget(groupsList_, 1);

        // Keep the complete control deck stable and fully visible. The window
        // minimum size guarantees the fixed deck never needs a scrollbar.
        controlsW_->setFixedSize(514,800);
        controlsW_->setSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed);
        leftSplit_->setChildrenCollapsible(false);
        leftSplit_->addWidget(controlsW_);
        leftSplit_->addWidget(groupsW_);
        leftSplit_->setCollapsible(0,false);
        leftSplit_->setCollapsible(1,false);
        leftSplit_->setStretchFactor(0,0);
        leftSplit_->setStretchFactor(1,1);

        // The groups list owns all space beneath the fixed control deck.
        QList<int> initSizes;
        initSizes << 800     // fixed settings-panel height
                  << 120;    // groups pane receives remaining height
        leftSplit_->setSizes(initSizes);
        QTimer::singleShot(0,this,[this]{
            const int groupHeight=std::max(80,leftSplit_->height()-800-leftSplit_->handleWidth());
            leftSplit_->setSizes({800,groupHeight});
        });

        // ==== RIGHT: image grid viewer ====
        auto* rightW = new QWidget;
        rightW->setObjectName(QStringLiteral("galleryPanel"));
        auto* rightV = new QVBoxLayout(rightW);
        rightV->setContentsMargins(18,18,18,18);
        rightV->setSpacing(12);

        auto* galleryHead=new QHBoxLayout;
        galleryTitle_=new QLabel(QStringLiteral("Choose a duplicate group"));
        galleryTitle_->setObjectName(QStringLiteral("galleryTitle"));
        selectionLabel_=new QLabel(QStringLiteral("0 selected"));
        selectionLabel_->setObjectName(QStringLiteral("selectionChip"));
        galleryHead->addWidget(galleryTitle_,1); galleryHead->addWidget(selectionLabel_);
        rightV->addLayout(galleryHead);

        thumbContainer_ = new QWidget;
        flow_ = new FlowLayout(thumbContainer_, 0, 12, 12);

        scroll_ = new QScrollArea;
        scroll_->setWidgetResizable(true);
        scroll_->setWidget(thumbContainer_);
        connect(scroll_->verticalScrollBar(),&QScrollBar::valueChanged,this,[this]{ loadMoreThumbsIfNeeded(); });
        connect(scroll_->verticalScrollBar(),&QScrollBar::rangeChanged,this,[this]{ loadMoreThumbsIfNeeded(); });

        rightV->addWidget(scroll_, 1);

        // Add the two panes to the main splitter
        mainSplit->addWidget(leftSplit_);
        mainSplit->addWidget(rightW);
        leftSplit_->setFixedWidth(514);
        mainSplit->setCollapsible(0,false);
        mainSplit->setStretchFactor(0, 0); // left column
        mainSplit->setStretchFactor(1, 1); // right (grid) grows more
        mainSplit->setSizes({514,1086});

        // Shortcuts
        auto* scSelAll = new QShortcut(QKeySequence::SelectAll, this);
        auto* scClear  = new QShortcut(QKeySequence(QStringLiteral("Ctrl+D")), this);
        auto* scFocus  = new QShortcut(QKeySequence::Find, this);
        connect(scSelAll, &QShortcut::activated, this, [this]{ setAllThumbsChecked(true); });
        connect(scClear,  &QShortcut::activated, this, [this]{ setAllThumbsChecked(false); });
        connect(scFocus,  &QShortcut::activated, search_, qOverload<>(&QWidget::setFocus));

		// Delete key -> same as clicking "Delete Selected"
auto* scDel = new QShortcut(QKeySequence::Delete, this);
connect(scDel, &QShortcut::activated, this, [this]{
    // don't nuke files while the user is typing
    if (qobject_cast<QLineEdit*>(QApplication::focusWidget())) return;
    deleteSelected();
});

// (optional) also map Backspace to delete, same guard
auto* scBksp = new QShortcut(QKeySequence(Qt::Key_Backspace), this);
connect(scBksp, &QShortcut::activated, this, [this]{
    if (qobject_cast<QLineEdit*>(QApplication::focusWidget())) return;
    deleteSelected();
});

        // Wire up actions
        connect(btnSelect_, &QPushButton::clicked, this, &DupeGemMainWindow::selectFolder);
        connect(btnRegroup_,&QPushButton::clicked, this, &DupeGemMainWindow::rescanOrRegroup);
        connect(btnDelete_, &QPushButton::clicked, this, &DupeGemMainWindow::deleteSelected);
        connect(btnDeleteAllGroups_, &QPushButton::clicked, this, &DupeGemMainWindow::deleteAllMd5Duplicates);
        connect(btnCancel_,&QPushButton::clicked,this,[this]{ cancelled_=true; statusTxt_->setText(QStringLiteral("Cancelling safely…")); btnCancel_->setEnabled(false); });

        connect(groupsList_, &QListWidget::currentRowChanged, this, &DupeGemMainWindow::groupChosen);
        connect(sortCombo_,  qOverload<int>(&QComboBox::currentIndexChanged), this, &DupeGemMainWindow::resortGroups);
        filterTimer_=new QTimer(this); filterTimer_->setSingleShot(true); filterTimer_->setInterval(180);
        searchTimer_=new QTimer(this); searchTimer_->setSingleShot(true); searchTimer_->setInterval(180);
        md5TypeTimer_=new QTimer(this); md5TypeTimer_->setSingleShot(true); md5TypeTimer_->setInterval(250);
        connect(filterTimer_,&QTimer::timeout,this,&DupeGemMainWindow::applyGroupFilter);
        connect(searchTimer_,&QTimer::timeout,this,[this]{ showGroup(currentGroup_); });
        connect(md5TypeTimer_,&QTimer::timeout,this,[this]{
            if (currentAlgorithm()==HashAlgo::MD5
                && !busy_.load() && !currentDir_.isEmpty()) startScan(currentDir_);
        });
        connect(groupFilter_,&QLineEdit::textChanged,this,[this]{ filterTimer_->start(); });
        connect(algoCombo_,  qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int){
            updateAlgorithmUi();
            if(!currentDir_.isEmpty() && !busy_.load()) startScan(currentDir_);
        });
        auto categoryChanged=[this](bool){
            if (currentAlgorithm()==HashAlgo::MD5)
                md5TypeTimer_->start();
        };
        connect(md5Images_,&QCheckBox::toggled,this,categoryChanged);
        connect(md5Videos_,&QCheckBox::toggled,this,categoryChanged);
        connect(md5Other_, &QCheckBox::toggled,this,categoryChanged);
        connect(showSingles_, &QCheckBox::toggled, this, &DupeGemMainWindow::regroup);
        connect(thrSlider_,   &QSlider::valueChanged, this, &DupeGemMainWindow::updateThrLabel);
        connect(sizeSlider_,  &QSlider::valueChanged, this, [this](int v){
            thumbHeight_ = v; thumbLabel_->setText(QStringLiteral("%1 px").arg(v)); showGroup(currentGroup_);
        });
        connect(search_,&QLineEdit::textChanged,this,[this]{ searchTimer_->start(); });

        addTimer_ = new QTimer(this);
        addTimer_->setSingleShot(true);
        addTimer_->setInterval(30);
        connect(addTimer_, &QTimer::timeout, this, &DupeGemMainWindow::addThumbBatch);

        updateThrLabel();
        updateAlgorithmUi();
        applyModernTheme();
    }

    void scanFolderOnLaunch(const QString& path, int algorithm=-1) {
        QTimer::singleShot(0,this,[this,path,algorithm]{
            if (algorithm>=0 && algorithm<int(HashAlgo::Count)) {
                const int index=algoCombo_->findData(algorithm);
                if (index>=0) algoCombo_->setCurrentIndex(index);
            }
            if (QDir(path).exists() && !busy_) startScan(QDir(path).absolutePath());
        });
    }

    ~DupeGemMainWindow() override {
        addTimer_->stop();
        unloadMovies();
        clearThumbs();
        currentThumbs_.clear();
        cancelled_ = true;
        if (scanWatcher_) scanWatcher_->future().waitForFinished();
        if (groupWatcher_) groupWatcher_->future().waitForFinished();
        if (bulkDeleteWatcher_) bulkDeleteWatcher_->future().waitForFinished();
    }
protected:
    void closeEvent(QCloseEvent* ev) override {
        if (scanning_ || busy_.load()) {
            if (QMessageBox::question(this, QStringLiteral("Operation in Progress"),
                                      QStringLiteral("An operation is currently in progress. Exit anyway?"),
                                      QMessageBox::Yes | QMessageBox::No) == QMessageBox::No) {
                ev->ignore(); return;
            }
            cancelled_ = true;
            statusTxt_->setText(QStringLiteral("Cancelling..."));
        }
        ev->accept();
    }

private:
    // === Core flow ===
    void selectFolder() {
        if (busy_.load()) return;
        const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Select Folder"));
        if (dir.isEmpty()) return;
        startScan(dir);
    }

    void startScan(const QString& dir) {
        busy_=true; scanning_=true; cancelled_=false;
        currentDir_ = dir;
        cacheFile_  = currentDir_ + QStringLiteral("/.dupegem_cache.sqlite");
        folderLabel_->setText(QDir::toNativeSeparators(currentDir_));

        clearThumbs();
        currentGroup_ = -1;
        images_.clear(); groups_.clear(); trees_.clear(); groupsList_->clear(); groupIdxSorted_.clear();
        setInteractive(false);
        prog_->setRange(0,0); prog_->setVisible(true);
        statusTxt_->setText(QStringLiteral("Enumerating files..."));

        const auto algo=currentAlgorithm();
        const bool includeImages=algo!=HashAlgo::MD5 || md5Images_->isChecked();
        const bool includeVideos=algo==HashAlgo::MD5 && md5Videos_->isChecked();
        const bool includeOther =algo==HashAlgo::MD5 && md5Other_->isChecked();
        QSet<QString> imageTypes, videoTypes;
        for (const QString& extension:supportedImageExtensions()) imageTypes.insert(extension.toLower());
        for (const QString& extension:supportedVideoExtensions()) videoTypes.insert(extension.toLower());
        QStringList filters;
        auto addFilters=[&filters](const QSet<QString>& extensions){
            for (const QString& extension:extensions)
                filters << "*."+extension.toLower() << "*."+extension.toUpper();
        };
        if (includeOther) filters << QStringLiteral("*");
        else {
            if (includeImages) addFilters(imageTypes);
            if (includeVideos) addFilters(videoTypes);
        }
        if (filters.isEmpty()) filters << QStringLiteral("__dupegem_no_selected_file_types__");
        filters.removeDuplicates();
        const bool recurse=scanSubs_->isChecked();
        const QString cachePath=cacheFile_;
        QPointer<DupeGemMainWindow> self(this);
        auto post=[self](const QString& text,int value=-1,int maximum=-1){
            if (!self) return;
            QMetaObject::invokeMethod(self,[self,text,value,maximum]{
                if (!self) return;
                self->statusTxt_->setText(text);
                if (maximum>=0) { self->prog_->setRange(0,maximum); self->prog_->setValue(value); }
                else self->prog_->setRange(0,0);
            },Qt::QueuedConnection);
        };

        auto* watcher=new QFutureWatcher<ScanResult>(this);
        scanWatcher_=watcher;
        connect(watcher,&QFutureWatcher<ScanResult>::finished,this,[this,watcher,algo]{
            ScanResult result=watcher->future().takeResult();
            watcher->deleteLater(); scanWatcher_=nullptr; scanning_=false;
            if (result.cancelled || cancelled_) {
                busy_=false; prog_->setVisible(false); setInteractive(true);
                statusTxt_->setText(QStringLiteral("Scan cancelled.")); return;
            }
            if (!result.error.isEmpty()) {
                busy_=false; prog_->setVisible(false); setInteractive(true);
                QMessageBox::critical(this,QStringLiteral("Scan failed"),result.error); return;
            }
            images_=std::move(result.images);
            if (!result.cacheWarning.isEmpty()) statusTxt_->setText(QStringLiteral("Cache unavailable: %1").arg(result.cacheWarning));
            else statusTxt_->setText(QStringLiteral("%1 %2 • %3 cache hits")
                .arg(images_.size()).arg(algo==HashAlgo::MD5 ? QStringLiteral("files") : QStringLiteral("images"))
                .arg(result.cacheHits));
            startGrouping();
        });

        watcher->setFuture(QtConcurrent::run([dir,cachePath,filters,recurse,algo,post,this,
                                               imageTypes,videoTypes,includeImages,includeVideos,includeOther]{
            ScanResult out;
            try {
                SqliteCache cache(cachePath);
                QHash<QString,ImgMeta> cached=cache.loadAll();
                if (!cache.isOpen()) out.cacheWarning=cache.error();
                QStringList files;
                const auto flags=recurse ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags;
                QDirIterator it(dir,filters,QDir::Files|QDir::Readable,flags);
                while (it.hasNext()) {
                    if (cancelled_) { out.cancelled=true; return out; }
                    const QString path=it.next();
                    const QFileInfo candidate(path);
                    const QString name=candidate.fileName();
                    if (name.startsWith(QStringLiteral(".dupegem_cache."),Qt::CaseInsensitive)) continue;
                    const QString suffix=candidate.suffix().toLower();
                    const bool image=imageTypes.contains(suffix);
                    const bool video=!image && videoTypes.contains(suffix);
                    const bool accepted=algo!=HashAlgo::MD5 ? image
                        : (image ? includeImages : (video ? includeVideos : includeOther));
                    if (!accepted) continue;
                    files<<path;
                    if ((files.size()&2047)==0)
                        post(QStringLiteral("Discovering %1... %2")
                            .arg(algo==HashAlgo::MD5 ? QStringLiteral("files") : QStringLiteral("images"))
                            .arg(files.size()));
                }
                std::sort(files.begin(),files.end(),[](const QString& a,const QString& b){
                    return QString::compare(a,b,Qt::CaseInsensitive)<0;
                });
                out.discovered=files.size();
                out.images.resize(size_t(files.size()));
                std::vector<int> pending; pending.reserve(files.size());
                const QDir root(dir);
                for (qsizetype i=0;i<files.size();++i) {
                    QFileInfo fi(files[i]);
                    const QString rel=root.relativeFilePath(files[i]);
                    auto found=cached.find(rel);
                    ImgMeta m;
                    if (found!=cached.end() && found->mtime==fi.lastModified().toMSecsSinceEpoch() && found->size==fi.size()) {
                        m=*found; ++out.cacheHits;
                    }
                    if (found!=cached.end()) cached.erase(found);
                    m.file=files[i]; m.size=fi.size(); m.mtime=fi.lastModified().toMSecsSinceEpoch(); m.valid=true;
                    out.images[size_t(i)]=std::move(m);
                    if (algo!=HashAlgo::MD5 && !hasHash(out.images[size_t(i)],algo)) pending.push_back(int(i));
                }
                post(QStringLiteral("Preparing %1 %2...").arg(files.size())
                    .arg(algo==HashAlgo::MD5 ? QStringLiteral("files") : QStringLiteral("images")),0,int(pending.size()));
                QThreadPool pool; pool.setMaxThreadCount(workerCount());
                std::atomic<int> done{0};
                int checkpointed=0;
                constexpr size_t checkpointSize=1000;
                for (size_t base=0;base<pending.size();base+=checkpointSize) {
                    const size_t end=std::min(base+checkpointSize,pending.size());
                    std::vector<char> completed(end-base,0);
                    for (size_t pos=base;pos<end;++pos) {
                        const int idx=pending[pos];
                        pool.start(new FunctionTask([&,idx,pos,base]{
                            if (!cancelled_) {
                                try { out.images[size_t(idx)].valid=readAndHash(out.images[size_t(idx)],algo); }
                                catch (...) { out.images[size_t(idx)].valid=false; }
                                completed[pos-base]=1;
                            }
                            const int d=++done;
                            if ((d&63)==0 || d==int(pending.size()))
                                post(QStringLiteral("Hashing %1 • %2 / %3").arg(QString::fromLatin1(algoName(algo))).arg(d).arg(pending.size()),d,int(pending.size()));
                        }));
                    }
                    pool.waitForDone();
                    std::vector<int> checkpoint;
                    checkpoint.reserve(end-base);
                    for (size_t pos=base;pos<end;++pos)
                        if (completed[pos-base] && out.images[size_t(pending[pos])].valid) checkpoint.push_back(pending[pos]);
                    if (cache.isOpen() && !checkpoint.empty() && !cache.saveRows(dir,out.images,checkpoint)) out.cacheWarning=cache.error();
                    checkpointed+=int(checkpoint.size());
                    if (!checkpoint.empty()) post(QStringLiteral("Saved checkpoint • %1 hashes retained").arg(checkpointed),done.load(),int(pending.size()));
                    if (cancelled_) { out.cancelled=true; return out; }
                }
                out.images.erase(std::remove_if(out.images.begin(),out.images.end(),[](const ImgMeta& m){return !m.valid;}),out.images.end());
                if (cache.isOpen() && !cached.isEmpty()) {
                    QStringList missing;
                    for (auto it=cached.cbegin();it!=cached.cend();++it)
                        if (!QFileInfo::exists(root.absoluteFilePath(it.key()))) missing << it.key();
                    if (!missing.isEmpty() && !cache.removePaths(missing)) out.cacheWarning=cache.error();
                }
            } catch (const std::exception& e) { out.error=QString::fromLocal8Bit(e.what()); }
            catch (...) { out.error=QStringLiteral("Unknown background scan failure."); }
            return out;
        }));
    }

    void startGrouping() {
        const auto algo=currentAlgorithm();
        if (images_.empty()) {
            busy_=false; prog_->setVisible(false); setInteractive(true);
            headerLabel_->setText(algo==HashAlgo::MD5
                ? QStringLiteral("No files match the selected MD5 categories.")
                : QStringLiteral("No readable images found."));
            libraryStats_->setText(QStringLiteral("0 %1").arg(
                algo==HashAlgo::MD5 ? QStringLiteral("files") : QStringLiteral("images")));
            return;
        }
        clearThumbs(); groupsList_->clear(); groupIdxSorted_.clear();
        currentGroup_=-1; cancelled_=false; setInteractive(false);
        prog_->setVisible(true); prog_->setRange(0,0);
        const int threshold=thrSlider_->value();
        const bool showSingles=showSingles_->isChecked();
        const QString root=currentDir_, cachePath=cacheFile_;
        std::shared_ptr<BKTree<BitHash>> existing;
        auto treeIt=trees_.find(algo); if (treeIt!=trees_.end()) existing=treeIt->second;
        auto work=std::move(images_);
        QPointer<DupeGemMainWindow> self(this);
        auto post=[self](const QString& text,int value=-1,int maximum=-1){
            if (!self) return;
            QMetaObject::invokeMethod(self,[self,text,value,maximum]{
                if (!self) return;
                self->statusTxt_->setText(text);
                if (maximum>=0) { self->prog_->setRange(0,maximum); self->prog_->setValue(value); }
                else self->prog_->setRange(0,0);
            },Qt::QueuedConnection);
        };
        auto* watcher=new QFutureWatcher<GroupResult>(this);
        groupWatcher_=watcher;
        connect(watcher,&QFutureWatcher<GroupResult>::finished,this,[this,watcher]{
            GroupResult result=watcher->future().takeResult();
            watcher->deleteLater(); groupWatcher_=nullptr;
            images_=std::move(result.images);
            if (result.cancelled || cancelled_) {
                busy_=false; prog_->setVisible(false); setInteractive(true); statusTxt_->setText(QStringLiteral("Operation cancelled.")); return;
            }
            if (!result.error.isEmpty()) {
                busy_=false; prog_->setVisible(false); setInteractive(true);
                QMessageBox::critical(this,QStringLiteral("Grouping failed"),result.error); return;
            }
            groups_=std::move(result.groups);
            if (result.tree) trees_[result.algo]=std::move(result.tree);
            prog_->setVisible(false); busy_=false; setInteractive(true);
            const bool exact=result.algo==HashAlgo::MD5;
            const QString noun=exact ? QStringLiteral("files") : QStringLiteral("images");
            statusTxt_->setText(QStringLiteral("Ready • %1 %2 • %3 groups")
                                .arg(images_.size()).arg(noun).arg(groups_.size()));
            libraryStats_->setText(QStringLiteral("%1 %2  •  %3 duplicate groups  •  %4")
                                   .arg(images_.size()).arg(noun).arg(groups_.size())
                                   .arg(exact ? QStringLiteral("exact matching")
                                              : QStringLiteral("representative matching")));
            refillGroupsList();
            if (groupsList_->count()>0) groupsList_->setCurrentRow(0);
            else { clearThumbs(); headerLabel_->setText(QStringLiteral("No duplicate groups for these settings.")); }
        });

        watcher->setFuture(QtConcurrent::run([work=std::move(work),algo,threshold,showSingles,root,cachePath,existing,post,this]() mutable {
            GroupResult out; out.images=std::move(work); out.algo=algo;
            try {
                int n=int(out.images.size());
                constexpr size_t checkpointSize=1000;
                std::unique_ptr<SqliteCache> checkpointCache;
                auto saveCheckpoint=[&](const std::vector<int>& rows){
                    if (rows.empty()) return;
                    if (!checkpointCache) checkpointCache=std::make_unique<SqliteCache>(cachePath);
                    if (checkpointCache->isOpen()) {
                        if (!checkpointCache->saveRows(root,out.images,rows)) out.cacheWarning=checkpointCache->error();
                    } else out.cacheWarning=checkpointCache->error();
                };
                if (algo==HashAlgo::MD5) {
                    QHash<qint64,QVector<int>> bySize; bySize.reserve(n*2);
                    for (int i=0;i<n;++i) bySize[out.images[size_t(i)].size].push_back(i);
                    std::vector<int> pending;
                    for (auto it=bySize.cbegin();it!=bySize.cend();++it)
                        if (it.value().size()>1) for (int i:it.value()) if (out.images[size_t(i)].md5.isEmpty()) pending.push_back(i);
                    post(QStringLiteral("Hashing exact size collisions..."),0,int(pending.size()));
                    QThreadPool pool; pool.setMaxThreadCount(workerCount()); std::atomic<int> done{0};
                    for (size_t base=0;base<pending.size();base+=checkpointSize) {
                        const size_t end=std::min(base+checkpointSize,pending.size());
                        std::vector<char> completed(end-base,0);
                        for (size_t pos=base;pos<end;++pos) {
                            const int idx=pending[pos];
                            pool.start(new FunctionTask([&,idx,pos,base]{
                                if (!cancelled_) {
                                    out.images[size_t(idx)].md5=md5ForFile(out.images[size_t(idx)].file);
                                    completed[pos-base]=1;
                                }
                                const int d=++done; if ((d&31)==0 || d==int(pending.size())) post(QStringLiteral("Exact hashing • %1 / %2").arg(d).arg(pending.size()),d,int(pending.size()));
                            }));
                        }
                        pool.waitForDone();
                        std::vector<int> checkpoint; checkpoint.reserve(end-base);
                        for (size_t pos=base;pos<end;++pos) if (completed[pos-base]) checkpoint.push_back(pending[pos]);
                        saveCheckpoint(checkpoint);
                        if (cancelled_) { out.cancelled=true; return out; }
                    }
                    QHash<QString,std::vector<int>> exact;
                    for (int i=0;i<n;++i) {
                        const auto& bucket=bySize[out.images[size_t(i)].size];
                        if (bucket.size()==1) { if (showSingles) out.groups.push_back({i}); }
                        else if (!out.images[size_t(i)].md5.isEmpty() && out.images[size_t(i)].md5!="error") exact[out.images[size_t(i)].md5].push_back(i);
                        else if (showSingles) out.groups.push_back({i});
                    }
                    for (auto it=exact.begin();it!=exact.end();++it) if (showSingles || it.value().size()>1) out.groups.push_back(std::move(it.value()));
                    std::sort(out.groups.begin(),out.groups.end(),[](const auto& a,const auto& b){return a.front()<b.front();});
                } else {
                    std::vector<int> pending;
                    for (int i=0;i<n;++i) if (!hasHash(out.images[size_t(i)],algo)) pending.push_back(i);
                    if (!pending.empty()) {
                        existing.reset();
                        post(QStringLiteral("Calculating %1 once, then caching it...").arg(QString::fromLatin1(algoName(algo))),0,int(pending.size()));
                        QThreadPool pool; pool.setMaxThreadCount(workerCount()); std::atomic<int> done{0};
                        for (size_t base=0;base<pending.size();base+=checkpointSize) {
                            const size_t end=std::min(base+checkpointSize,pending.size());
                            std::vector<char> completed(end-base,0);
                            for (size_t pos=base;pos<end;++pos) {
                                const int idx=pending[pos];
                                pool.start(new FunctionTask([&,idx,pos,base]{
                                    if (!cancelled_) {
                                        out.images[size_t(idx)].valid=readAndHash(out.images[size_t(idx)],algo);
                                        completed[pos-base]=1;
                                    }
                                    const int d=++done; if ((d&63)==0 || d==int(pending.size())) post(QStringLiteral("Lazy hashing • %1 / %2").arg(d).arg(pending.size()),d,int(pending.size()));
                                }));
                            }
                            pool.waitForDone();
                            std::vector<int> checkpoint; checkpoint.reserve(end-base);
                            for (size_t pos=base;pos<end;++pos)
                                if (completed[pos-base] && out.images[size_t(pending[pos])].valid) checkpoint.push_back(pending[pos]);
                            saveCheckpoint(checkpoint);
                            if (cancelled_) { out.cancelled=true; return out; }
                        }
                        out.images.erase(std::remove_if(out.images.begin(),out.images.end(),[](const ImgMeta& m){return !m.valid;}),out.images.end());
                        n=int(out.images.size());
                    }
                    if (!existing) {
                        post(QStringLiteral("Building similarity index..."),0,n);
                        existing=std::make_shared<BKTree<BitHash>>(hamming);
                        for (int i=0;i<n;++i) {
                            existing->add(pickHash(out.images[size_t(i)],algo),i);
                            if ((i&2047)==0) post(QStringLiteral("Building similarity index... %1 / %2").arg(i).arg(n),i,n);
                            if (cancelled_) { out.cancelled=true; return out; }
                        }
                    }
                    out.tree=existing;
                    post(QStringLiteral("Forming representative-based groups..."),0,n);
                    std::vector<bool> assigned(size_t(n),false);
                    for (int i=0;i<n;++i) {
                        if (assigned[size_t(i)]) continue;
                        std::vector<int> group{i}; assigned[size_t(i)]=true;
                        auto hits=existing->query(pickHash(out.images[size_t(i)],algo),threshold,i);
                        std::sort(hits.begin(),hits.end());
                        for (int hit:hits) if (!assigned[size_t(hit)] && hamming(pickHash(out.images[size_t(i)],algo),pickHash(out.images[size_t(hit)],algo))<=threshold) {
                            assigned[size_t(hit)]=true; group.push_back(hit);
                        }
                        if (showSingles || group.size()>1) out.groups.push_back(std::move(group));
                        if ((i&2047)==0) post(QStringLiteral("Grouping... %1 / %2").arg(i).arg(n),i,n);
                        if (cancelled_) { out.cancelled=true; return out; }
                    }
                }
                for (auto& m:out.images) m.group=-1;
                for (int g=0;g<int(out.groups.size());++g) for (int idx:out.groups[size_t(g)]) out.images[size_t(idx)].group=g;
            } catch (const std::exception& e) { out.error=QString::fromLocal8Bit(e.what()); }
            catch (...) { out.error=QStringLiteral("Unknown background grouping failure."); }
            return out;
        }));
    }

    void rescanOrRegroup() {
        if (busy_.load()) return;
        if (!currentDir_.isEmpty()) startScan(currentDir_);
        else regroup();
    }

    void regroup() {
        if (images_.empty() || busy_.load()) {
            if (images_.empty() && !busy_) QMessageBox::information(this,QStringLiteral("No Data"),QStringLiteral("Select a folder and scan first."));
            return;
        }
        busy_=true; cancelled_=false; startGrouping();
    }

    void deleteSelected() {
        if (currentGroup_ < 0 || currentGroup_ >= int(groups_.size())) return;
        QStringList toDelete;
        for (int idx:groups_[size_t(currentGroup_)])
            if (idx<int(images_.size()) && images_[size_t(idx)].selected && !images_[size_t(idx)].file.isEmpty())
                toDelete << images_[size_t(idx)].file;
        if (toDelete.isEmpty()) {
            QMessageBox::information(this, QStringLiteral("Delete Selected"), QStringLiteral("No images selected."));
            return;
        }
        if (QMessageBox::question(this, QStringLiteral("Confirm Delete"),
                                  QStringLiteral("Delete %1 selected image(s)?").arg(toDelete.size()),
                                  QMessageBox::Yes|QMessageBox::Cancel) != QMessageBox::Yes) return;

        for (const QPointer<ThumbWidget>& t: currentThumbs_) if (t && t->isChecked()) t->unloadMovie();
        QCoreApplication::processEvents();

        QStringList deleted, failed;
        for (const QString& f: toDelete) {
            if (QFile::moveToTrash(f)) {
                deleted << f;
            } else failed << f;
        }
        forgetDeletedFiles(deleted);
        if (!failed.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("Deletion Failed"),
                                 QStringLiteral("Could not delete:\n\n%1").arg(failed.join('\n')));
        }
        refreshAfterDelete();
    }

    void deleteAllMd5Duplicates() {
        if (busy_.load()) return;
        if (currentAlgorithm() != HashAlgo::MD5) {
            QMessageBox::information(this, QStringLiteral("MD5 Required"),
                                     QStringLiteral("This action is available only in Exact MD5 mode."));
            return;
        }

        int eligibleGroups=0;
        int deleteCount=0;
        for (const auto& group:groups_) {
            int valid=0;
            for (int idx:group)
                if (idx>=0 && idx<int(images_.size()) && !images_[size_t(idx)].file.isEmpty()) ++valid;
            if (valid>1) { ++eligibleGroups; deleteCount+=valid-1; }
        }
        if (!eligibleGroups) {
            QMessageBox::information(this, QStringLiteral("No Exact Duplicates"),
                                     QStringLiteral("There are no MD5 groups containing more than one existing file."));
            return;
        }

        QDialog dialog(this);
        dialog.setWindowTitle(QStringLiteral("Keep One File Per MD5 Group"));
        dialog.setMinimumWidth(540);
        auto* layout=new QVBoxLayout(&dialog);
        layout->setContentsMargins(20,18,20,18);
        layout->setSpacing(12);
        auto* summary=new QLabel(QStringLiteral(
            "<b>%1 exact-match groups</b> contain %2 extra files.<br>"
            "Choose which single file DupeGem should keep in every group.")
            .arg(eligibleGroups).arg(deleteCount));
        summary->setTextFormat(Qt::RichText);
        summary->setWordWrap(true);
        layout->addWidget(summary);

        auto* rules=new QComboBox;
        rules->addItem(QStringLiteral("Keep newest modified file"), int(Md5KeepRule::Newest));
        rules->addItem(QStringLiteral("Keep oldest modified file"), int(Md5KeepRule::Oldest));
        rules->addItem(QStringLiteral("Keep filename first (A–Z)"), int(Md5KeepRule::FilenameFirst));
        rules->addItem(QStringLiteral("Keep filename last (Z–A)"), int(Md5KeepRule::FilenameLast));
        rules->addItem(QStringLiteral("Keep shortest filename"), int(Md5KeepRule::ShortestName));
        rules->addItem(QStringLiteral("Keep longest filename"), int(Md5KeepRule::LongestName));
        layout->addWidget(rules);

        auto* explanation=new QLabel;
        explanation->setObjectName(QStringLiteral("infoCard"));
        explanation->setWordWrap(true);
        layout->addWidget(explanation);
        auto updateExplanation=[rules,explanation]{
            const auto rule=static_cast<Md5KeepRule>(rules->currentData().toInt());
            QString text;
            switch (rule) {
                case Md5KeepRule::Newest: text=QStringLiteral("Uses each file's last-modified time. Alphabetical full path breaks ties."); break;
                case Md5KeepRule::Oldest: text=QStringLiteral("Keeps the earliest last-modified file. Alphabetical full path breaks ties."); break;
                case Md5KeepRule::FilenameFirst: text=QStringLiteral("Compares filenames case-insensitively from A to Z, then compares full paths."); break;
                case Md5KeepRule::FilenameLast: text=QStringLiteral("Compares filenames case-insensitively from Z to A, then compares full paths."); break;
                case Md5KeepRule::ShortestName: text=QStringLiteral("Keeps the shortest filename. Alphabetical filename and full path break ties."); break;
                case Md5KeepRule::LongestName: text=QStringLiteral("Keeps the longest filename. Alphabetical filename and full path break ties."); break;
            }
            explanation->setText(text);
        };
        connect(rules,qOverload<int>(&QComboBox::currentIndexChanged),&dialog,[updateExplanation](int){updateExplanation();});
        updateExplanation();

        auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);
        buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Review Deletion"));
        connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);
        connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
        layout->addWidget(buttons);
        if (dialog.exec()!=QDialog::Accepted) return;

        const auto rule=static_cast<Md5KeepRule>(rules->currentData().toInt());
        const QString ruleLabel=rules->currentText();
        QStringList toDelete;
        for (const auto& group:groups_) {
            std::vector<int> valid;
            valid.reserve(group.size());
            for (int idx:group)
                if (idx>=0 && idx<int(images_.size()) && !images_[size_t(idx)].file.isEmpty()) valid.push_back(idx);
            if (valid.size()<2) continue;
            const int keeper=chooseMd5Keeper(valid,rule);
            for (int idx:valid) if (idx!=keeper) toDelete << images_[size_t(idx)].file;
        }
        if (toDelete.isEmpty()) return;

        QMessageBox confirm(QMessageBox::Warning, QStringLiteral("Confirm Bulk Delete"),
            QStringLiteral("Move %1 files from %2 exact-match groups to the Recycle Bin?")
                .arg(toDelete.size()).arg(eligibleGroups),
            QMessageBox::Yes|QMessageBox::Cancel, this);
        confirm.setInformativeText(QStringLiteral("%1. Exactly one file will remain in every group.").arg(ruleLabel));
        confirm.setDefaultButton(QMessageBox::Cancel);
        if (confirm.exec()!=QMessageBox::Yes) return;
        startBulkDelete(std::move(toDelete),eligibleGroups,ruleLabel);
    }

    int chooseMd5Keeper(const std::vector<int>& candidates, Md5KeepRule rule) const {
        auto pathCompare=[this](int left,int right) {
            return QString::compare(images_[size_t(left)].file,images_[size_t(right)].file,
                                    Qt::CaseInsensitive);
        };
        auto nameOf=[this](int idx) { return QFileInfo(images_[size_t(idx)].file).fileName(); };
        auto better=[&](int candidate,int current) {
            const ImgMeta& a=images_[size_t(candidate)];
            const ImgMeta& b=images_[size_t(current)];
            const QString an=nameOf(candidate), bn=nameOf(current);
            const int nameCmp=QString::compare(an,bn,Qt::CaseInsensitive);
            switch (rule) {
                case Md5KeepRule::Newest:
                    if (a.mtime!=b.mtime) return a.mtime>b.mtime;
                    break;
                case Md5KeepRule::Oldest:
                    if (a.mtime!=b.mtime) return a.mtime<b.mtime;
                    break;
                case Md5KeepRule::FilenameFirst:
                    if (nameCmp!=0) return nameCmp<0;
                    break;
                case Md5KeepRule::FilenameLast:
                    if (nameCmp!=0) return nameCmp>0;
                    break;
                case Md5KeepRule::ShortestName:
                    if (an.size()!=bn.size()) return an.size()<bn.size();
                    if (nameCmp!=0) return nameCmp<0;
                    break;
                case Md5KeepRule::LongestName:
                    if (an.size()!=bn.size()) return an.size()>bn.size();
                    if (nameCmp!=0) return nameCmp<0;
                    break;
            }
            return pathCompare(candidate,current)<0;
        };
        int keeper=candidates.front();
        for (size_t i=1;i<candidates.size();++i)
            if (better(candidates[i],keeper)) keeper=candidates[i];
        return keeper;
    }

    void forgetDeletedFiles(const QStringList& deleted) {
        if (deleted.isEmpty()) return;
        QSet<QString> paths;
        paths.reserve(deleted.size());
        for (const QString& path:deleted) paths.insert(path);
        for (ImgMeta& image:images_) {
            if (paths.contains(image.file)) { image.file.clear(); image.selected=false; }
        }

        SqliteCache cache(cacheFile_);
        if (cache.isOpen()) {
            const QDir root(currentDir_);
            QStringList relative;
            relative.reserve(deleted.size());
            for (const QString& path:deleted) relative << root.relativeFilePath(path);
            if (!cache.removePaths(relative))
                statusTxt_->setText(QStringLiteral("Files deleted; cache cleanup will finish on the next scan."));
        }
    }

    void startBulkDelete(QStringList toDelete, int groupCount, const QString& ruleLabel) {
        clearThumbs();
        QCoreApplication::processEvents();
        busy_=true;
        bulkDeleting_=true;
        cancelled_=false;
        setInteractive(false);
        prog_->setVisible(true);
        prog_->setRange(0,toDelete.size());
        prog_->setValue(0);
        statusTxt_->setText(QStringLiteral("Deleting exact duplicates… 0 / %1").arg(toDelete.size()));

        QPointer<DupeGemMainWindow> self(this);
        auto post=[self,total=toDelete.size()](int done) {
            if (!self) return;
            QMetaObject::invokeMethod(self,[self,done,total]{
                if (!self || !self->bulkDeleting_.load()) return;
                self->prog_->setValue(done);
                self->statusTxt_->setText(QStringLiteral("Deleting exact duplicates… %1 / %2").arg(done).arg(total));
            },Qt::QueuedConnection);
        };

        auto* watcher=new QFutureWatcher<BulkDeleteResult>(this);
        bulkDeleteWatcher_=watcher;
        connect(watcher,&QFutureWatcher<BulkDeleteResult>::finished,this,
                [this,watcher,groupCount,ruleLabel]{
            BulkDeleteResult result=watcher->future().takeResult();
            watcher->deleteLater(); bulkDeleteWatcher_=nullptr; bulkDeleting_=false;
            forgetDeletedFiles(result.deleted);
            prog_->setVisible(false);
            busy_=false;

            if (!result.failed.isEmpty()) {
                QStringList shown=result.failed.mid(0,12);
                if (result.failed.size()>shown.size())
                    shown << QStringLiteral("…and %1 more").arg(result.failed.size()-shown.size());
                QMessageBox::warning(this,QStringLiteral("Some Files Could Not Be Deleted"),
                    QStringLiteral("%1 file(s) could not be moved to the Recycle Bin:\n\n%2")
                        .arg(result.failed.size()).arg(shown.join('\n')));
            }

            if (!result.deleted.isEmpty()) {
                statusTxt_->setText(QStringLiteral("Deleted %1 files from %2 groups using ‘%3’. Regrouping…")
                    .arg(result.deleted.size()).arg(groupCount).arg(ruleLabel));
                refreshAfterDelete();
            } else {
                setInteractive(true);
                showGroup(currentGroup_);
                statusTxt_->setText(result.cancelled
                    ? QStringLiteral("Bulk deletion cancelled before any files were deleted.")
                    : QStringLiteral("No files were deleted."));
            }
        });

        watcher->setFuture(QtConcurrent::run([toDelete=std::move(toDelete),self,post]() mutable {
            BulkDeleteResult result;
            for (int i=0;i<toDelete.size();++i) {
                if (!self || self->cancelled_.load()) { result.cancelled=true; break; }
                const QString& path=toDelete[i];
                if (QFile::moveToTrash(path)) result.deleted << path;
                else result.failed << path;
                const int done=i+1;
                if ((done&15)==0 || done==toDelete.size()) post(done);
            }
            return result;
        }));
    }

    // === UI interactions ===
    void groupChosen(int row) {
        if (row<0 || row>=groupsList_->count() || row>=int(groupIdxSorted_.size())) { showGroup(-1); return; }
        currentGroup_ = groupIdxSorted_[row];
        showGroup(currentGroup_);
    }
    void resortGroups() { refillGroupsList(); }
    void applyGroupFilter() { refillGroupsList(); }
    void applyFileSearch(const QString&) { showGroup(currentGroup_); }

    void updateAlgorithmInfo() {
        const auto algo=currentAlgorithm();
        algoInfo_->setText(QStringLiteral("<b>%1 speed:</b> %2<br>%3")
                           .arg(QString::fromLatin1(algoName(algo)),
                                QString::fromLatin1(algoSpeed(algo)),
                                algoDescription(algo)));
    }

    HashAlgo currentAlgorithm() const {
        bool ok=false;
        const int value=algoCombo_->currentData().toInt(&ok);
        return ok && value>=0 && value<int(HashAlgo::Count)
            ? static_cast<HashAlgo>(value) : HashAlgo::MD5;
    }

    void updateAlgorithmUi() {
        updateAlgorithmInfo();
        const bool md5=currentAlgorithm()==HashAlgo::MD5;
        thresholdRow_->setVisible(!md5);
        md5TypesRow_->setVisible(md5);
        btnDeleteAllGroups_->setVisible(md5);
        btnRegroup_->setText(md5 ? QStringLiteral("Rescan/Regroup Files")
                                : QStringLiteral("Rescan/Regroup Images"));
    }

    void updateThrLabel() {
        const int v = thrSlider_->value();
        const int minSim = 100 - int(std::round(double(v)*100.0/kHashBits));
        thrLabel_->setText(QStringLiteral("%1  (min %2%)").arg(v).arg(minSim));
    }

    void addThumbBatch() {
        constexpr size_t kBatch = 12;
        const size_t start = nextThumb_;
        const size_t end = std::min(start + kBatch, pendingThumbIndices_.size());
        for (size_t i=start;i<end;++i) {
            const int idx=pendingThumbIndices_[i];
            if (idx<0 || idx>=int(images_.size()) || currentLeaderIndex_<0 || currentLeaderIndex_>=int(images_.size())) continue;
            auto* t=new ThumbWidget(images_[size_t(idx)],images_[size_t(currentLeaderIndex_)],
                                    currentAlgorithm(),thumbHeight_,thumbContainer_);
            t->setChecked(images_[size_t(idx)].selected);
            connect(t,&ThumbWidget::checkedChanged,this,[this,idx](bool on){
                if(idx<int(images_.size())) images_[size_t(idx)].selected=on;
                updateSelectionSummary();
            });
            connect(t,&ThumbWidget::requestDelete,this,[this,idx,t](const QString& path){
                if (t) t->unloadMovie();
                if (QFile::moveToTrash(path)) {
                    if (idx<int(images_.size())) { images_[size_t(idx)].file.clear(); images_[size_t(idx)].selected=false; }
                    refreshAfterDelete();
                } else QMessageBox::warning(this,QStringLiteral("Deletion failed"),QStringLiteral("The file could not be moved to the Recycle Bin."));
            });
            flow_->addWidget(t); currentThumbs_.push_back(t); t->startMovie();
        }
        nextThumb_ = end;
        QTimer::singleShot(0,this,&DupeGemMainWindow::loadMoreThumbsIfNeeded);
    }

    void loadMoreThumbsIfNeeded() {
        if (nextThumb_>=pendingThumbIndices_.size() || addTimer_->isActive()) return;
        QScrollBar* bar=scroll_->verticalScrollBar();
        const int preload=scroll_->viewport()->height()*2;
        if (bar->maximum()==0 || bar->value()+preload>=bar->maximum()) addTimer_->start(0);
    }

    // ---- MD5 helpers (only used by MD5 mode) ----
    static QString md5ForFile(const QString& path) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return QStringLiteral("error");
        QCryptographicHash hh(QCryptographicHash::Md5);
        QByteArray buffer(1<<20, Qt::Uninitialized);
        while (true) {
            const qint64 bytes=f.read(buffer.data(), buffer.size());
            if (bytes<0) return QStringLiteral("error");
            if (bytes==0) break;
            hh.addData(QByteArrayView(buffer.constData(), bytes));
        }
        return hh.result().toHex();
    }
    inline void ensureMd5(ImgMeta& m) {
        if (m.md5.isEmpty() || m.md5 == QStringLiteral("error")) {
            m.md5 = md5ForFile(m.file);
        }
    }

    // === Utility ===
    void applyModernTheme() {
        QApplication::setStyle(QStringLiteral("Fusion"));
        setStyleSheet(QStringLiteral(R"CSS(
            QMainWindow, QWidget { background:#181818; color:#eeeeee; font-family:"Segoe UI"; font-size:10pt; }
            QFrame#sidePanel, QFrame#sideSection { background:#222222; border:1px solid #3a3a3a; border-radius:10px; }
            QWidget#galleryPanel { background:#181818; }
            QLabel#brandTitle { font-size:26pt; font-weight:750; color:#fafafa; }
            QLabel#brandSubtitle, QLabel#mutedLabel { color:#a3a3a3; }
            QLabel#sectionTitle { color:#d0d0d0; font-size:8pt; font-weight:700; }
            QLabel#panelTitle, QLabel#galleryTitle { font-size:15pt; font-weight:700; color:#fafafa; }
            QLabel#infoCard { background:#2a2a2a; border:1px solid #444444; border-radius:8px; padding:10px; color:#dddddd; }
            QLabel#selectionChip { background:#303030; color:#eeeeee; border:1px solid #525252; border-radius:11px; padding:4px 10px; font-weight:650; }
            QLabel#statusText { color:#cecece; }
            QPushButton { background:#363636; border:1px solid #505050; border-radius:7px; padding:8px 12px; color:#f2f2f2; font-weight:600; }
            QPushButton:hover { background:#464646; border-color:#686868; }
            QPushButton:pressed { background:#2b2b2b; }
            QPushButton:disabled { color:#777777; background:#292929; border-color:#383838; }
            QPushButton#primaryButton { background:#486b80; border-color:#668da3; color:#ffffff; }
            QPushButton#primaryButton:hover { background:#587f96; }
            QPushButton#dangerButton { background:#7d3038; border-color:#a64a55; color:white; }
            QPushButton#dangerButton:hover { background:#94404a; }
            QPushButton#secondaryDangerButton { background:#493236; border-color:#805159; color:#f4e8ea; }
            QPushButton#secondaryDangerButton:hover { background:#5c3a40; border-color:#9b626c; }
            QPushButton#quietButton { background:transparent; }
            QLineEdit, QComboBox { background:#1d1d1d; border:1px solid #484848; border-radius:7px; padding:8px; selection-background-color:#626262; }
            QLineEdit:focus, QComboBox:focus { border-color:#858585; }
            QComboBox QAbstractItemView { background:#252525; border:1px solid #505050; selection-background-color:#555555; }
            QListWidget { background:#1d1d1d; border:1px solid #444444; border-radius:8px; padding:4px; outline:0; }
            QListWidget::item { border-radius:6px; padding:9px; margin:2px; }
            QListWidget::item:hover { background:#303030; }
            QListWidget::item:selected { background:#505050; color:#ffffff; }
            QProgressBar { background:#202020; border:1px solid #484848; border-radius:6px; min-height:12px; text-align:center; }
            QProgressBar::chunk { background:#747474; border-radius:5px; }
            QSlider::groove:horizontal { height:5px; background:#414141; border-radius:2px; }
            QSlider::handle:horizontal { width:16px; margin:-6px 0; border-radius:8px; background:#bdbdbd; }
            QCheckBox { spacing:7px; }
            QCheckBox::indicator { width:16px; height:16px; border:1px solid #656565; border-radius:4px; background:#202020; }
            QCheckBox::indicator:hover { border-color:#a0a0a0; }
            QCheckBox::indicator:checked { background:#a8a8a8; border-color:#d0d0d0; image:none; }
            QScrollArea { border:0; background:transparent; }
            QScrollBar:vertical { background:#202020; width:11px; margin:0; }
            QScrollBar::handle:vertical { background:#555555; border-radius:5px; min-height:28px; }
            QSplitter::handle { background:#181818; width:6px; height:6px; }
            QWidget#thumbCard { background:#242424; border:1px solid #414141; border-radius:10px; padding:6px; }
            QWidget#thumbCard:hover { border-color:#8a8a8a; background:#2d2d2d; }
            QLabel#thumbName { color:#f2f2f2; font-weight:650; padding-top:3px; }
            QLabel#thumbInfo { color:#aaaaaa; font-family:Consolas; font-size:8pt; }
            QToolTip { background:#252525; color:#f5f5f5; border:1px solid #555555; padding:5px; }
        )CSS"));
    }

    void updateSelectionSummary() {
        int selected=0;
        if (currentGroup_>=0 && currentGroup_<int(groups_.size()))
            for (int idx:groups_[size_t(currentGroup_)]) if (idx<int(images_.size())&&images_[size_t(idx)].selected) ++selected;
        if (selectionLabel_) selectionLabel_->setText(QStringLiteral("%1 selected").arg(selected));
    }

    void setInteractive(bool e) {
        for (QPushButton* b : {btnSelect_, btnRegroup_, btnDelete_, btnDeleteAllGroups_}) if (b) b->setEnabled(e);

        QList<QWidget*> widgets = {
            groupsList_, sortCombo_, groupFilter_,
            algoCombo_,  thrSlider_, search_, sizeSlider_,
            showSingles_, scanSubs_, md5Images_, md5Videos_, md5Other_
        };
        for (QWidget* w : widgets) if (w) w->setEnabled(e);
        if (btnCancel_) { btnCancel_->setVisible(!e); btnCancel_->setEnabled(!e); }
    }

    void unloadMovies() { for (const QPointer<ThumbWidget>& t: currentThumbs_) if (t) t->unloadMovie(); }

    void clearThumbs() {
        addTimer_->stop();
        while (flow_->count() > 0) {
            QLayoutItem* it = flow_->takeAt(0);
            if (auto* w = it->widget()) delete w;
            delete it;
        }
        currentThumbs_.clear();
        pendingThumbIndices_.clear();
        nextThumb_ = 0;
        currentLeaderIndex_=-1;
    }

    void setAllThumbsChecked(bool on) {
        for (const QPointer<ThumbWidget>& t : currentThumbs_) if (t) t->setChecked(on);
        if (currentGroup_ >= 0 && currentGroup_ < int(groups_.size())) {
            for (int idx : groups_[currentGroup_]) {
                if (!images_[idx].file.isEmpty()) images_[idx].selected = on;
            }
        }
        updateSelectionSummary();
    }

    struct GroupStats {
        int files = 0;
        double mb = 0.0;
    };

    void parseFilter(int& minFilesOut, double& minMbOut, QStringList& termsOut) const {
        minFilesOut = 0;
        minMbOut = 0.0;
        termsOut.clear();

        const QString s = groupFilter_->text().trimmed();
        if (s.isEmpty()) return;

        const auto parts = s.split(QRegularExpression(R"(\s+)"), Qt::SkipEmptyParts);
        for (const QString& p : parts) {
            const QString pl = p.toLower();
            if (pl.startsWith("min:")) {
                bool ok=false; int v = pl.mid(4).toInt(&ok); if (ok) minFilesOut = std::max(minFilesOut, v);
            } else if (pl.startsWith("minmb:")) {
                bool ok=false; double v = pl.mid(6).toDouble(&ok); if (ok) minMbOut = std::max(minMbOut, v);
            } else if (pl.startsWith("size>=")) {
                bool ok=false; double v = pl.mid(6).toDouble(&ok); if (ok) minMbOut = std::max(minMbOut, v);
            } else if (pl.endsWith("mb")) {
                bool ok=false; double v = pl.left(pl.size()-2).toDouble(&ok); if (ok) minMbOut = std::max(minMbOut, v);
            } else {
                termsOut << pl;
            }
        }
    }

    GroupStats calcGroupStats(const std::vector<int>& group) const {
        GroupStats gs;
        gs.files = int(group.size());
        qint64 bytes = 0;
        for (int idx : group) {
            const auto& im = images_[idx];
            bytes += im.size;
        }
        gs.mb = bytes / (1024.0 * 1024.0);
        return gs;
    }

    void refillGroupsList() {
        // Sort lightweight indices; copying every group's member vector here
        // doubled memory and made large libraries pause after grouping.
        std::vector<int> order(groups_.size());
        std::iota(order.begin(),order.end(),0);

        // Sort
        switch (sortCombo_->currentIndex()) {
            case 1: // by file count
                std::sort(order.begin(),order.end(),[this](int a,int b){return groups_[size_t(a)].size()>groups_[size_t(b)].size();});
                break;
            case 2: // by total size
                std::sort(order.begin(),order.end(), [this](int a, int b){
                    auto tot=[this](const std::vector<int>& g){ qint64 s=0; for (int i: g) s+=images_[i].size; return s; };
                    return tot(groups_[size_t(a)]) > tot(groups_[size_t(b)]);
                });
                break;
            default: break; // group #
        }

        // Filter
        int minFiles; double minMb; QStringList terms;
        parseFilter(minFiles, minMb, terms);

        groupsList_->clear();
        groupIdxSorted_.clear();
        for (int idx : order) {
            const auto& g  = groups_[size_t(idx)];
            if (!showSingles_->isChecked() && g.size() < 2) continue;

            GroupStats gs = calcGroupStats(g);
            if (gs.files < minFiles) continue;
            if (gs.mb    < minMb)    continue;
            bool okTerms = true;
            for (const QString& t : terms) {
                bool found=false;
                for (int imageIndex : g) {
                    if (QFileInfo(images_[imageIndex].file).fileName().contains(t, Qt::CaseInsensitive)) {
                        found=true; break;
                    }
                }
                if (!found) { okTerms = false; break; }
            }
            if (!okTerms) continue;

            qint64 total = 0; for (int i: g) total += images_[i].size;
            groupsList_->addItem(QStringLiteral("Group %1  •  %2 files  •  %3 MB")
                                 .arg(idx+1)
                                 .arg(g.size())
                                 .arg(QString::number(total/1024.0/1024.0, 'f', 2)));
            groupIdxSorted_.push_back(idx);
        }
    }

    void showGroup(int gidx) {
        addTimer_->stop();
        unloadMovies();
        clearThumbs();
        if (gidx < 0 || gidx >= int(groups_.size()) || groups_[gidx].empty()) {
            headerLabel_->setText(QStringLiteral("Select a group to view images."));
            if (galleryTitle_) galleryTitle_->setText(QStringLiteral("Choose a duplicate group"));
            return;
        }

        auto& g = groups_[gidx];
        headerLabel_->setText(QStringLiteral("Group %1 / %2 — %3 files")
                              .arg(gidx+1).arg(groups_.size()).arg(g.size()));
        galleryTitle_->setText(QStringLiteral("Group %1  •  %2 candidates").arg(gidx+1).arg(g.size()));
        updateSelectionSummary();

        // Filename search filter
        const QString searchTerm = search_->text().trimmed().toLower();

        // Adaptive thumbnail height from current slider value
        const int thumbH = thumbHeight_;

        // Keep only lightweight indices here. Cards and previews are created on
        // demand as the user approaches the end of the visible scroll region.
        std::vector<int> valid;
        valid.reserve(g.size());
        for (int idx : g) {
            const auto& im = images_[idx];
            if (im.file.isEmpty()) continue;
            if (!searchTerm.isEmpty() && !QFileInfo(im.file).fileName().toLower().contains(searchTerm)) continue;
            valid.push_back(idx);
        }
        if (valid.empty()) {
            headerLabel_->setText(headerLabel_->text() + QStringLiteral("  (no files match current search)"));
            return;
        }
        Q_UNUSED(thumbH);
        currentLeaderIndex_=g.front();
        pendingThumbIndices_=std::move(valid);
        const bool filtered = !searchTerm.isEmpty();
        headerLabel_->setText(filtered
            ? QStringLiteral("Group %1 of %2  •  %3 of %4 files match")
                  .arg(gidx+1).arg(groups_.size()).arg(pendingThumbIndices_.size()).arg(g.size())
            : QStringLiteral("Group %1 of %2  •  %3 files")
                  .arg(gidx+1).arg(groups_.size()).arg(g.size()));
        scroll_->verticalScrollBar()->setValue(0);
        nextThumb_ = 0;
        addTimer_->start(0);
    }

    void refreshAfterDelete() {
        // ThumbWidget stores references into images_. Destroy all thumbnails
        // before erase() can move elements and invalidate those references.
        clearThumbs();
        currentGroup_ = -1;
        // purge removed/invalid
        images_.erase(std::remove_if(images_.begin(), images_.end(),
                     [](const ImgMeta& im){ return !im.valid || im.file.isEmpty(); }),
                     images_.end());
        trees_.clear();
        if (images_.empty()) {
            groups_.clear(); groupsList_->clear(); busy_=false; setInteractive(true);
            headerLabel_->setText(QStringLiteral("No images remain.")); return;
        }
        busy_=true; startGrouping();
    }

private:
    // ==== State ====
    // Data & indexing
    QString currentDir_;
    QString cacheFile_;
    std::vector<ImgMeta> images_;
    std::vector<std::vector<int>> groups_;
    std::atomic<bool> scanning_{false};
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> busy_{false};
    std::atomic<bool> bulkDeleting_{false};
    QFutureWatcher<ScanResult>* scanWatcher_{};
    QFutureWatcher<GroupResult>* groupWatcher_{};
    QFutureWatcher<BulkDeleteResult>* bulkDeleteWatcher_{};
    std::map<HashAlgo, std::shared_ptr<BKTree<BitHash>>> trees_;

    // Layout containers
    QSplitter* leftSplit_{};
    QWidget* controlsW_{};
    QWidget* groupsW_{};

    // RIGHT: grid
    QLabel* headerLabel_{};
    QWidget* thumbContainer_{};
    FlowLayout* flow_{};
    QScrollArea* scroll_{};

    // LEFT: controls
    QPushButton* btnSelect_{};
    QLabel*      folderLabel_{};
    QCheckBox*   scanSubs_{};
    QComboBox*   algoCombo_{};
    QLabel*      algoInfo_{};
    QWidget*     thresholdRow_{};
    QWidget*     md5TypesRow_{};
    QSlider*     thrSlider_{};
    QLabel*      thrLabel_{};
    QCheckBox*   md5Images_{};
    QCheckBox*   md5Videos_{};
    QCheckBox*   md5Other_{};
    QSlider*     sizeSlider_{};
    QLabel*      thumbLabel_{};
    QPushButton* btnRegroup_{};
    QCheckBox*   showSingles_{};
    QLabel*      statusTxt_{};
    QProgressBar* prog_{};
    QPushButton*  btnCancel_{};
    QPushButton*  btnDelete_{};
    QPushButton*  btnDeleteAllGroups_{};
    QLineEdit*    search_{};
    QLineEdit*    groupFilter_{};
    QComboBox*    sortCombo_{};
    QTimer*        md5TypeTimer_{};
    QListWidget*  groupsList_{};
    QLabel*        libraryStats_{};
    QLabel*        galleryTitle_{};
    QLabel*        selectionLabel_{};

    // Thumbs
    QTimer* addTimer_{};
    QTimer* filterTimer_{};
    QTimer* searchTimer_{};
    std::vector<QPointer<ThumbWidget>> currentThumbs_;
    std::vector<int> pendingThumbIndices_;
    size_t nextThumb_ = 0;
    int currentLeaderIndex_ = -1;
    int currentGroup_ = -1;
    int thumbHeight_ = 240;
    std::vector<int> groupIdxSorted_;
}; // class DupeGemMainWindow

} // namespace dg

// ---------- main ----------
#include "main.moc"

int main(int argc, char *argv[]) {
    qInstallMessageHandler(dg::qtMessageHandler);
    bool imageLimitOk=false;
    int imageLimitMb=qEnvironmentVariableIntValue("DUPEGEM_IMAGE_LIMIT_MB", &imageLimitOk);
    if (!imageLimitOk || imageLimitMb<32) imageLimitMb=128;
    QImageReader::setAllocationLimit(std::clamp(imageLimitMb, 32, 4096));
    QApplication app(argc, argv);
    dg::DupeGemMainWindow w;
    w.show();
    if (argc>1) {
        bool ok=false; const int algorithm=argc>2 ? QString::fromLocal8Bit(argv[2]).toInt(&ok) : -1;
        w.scanFolderOnLaunch(QString::fromLocal8Bit(argv[1]),ok?algorithm:-1);
    }
    return app.exec();
}
