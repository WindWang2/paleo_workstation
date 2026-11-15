// 层：数据
#pragma once
#include "pythonenvironmentconfig.h"
class QFileDevice;

namespace paleo::python::environment_config {
inline constexpr qsizetype MaxConfigurationBytes = 256 * 1024;
inline constexpr qsizetype MaxApproved = 64;
QByteArray canonical(const QJsonObject &);
QString identityId(const EnvironmentIdentity &);
bool parseIdentity(const QJsonObject &, EnvironmentIdentity &);
bool parseConfiguration(const QByteArray &, EnvironmentConfiguration &);
QString verifyScripts(const EnvironmentIdentity &, const std::function<bool()> &);
bool syncConfigurationFile(QFileDevice &);
} // namespace paleo::python::environment_config
