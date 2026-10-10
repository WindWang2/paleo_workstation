// #291：QString ↔ std::filesystem::path 的编码契约测试。
// Windows/MSVC 上 path 窄字符构造按 ANSI 代码页解码，中文路径必须用
// UTF-16 宽通道（paleo::toFsPath / paleo::fromFsPath）。Linux 上窄字节
// 直通，本套件在任何平台都应通过；在 Windows CI 上才是真正的回归闸门
// （窄构造写法在本测试的往返断言下会产出乱码而失败）。
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "domain/seismic/sgyvolume.h"
#include "services/fspathutils.h"

class TestFsPath : public QObject
{
  Q_OBJECT

private slots:
  void roundTripChinese()
  {
    const QString original = QStringLiteral("D:/工区_01/地震数据.sgy");
    QCOMPARE(paleo::fromFsPath(paleo::toFsPath(original)), original);
  }

  void roundTripMixedAndSpaces()
  {
    const QString original =
        QStringLiteral("/tmp/paleo 数据/工区 A/line-01_观测.sgy");
    const std::filesystem::path p = paleo::toFsPath(original);
    QVERIFY(std::filesystem::path(p) == p); // 拷贝构造无副作用
    QCOMPARE(paleo::fromFsPath(p), original);
  }

  void nativeFileIoThroughFsPath()
  {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QDir dir(tempDir.path());
    QVERIFY(dir.mkpath(QStringLiteral("工区")));
    const QString filePath =
        dir.filePath(QStringLiteral("工区/振幅.bin"));

    {
      std::ofstream out(paleo::toFsPath(filePath), std::ios::binary);
      QVERIFY(out.good());
      out << "paleo-#291";
    }
    QVERIFY(std::filesystem::exists(paleo::toFsPath(filePath)));

    std::ifstream in(paleo::toFsPath(filePath), std::ios::binary);
    QVERIFY(in.good());
    std::string contents((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    QCOMPARE(contents, std::string("paleo-#291"));
  }

  void sgyVolumeLoadFromChinesePath()
  {
#ifndef SEGY_FIXTURE_PATH
    QSKIP("SEGY_FIXTURE_PATH 未定义");
#else
    QFile fixture(QStringLiteral(SEGY_FIXTURE_PATH));
    QVERIFY(fixture.exists());

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QDir dir(tempDir.path());
    QVERIFY(dir.mkpath(QStringLiteral("工区")));
    const QString target = dir.filePath(QStringLiteral("工区/地震.sgy"));
    QVERIFY(QFile::copy(QStringLiteral(SEGY_FIXTURE_PATH), target));

    seismic::SgyVolume volume;
    std::string error;
    QVERIFY2(volume.Load(paleo::toFsPath(target), error),
             error.empty() ? "SgyVolume::Load failed" : error.c_str());
    // volume.Path() 存的是原生分隔符形式（Windows 反斜杠），按原生形式比较。
    QCOMPARE(QDir::fromNativeSeparators(paleo::fromFsPath(volume.Path())), target);
#endif
  }
};

QTEST_MAIN(TestFsPath)
#include "tst_fspath.moc"
