// 层：功能
#pragma once
#include <QByteArray>
#include <QString>
#include <functional>
class QObject;

// 测井相预测 API 密钥的系统钥匙串存取（#133）。
//
// 构建期找到 Qt6Keychain 时定义 PALEO_HAVE_QTKEYCHAIN，密钥进系统钥匙串
// （Windows 凭据管理器 / Linux Secret Service(libsecret/KWallet) / macOS
// 钥匙串）；否则 available() 为 false，调用方回落 JSON 文件（POSIX 0600）。
// PALEO_WELL_FACIES_NO_KEYCHAIN=1 强制关闭（ctest 沙箱用，避免测试碰真实
// 用户钥匙串）。全部接口异步：回调在 context 所在线程、context 存活时触发。
namespace WellFaciesKeyStore {
bool available();
using ReadCallback =
    std::function<void(bool found, const QByteArray &key, const QString &error)>;
using WriteCallback = std::function<void(bool ok, const QString &error)>;
void read(QObject *context, ReadCallback done);
void write(QObject *context, const QByteArray &key, WriteCallback done);
} // namespace WellFaciesKeyStore
