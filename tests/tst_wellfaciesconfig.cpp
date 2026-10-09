// tst_wellfaciesconfig — 测井相预测服务配置的传输安全与密钥落盘（#133）。
#include <QtTest>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "ai/wellfacieskeystore.h"
#include "ai/wellfaciesservice.h"

class TestWellFaciesConfig : public QObject
{
  Q_OBJECT
  QTemporaryDir m_home;
  QString m_config;

private slots:
  void initTestCase()
  {
    QVERIFY(m_home.isValid());
    // 方向 81：配置文件经 PALEO_WELL_FACIES_CONFIG 钉进本用例临时目录——
    // Windows 的 GenericConfigLocation 是 %LOCALAPPDATA%（known folder，不看
    // XDG_CONFIG_HOME），旧写法在本机读写开发者真实的
    // %LOCALAPPDATA%/paleo/well-facies.json：已配置过服务的机器上
    // saveRejectsCleartextPublicHttp 的 !exists 断言必红、
    // saveWithoutKeyForKeychainRoute 覆盖真实配置；仓库树内进程的文件监狱
    // 也拒写树外 %LOCALAPPDATA%。全平台同一路径，CI 可验。
    m_config = m_home.filePath(QStringLiteral("cfg/paleo/well-facies.json"));
    qputenv("PALEO_WELL_FACIES_CONFIG", QFile::encodeName(m_config));
    QCOMPARE(WellFaciesConfig::path(), m_config);
    // 自洽前提：从「无配置文件」开始（不依赖上一轮/他测试留下的状态）。
    QFile::remove(WellFaciesConfig::path());
    QVERIFY2(!QFile::exists(WellFaciesConfig::path()), qPrintable(WellFaciesConfig::path()));
    qunsetenv("PALEO_WELL_FACIES_URL");
    qunsetenv("PALEO_WELL_FACIES_API_KEY");
    qunsetenv("PALEO_WELL_FACIES_ALLOW_INSECURE_HTTP");
  }

  void cleanupTestCase()
  {
    QFile::remove(WellFaciesConfig::path());
    qunsetenv("PALEO_WELL_FACIES_CONFIG");
  }

  void defaultPathIsGenericConfig()
  {
    // 覆盖关掉即回到产品缺省位置（只算路径，不读写）。
    const QByteArray saved = qgetenv("PALEO_WELL_FACIES_CONFIG");
    qunsetenv("PALEO_WELL_FACIES_CONFIG");
    const QString def = WellFaciesConfig::path();
    qputenv("PALEO_WELL_FACIES_CONFIG", saved);
    QCOMPARE(def, QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
                      QStringLiteral("/paleo/well-facies.json"));
  }

  void noHardcodedDefault()
  {
    const WellFaciesConfig c;
    QVERIFY(c.baseUrl.isEmpty());
    QVERIFY(!c.allowInsecureHttp);
    QVERIFY(!WellFaciesConfig::validateUrl(c.baseUrl, false).isEmpty());
  }

  void validateUrl_data()
  {
    QTest::addColumn<QString>("url");
    QTest::addColumn<bool>("allowInsecure");
    QTest::addColumn<bool>("ok");
    QTest::newRow("https") << "https://facies.example.com/api/v1" << false << true;
    QTest::newRow("http-public") << "http://203.0.113.7:3100/api/v1" << false << false;
    QTest::newRow("http-public-optin") << "http://203.0.113.7:3100/api/v1" << true << true;
    QTest::newRow("http-loopback-v4") << "http://127.0.0.1:8080/api/v1" << false << true;
    QTest::newRow("http-loopback-v6") << "http://[::1]:8080/api/v1" << false << true;
    QTest::newRow("http-localhost") << "http://localhost:8080/api" << false << true;
    QTest::newRow("http-localhost-lookalike") << "http://localhost.evil.example/api" << false << false;
    QTest::newRow("ftp") << "ftp://example.com/x" << true << false;
    QTest::newRow("userinfo") << "https://u:p@example.com/x" << false << false;
    QTest::newRow("query") << "https://example.com/x?a=1" << false << false;
    QTest::newRow("empty") << "" << true << false;
  }
  void validateUrl()
  {
    QFETCH(QString, url);
    QFETCH(bool, allowInsecure);
    QFETCH(bool, ok);
    const QString err = WellFaciesConfig::validateUrl(QUrl(url), allowInsecure);
    QCOMPARE(err.isEmpty(), ok);
  }

  void saveRejectsCleartextPublicHttp()
  {
    WellFaciesConfig c;
    c.baseUrl = QUrl(QStringLiteral("http://203.0.113.7:3100/api/v1"));
    c.apiKey = "secret";
    QString error;
    QVERIFY(!c.save(&error));
    QVERIFY(error.contains(QStringLiteral("https")));
    QVERIFY(!QFile::exists(WellFaciesConfig::path()));
  }

  void saveWithoutKeyForKeychainRoute()
  {
    WellFaciesConfig c;
    c.baseUrl = QUrl(QStringLiteral("https://facies.example.com/api/v1"));
    c.apiKey = "secret-key";
    QString error;
    QVERIFY2(c.save(&error, false), qPrintable(error));
    QFile f(WellFaciesConfig::path());
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray raw = f.readAll();
    QVERIFY(!raw.contains("secret-key"));
    const auto j = QJsonDocument::fromJson(raw).object();
    QVERIFY(!j.contains("apiKey"));
    QVERIFY(!j.contains("allowInsecureHttp"));
#ifndef Q_OS_WIN
    QCOMPARE(f.permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther),
             QFileDevice::Permissions());
#endif
    const WellFaciesConfig back = WellFaciesConfig::load();
    QCOMPARE(back.baseUrl, c.baseUrl);
    QVERIFY(back.apiKey.isEmpty());
  }

  void fileFallbackRoundTripAndOptIn()
  {
    WellFaciesConfig c;
    c.baseUrl = QUrl(QStringLiteral("http://10.0.0.5:3100/api/v1"));
    c.apiKey = "k";
    c.allowInsecureHttp = true;
    QString error;
    QVERIFY2(c.save(&error, true), qPrintable(error));
    const WellFaciesConfig back = WellFaciesConfig::load();
    QCOMPARE(back.apiKey, QByteArray("k"));
    QVERIFY(back.allowInsecureHttp);
  }

  void envOverrides()
  {
    qputenv("PALEO_WELL_FACIES_URL", "https://env.example.com/api");
    qputenv("PALEO_WELL_FACIES_API_KEY", "env-key");
    qputenv("PALEO_WELL_FACIES_ALLOW_INSECURE_HTTP", "1");
    const WellFaciesConfig c = WellFaciesConfig::load();
    QCOMPARE(c.baseUrl, QUrl(QStringLiteral("https://env.example.com/api")));
    QCOMPARE(c.apiKey, QByteArray("env-key"));
    QVERIFY(c.allowInsecureHttp);
    qunsetenv("PALEO_WELL_FACIES_URL");
    qunsetenv("PALEO_WELL_FACIES_API_KEY");
    qunsetenv("PALEO_WELL_FACIES_ALLOW_INSECURE_HTTP");
  }

  void keychainCanBeDisabled()
  {
    qputenv("PALEO_WELL_FACIES_NO_KEYCHAIN", "1");
    QVERIFY(!WellFaciesKeyStore::available());
    bool called = false;
    WellFaciesKeyStore::read(this, [&](bool found, const QByteArray &, const QString &) {
      called = true;
      QVERIFY(!found);
    });
    QVERIFY(called); // 不可用时同步回调，调用方立即回落文件
  }
};

QTEST_GUILESS_MAIN(TestWellFaciesConfig)
#include "tst_wellfaciesconfig.moc"
