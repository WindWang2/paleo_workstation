// 层：功能
#pragma once
#include <QByteArray>
#include <QObject>
#include <functional>

// ai/chat — LLM API 密钥存取（方向51）。
//
// 纪律照 WellFaciesKeyStore 的现成口径：密钥只进系统钥匙串（Windows 凭据管理
// 器 / libsecret / macOS 钥匙串），**不落 JSON、不进工程文件、不进环境变量日志**。
// 没有 QtKeychain 的构建里 available() 恒 false——缺口如实暴露（UI 呈禁用态），
// 不退化成明文文件。
class LlmKeyStore {
public:
  static bool available();
  // done(ok, key, error)：ok=false 且 error 为空 = 钥匙串里没有这条密钥。
  using ReadCallback =
    std::function<void(bool ok, const QByteArray &key, const QString &error)>;
  using WriteCallback = std::function<void(bool ok, const QString &error)>;
  static void read(QObject *context, ReadCallback done);
  static void write(QObject *context, const QByteArray &key,
                    WriteCallback done);
  static void clear(QObject *context, WriteCallback done);
  // 测试/离线封装逃生口：PALEO_LLM_NO_KEYCHAIN=1 时禁用系统钥匙串。
  static QString disabledByEnvFlagName();
};
