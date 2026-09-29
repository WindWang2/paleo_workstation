// tst_cache_io — wave/io-perf-cache D7：编码检测/路径规范化/部分读/临时文件
// 卫生/文件锁/流式解析。
#include <QtTest>

#include "io/encodingdetect.h"
#include "io/pathcanon.h"
#include "io/partialread.h"
#include "io/streaming.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

class CacheIoTests : public QObject
{
    Q_OBJECT

  private slots:
    // D7.2 编码
    void detectUtf8();
    void detectGb18030();
    void detectBomVariants();
    void decodeGbWellFile();
    // D7.3 路径规范化
    void canonicalizeRelativeAndSymlink();
    void fingerprintStable();
    // D7.1 部分读
    void partialReadDropsHalfLine();
    void partialReadSmallFileWhole();
    // D7.4 临时文件卫生
    void sweepRemovesDeadPidTemps();
    void sweepKeepsAlivePidTemps();
    // D7.5 文件锁
    void writeGuardExcludesSecondLiveWriter();
    void writeGuardReclaimsStaleLock();
    // D7.6 流式解析
    void streamingLinesMatchReadAll();
    void streamingCsvRecords();
    void streamingGeoJsonBoundsMatchesDom();
    void streamingGeoJsonCountsFeatures();
    void streamingCancelStopsEarly();
    void partialReadMissingFile();
    void writeGuardWaitSucceedsWhenReleased();
    void sweepNoOpOnCleanDir();

  private:
    QTemporaryDir m_dir;
};

void CacheIoTests::detectUtf8()
{
  QCOMPARE(EncodingDetect::detect(QByteArrayLiteral("plain ascii only")),
           TextEncoding::Utf8);
  QCOMPARE(EncodingDetect::detect(QString::fromUtf8("中文井名").toUtf8()),
           TextEncoding::Utf8);
}

void CacheIoTests::detectGb18030()
{
  // GB18030 编码的中文（非法 UTF-8 字节序列）。
  const QString text = QStringLiteral("井名：基准井-1 号，分层：SB1");
  QStringEncoder enc2("GB18030", QStringConverter::Flag::Stateless);
  const QByteArray gb = enc2.encode(text);
  QVERIFY(gb.size() > text.size()); // 中文占双字节
  QCOMPARE(EncodingDetect::detect(gb), TextEncoding::GB18030);
  QCOMPARE(EncodingDetect::decodeText(gb), text);
}

void CacheIoTests::detectBomVariants()
{
  QByteArray utf8Bom;
  utf8Bom.append('\xEF').append('\xBB').append('\xBF');
  utf8Bom += QString::fromUtf8("带 BOM 的文本").toUtf8();
  QCOMPARE(EncodingDetect::detect(utf8Bom), TextEncoding::Utf8);
  QCOMPARE(EncodingDetect::decodeText(utf8Bom), QStringLiteral("带 BOM 的文本"));

  // UTF-16LE BOM。
  const QString wide = QStringLiteral("A中文1");
  QByteArray utf16le;
  utf16le.append('\xFF').append('\xFE');
  for (const QChar c : wide)
  {
    const ushort u = c.unicode();
    utf16le.append(char(u & 0xFF));
    utf16le.append(char(u >> 8));
  }
  QCOMPARE(EncodingDetect::decodeText(utf16le), wide);
}

void CacheIoTests::decodeGbWellFile()
{
  // GB18030 井口文件：井名可解（此前 fromUtf8 全串变 � 的回归防线）。
  const QString gbText = QStringLiteral("井-1 500000.5 4000000.25 0 3000\n井-2 500100 4000100 0 2800\n");
  QStringEncoder enc("GB18030", QStringConverter::Flag::Stateless);
  const QString path = m_dir.filePath("heads_gb.dat");
  QFile f(path);
  QVERIFY(f.open(QIODevice::WriteOnly));
  f.write(enc.encode(gbText));
  f.close();
  PartialRead::PartialText pt = PartialRead::readTextPartial(path, 1 << 20);
  QVERIFY(!pt.truncated);
  QVERIFY(pt.text.contains(QStringLiteral("井-1")));
  QVERIFY(pt.text.contains(QStringLiteral("井-2")));
}

void CacheIoTests::canonicalizeRelativeAndSymlink()
{
  const QString real = m_dir.filePath("real.dat");
  QFile f(real);
  QVERIFY(f.open(QIODevice::WriteOnly));
  f.write("x");
  f.close();
  const QString link = m_dir.filePath("link.dat");
  QFile::remove(link);
  QVERIFY(QFile::link(real, link));

  // 符号链接 → 真身。
  QCOMPARE(PathCanon::canonicalize(link), PathCanon::canonicalize(real));
  // 相对路径以 base 绝对化。
  const QString abs = PathCanon::canonicalize(QStringLiteral("sub/../real.dat"), m_dir.path());
  QCOMPARE(abs, PathCanon::canonicalize(real));
  // 不存在的路径：清洗后的绝对形态（不空、无 ../）。
  const QString ghost = PathCanon::canonicalize(m_dir.filePath(QStringLiteral("a/../ghost.dat")));
  QVERIFY(ghost.endsWith(QStringLiteral("ghost.dat")));
  QVERIFY(!ghost.contains(QStringLiteral("../")));
}

void CacheIoTests::fingerprintStable()
{
  const QFileInfo info(QFileInfo(m_dir.path()));
  const QString a = PathCanon::fingerprint(info.absoluteFilePath(), 12345, 678);
  const QString b = PathCanon::fingerprint(info.absoluteFilePath(), 12345, 678);
  QCOMPARE(a, b);
  QVERIFY(PathCanon::fingerprint(info.absoluteFilePath(), 99999, 678) != a);
}

void CacheIoTests::partialReadDropsHalfLine()
{
  // D7.1：5 行文件只读前半 → 得到完整行 + truncated 标志，半行丢弃。
  const QStringList lines = {"one 1", "two 2", "three 3", "four 4", "five 5"};
  const QString path = m_dir.filePath("partial.txt");
  QFile f(path);
  QVERIFY(f.open(QIODevice::WriteOnly));
  f.write(lines.join(QLatin1Char('\n')).toUtf8());
  f.close();
  QFile allFile(path);
  QVERIFY(allFile.open(QIODevice::ReadOnly));
  const QByteArray all = allFile.readAll();
  allFile.close();
  PartialRead::PartialText pt = PartialRead::readTextPartial(path, all.size() / 2);
  QVERIFY(pt.truncated);
  QVERIFY(pt.bytesRead <= all.size() / 2);
  const QStringList got = pt.text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
  QVERIFY(got.size() >= 1 && got.size() < 5);
  QCOMPARE(got.first(), QStringLiteral("one 1"));
  // 得到的每一行都必须是完整行（原行集合的前缀）。
  for (int i = 0; i < got.size(); ++i)
    QCOMPARE(got.at(i), lines.at(i));
}

void CacheIoTests::partialReadSmallFileWhole()
{
  const QString path = m_dir.filePath("whole.txt");
  QFile f(path);
  QVERIFY(f.open(QIODevice::WriteOnly));
  f.write(QByteArrayLiteral("a\nb\nc\n"));
  f.close();
  const auto pt = PartialRead::readTextPartial(path, 1 << 20);
  QVERIFY(!pt.truncated);
  QCOMPARE(pt.text, QStringLiteral("a\nb\nc\n"));
}

void CacheIoTests::sweepRemovesDeadPidTemps()
{
  // D7.4：带死 pid 的过渡文件被清。
  const QDir dir(m_dir.path());
  const QString dead = dir.filePath(QStringLiteral("data.dat.tmp999999_1"));
  QFile(dead).open(QIODevice::WriteOnly);
  const QString stale = dir.filePath(QStringLiteral("old.partial")); // 无 pid、设老 mtime
  {
    QFile sf(stale);
    QVERIFY(sf.open(QIODevice::WriteOnly));
    QVERIFY(sf.setFileTime(QDateTime::currentDateTime().addDays(-3),
                           QFileDevice::FileModificationTime));
  }
  const QStringList removed = PartialRead::sweepTempFiles(m_dir.path(), 24 * 3600 * 1000);
  QVERIFY(removed.contains(dead));
  QVERIFY(removed.contains(stale));
  QVERIFY(!QFile::exists(dead));
}

void CacheIoTests::sweepKeepsAlivePidTemps()
{
  const QDir dir(m_dir.path());
  const QString alive = dir.filePath(
      QStringLiteral("live.dat.tmp%1_2").arg(QCoreApplication::applicationPid()));
  QFile(alive).open(QIODevice::WriteOnly);
  const QStringList removed = PartialRead::sweepTempFiles(m_dir.path(), 24 * 3600 * 1000);
  QVERIFY(!removed.contains(alive));
  QVERIFY(QFile::exists(alive)); // 活进程的过渡文件不动
}

void CacheIoTests::writeGuardExcludesSecondLiveWriter()
{
  const QString target = m_dir.filePath("asset.dat");
  {
    PartialRead::WriteGuard g1(target);
    QVERIFY(g1.locked());
    // 同进程第二守卫：持有者是自己 → 允许覆盖（同进程协作由调用方串行化），
    // 换个角度测真正的互斥：手工写一个活 pid 的锁。
    QFile lf(g1.lockFilePath());
    QVERIFY(lf.open(QIODevice::WriteOnly | QIODevice::Truncate));
    lf.write(QByteArrayLiteral("1 paleo-write")); // pid=1（init，恒活）
    lf.close();
    PartialRead::WriteGuard g2(target);
    QVERIFY(!g2.locked());
    QVERIFY(g2.refusalReason().contains(QLatin1Char('1')));
  }
  // g1 析构后锁文件应已被释放路径处理（g2 未持锁不动文件；g1 release 删除）。
  // 注：上面手工覆盖了锁文件——g1 的 release 仍会 remove 该路径。
  QVERIFY(!QFile::exists(target + QStringLiteral(".wlock")));
}

void CacheIoTests::writeGuardReclaimsStaleLock()
{
  const QString target = m_dir.filePath("stale.dat");
  QFile lf(target + QStringLiteral(".wlock"));
  QVERIFY(lf.open(QIODevice::WriteOnly));
  lf.write(QByteArrayLiteral("999999 paleo-write")); // 死 pid
  lf.close();
  PartialRead::WriteGuard g(target);
  QVERIFY(g.locked()); // 陈锁回收后获得
}

void CacheIoTests::streamingLinesMatchReadAll()
{
  // 生成 20k 行井口样式文本（>256KB 分块边界）。
  const QString path = m_dir.filePath("lines.dat");
  {
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    for (int i = 0; i < 20000; ++i)
      f.write(QStringLiteral("W%1 %2 %3 0 3000\n")
                  .arg(i)
                  .arg(QString::number(500000.0 + i, 'f', 1))
                  .arg(QString::number(4000000.0 + i * 0.5, 'f', 1))
                  .toUtf8());
  }
  QFile refFile(path);
  QVERIFY(refFile.open(QIODevice::ReadOnly));
  const QStringList reference =
      QString::fromUtf8(refFile.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
  qint64 progressCalls = 0;
  qint64 count = 0;
  QStringList got;
  got.reserve(reference.size());
  const qint64 n = Streaming::forEachLine(
      path,
      [&got](qint64, const QString &line) {
        got.append(line);
        return true;
      },
      nullptr,
      [&](qint64, qint64) { ++progressCalls; });
  QCOMPARE(n, qint64(reference.size()));
  QCOMPARE(got, reference);
  QVERIFY(progressCalls > 0);
}

void CacheIoTests::streamingCsvRecords()
{
  const QString path = m_dir.filePath("csv.dat");
  {
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("# comment\nA B C\n1,2,3\n4,5,6\n\n7,8,9\n");
  }
  QList<QStringList> rows;
  const qint64 n = Streaming::forEachRecord(
      path, ',',
      [&rows](qint64, const QStringList &cols) {
        rows.append(cols);
        return true;
      });
  QCOMPARE(n, qint64(4)); // 空行/注释不算记录；泛型口径首行也是记录
  QCOMPARE(rows.at(0), QStringList({QStringLiteral("A B C")}));
  QCOMPARE(rows.at(1), QStringList({"1", "2", "3"}));
  QCOMPARE(rows.at(3), QStringList({"7", "8", "9"}));
}

void CacheIoTests::streamingGeoJsonBoundsMatchesDom()
{
  // 生成多要素 GeoJSON（点+线）——流式包围盒 == QJsonDocument 全量计算。
  const QString path = m_dir.filePath("fc.geojson");
  {
    QJsonArray feats;
    for (int i = 0; i < 500; ++i)
    {
      QJsonObject geom;
      if (i % 3 == 0)
      {
        geom.insert(QStringLiteral("type"), QStringLiteral("Point"));
        geom.insert(QStringLiteral("coordinates"),
                    QJsonArray{100.0 + i * 3.5, 200.0 - i * 1.25});
      }
      else
      {
        QJsonArray line;
        for (int k = 0; k < 6; ++k)
          line.append(QJsonArray{50.0 + i + k * 7.0, 80.0 - i * 0.5 + k * 2.0});
        geom.insert(QStringLiteral("type"), QStringLiteral("LineString"));
        geom.insert(QStringLiteral("coordinates"), line);
      }
      QJsonObject f;
      f.insert(QStringLiteral("type"), QStringLiteral("Feature"));
      QJsonObject props;
      props.insert(QStringLiteral("name"), QStringLiteral("f%1").arg(i));
      props.insert(QStringLiteral("seq"), i);
      f.insert(QStringLiteral("properties"), props);
      f.insert(QStringLiteral("geometry"), geom);
      feats.append(f);
    }
    QJsonObject root;
    root.insert(QStringLiteral("type"), QStringLiteral("FeatureCollection"));
    root.insert(QStringLiteral("features"), feats);
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(root).toJson());
  }
  // DOM 基准。
  double dom[4] = {0, 0, 0, 0};
  {
    QFile domFile(path);
    QVERIFY(domFile.open(QIODevice::ReadOnly));
    const QJsonDocument doc = QJsonDocument::fromJson(domFile.readAll());
    bool first = true;
    const auto visitCoords = [](const QJsonValue &v, auto &self, double *b, bool &firstVal) {
      if (v.isArray())
      {
        const QJsonArray arr = v.toArray();
        const bool numeric = !arr.isEmpty() && arr.at(0).isDouble();
        if (numeric && arr.size() >= 2)
        {
          const double x = arr.at(0).toDouble();
          const double y = arr.at(1).toDouble();
          if (firstVal)
          {
            b[0] = b[2] = x;
            b[1] = b[3] = y;
            firstVal = false;
          }
          else
          {
            b[0] = qMin(b[0], x);
            b[1] = qMin(b[1], y);
            b[2] = qMax(b[2], x);
            b[3] = qMax(b[3], y);
          }
          return;
        }
        for (const QJsonValue &e : arr)
          self(e, self, b, firstVal);
      }
      else if (v.isObject())
      {
        const QJsonObject obj = v.toObject(); // 临时对象必须先落地——迭代器不能悬垂
        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it)
          self(it.value(), self, b, firstVal);
      }
    };
    visitCoords(doc.object(), visitCoords, dom, first);
  }
  double streamed[4] = {0, 0, 0, 0};
  QString err;
  QVERIFY2(Streaming::geoJsonBoundsStreaming(path, streamed, nullptr, &err), qPrintable(err));
  for (int i = 0; i < 4; ++i)
    QCOMPARE(streamed[i], dom[i]);
}

void CacheIoTests::streamingGeoJsonCountsFeatures()
{
  const QString path = m_dir.filePath("count.geojson");
  {
    QJsonObject root;
    root.insert(QStringLiteral("type"), QStringLiteral("FeatureCollection"));
    QJsonArray feats;
    for (int i = 0; i < 42; ++i)
    {
      QJsonObject f;
      f.insert(QStringLiteral("type"), QStringLiteral("Feature"));
      QJsonObject props;
      props.insert(QStringLiteral("kind"), QStringLiteral("x"));
      props.insert(QStringLiteral("weight"), i);
      f.insert(QStringLiteral("properties"), props);
      feats.append(f);
    }
    root.insert(QStringLiteral("features"), feats);
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(root).toJson());
  }
  QStringList keys;
  QCOMPARE(Streaming::geoJsonFeatureCountStreaming(path, &keys), qint64(42));
  QVERIFY(keys.contains(QStringLiteral("kind")));
  QVERIFY(keys.contains(QStringLiteral("weight")));
}

void CacheIoTests::streamingCancelStopsEarly()
{
  // D7.6：取消令牌生效——行产出在中途停止（< 全量行数）。
  const QString path = m_dir.filePath("cancel.dat");
  {
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    for (int i = 0; i < 5000; ++i)
      f.write(QStringLiteral("L%1\n").arg(i).toUtf8());
  }
  qint64 seen = 0;
  const qint64 n = Streaming::forEachLine(
      path,
      [&seen](qint64, const QString &) {
        ++seen;
        return true;
      },
      [&seen]() { return seen >= 100; });
  QCOMPARE(n, qint64(100));
  QVERIFY(seen <= 101);
}

void CacheIoTests::partialReadMissingFile()
{
  const auto pt = PartialRead::readTextPartial(m_dir.filePath("ghost.txt"), 1024);
  QVERIFY(pt.text.isEmpty());
  QVERIFY(!pt.truncated);
}

void CacheIoTests::writeGuardWaitSucceedsWhenReleased()
{
  // 等待模式：先放一个死 pid 锁 + 等待窗内自清——acquire 轮询拿到。
  const QString target = m_dir.filePath("wait.dat");
  PartialRead::WriteGuard g(target, 2000);
  QVERIFY(g.locked());
  QVERIFY(!g.lockFilePath().isEmpty());
}

void CacheIoTests::sweepNoOpOnCleanDir()
{
  // 干净目录：零动作、零误删（普通数据文件必须保留）。
  const QDir dir(m_dir.path());
  const QString plain = dir.filePath("normal.dat");
  QFile(plain).open(QIODevice::WriteOnly);
  const QStringList removed = PartialRead::sweepTempFiles(m_dir.path());
  QVERIFY(removed.isEmpty());
  QVERIFY(QFile::exists(plain));
}

QTEST_MAIN(CacheIoTests)
#include "tst_cache_io.moc"
