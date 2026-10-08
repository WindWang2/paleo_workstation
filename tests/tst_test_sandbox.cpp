// tst_test_sandbox — 测试沙箱自身的契约钉（方向 81）。
//
// add_paleo_test 给每个 ctest 项一套私有用户态：Linux/macOS 走 XDG/HOME 环境
// （QSettings NativeFormat = $XDG_CONFIG_HOME 下的 ini），Windows 走
// tests/support/paleo_test_registry_sandbox.cpp 的私有注册表 hive（HKCU 重映射）。
// 本测试断言「QSettings("paleo","paleo") 写入落在沙箱里、真实用户配置不受影响」，
// 沙箱坏掉（env 未注入、hook 未链入、重映射失效）即红——Windows 侧 CI 可验。
#include <QtTest>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QUuid>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <sddl.h>

namespace
{
// 真实用户 hive = HKEY_USERS\<当前用户 SID>（HKEY_USERS 不在重映射之列；
// 不用 RegOpenCurrentUser——Wine 等实现把它退化成 HKEY_CURRENT_USER 伪句柄）。
HKEY openRealUserHive()
{
  HANDLE token = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
    return nullptr;
  BYTE buf[512];
  DWORD len = 0;
  HKEY hive = nullptr;
  if (::GetTokenInformation(token, TokenUser, buf, sizeof(buf), &len))
  {
    LPWSTR sid = nullptr;
    if (::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buf)->User.Sid, &sid))
    {
      if (::RegOpenKeyExW(HKEY_USERS, sid, 0, KEY_READ, &hive) != ERROR_SUCCESS)
        hive = nullptr;
      ::LocalFree(sid);
    }
  }
  ::CloseHandle(token);
  return hive;
}
} // namespace
#endif

class TestTestSandbox : public QObject
{
  Q_OBJECT

private slots:
  void settingsLandInSandbox()
  {
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString key = QStringLiteral("paleo-sandbox-probe/token");
    {
      QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
#ifdef Q_OS_WIN
      // 每次运行 hive 从空白开始：上一轮的探针值不得残留。
      QVERIFY2(!s.contains(key), "registry sandbox not fresh: probe from a previous run survived");
#endif
      s.setValue(key, token);
      s.sync();
      QCOMPARE(s.status(), QSettings::NoError);
    }
    QSettings back(QStringLiteral("paleo"), QStringLiteral("paleo"));
    QCOMPARE(back.value(key).toString(), token); // 沙箱内往返

#ifdef Q_OS_WIN
    const QString active = qEnvironmentVariable("PALEO_TEST_REGISTRY_SANDBOX_ACTIVE");
    qInfo("registry sandbox: %s", qPrintable(active.isEmpty() ? QStringLiteral("<inactive>") : active));
    QVERIFY2(!active.isEmpty(),
             "Windows registry sandbox inactive — paleo_test_registry_sandbox.cpp not linked "
             "or RegLoadAppKey/RegOverridePredefKey failed (see stderr)");
    // ctest 注入了 hive 路径时，第一档（私有 app hive）必须生效——退到第二档
    //（真实 HKCU 下的专用键）只算降级，不算沙箱合格。
    if (qEnvironmentVariableIsSet("PALEO_TEST_REGISTRY_HIVE"))
      QVERIFY2(!active.startsWith(QStringLiteral("hkcu-key:")),
               qPrintable(QStringLiteral("app hive tier failed, degraded to %1").arg(active)));
    if (qEnvironmentVariableIsSet("PALEO_TEST_REGISTRY_HIVE"))
      QVERIFY2(QFileInfo::exists(qEnvironmentVariable("PALEO_TEST_REGISTRY_HIVE")),
               qPrintable(QStringLiteral("hive file missing: %1 (active=%2)")
                              .arg(qEnvironmentVariable("PALEO_TEST_REGISTRY_HIVE"), active)));
    // 真实用户 hive 的 Software\paleo\paleo 不得出现本轮探针值。
    HKEY realUser = openRealUserHive();
    QVERIFY2(realUser, "cannot open HKEY_USERS\\<sid>");
    wchar_t buf[128] = {};
    DWORD bytes = sizeof(buf);
    const LSTATUS rc = ::RegGetValueW(realUser, L"Software\\paleo\\paleo\\paleo-sandbox-probe",
                                      L"token", RRF_RT_REG_SZ, nullptr, buf, &bytes);
    ::RegCloseKey(realUser);
    QVERIFY2(rc != ERROR_SUCCESS || QString::fromWCharArray(buf) != token,
             "probe leaked into the real HKCU\\Software\\paleo\\paleo");
#else
    // POSIX：ctest 注入 XDG_CONFIG_HOME，NativeFormat 文件必须落在它下面。
    const QString xdg = qEnvironmentVariable("XDG_CONFIG_HOME");
    QVERIFY2(!xdg.isEmpty(), "XDG_CONFIG_HOME not injected — run via ctest (paleo_test_sandbox)");
    const QString file = QFileInfo(back.fileName()).absoluteFilePath();
    QVERIFY2(file.startsWith(QDir(xdg).absolutePath() + QLatin1Char('/')),
             qPrintable(QStringLiteral("%1 not under %2").arg(file, xdg)));
#endif
    back.remove(QStringLiteral("paleo-sandbox-probe"));
  }
};

QTEST_GUILESS_MAIN(TestTestSandbox)
#include "tst_test_sandbox.moc"
