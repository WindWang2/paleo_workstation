#pragma once
// 层：数据
//
// 受管文件路径/校验原语（review-fix M2：自 DataCatalog 静态方法下沉）。
// metadata 层（mapversionstore/mapsnapshot）曾为这三件小工具反向 include
// catalog/datacatalog.h，与 catalog→metadata 构成模块环；下沉到 metadata 后
// DataCatalog 同名静态方法转发至此，语义逐字节不变。

#include <QString>

#include <functional>

namespace paleo::managedpath
{
// 文件 SHA-256（小写十六进制）；读失败/取消回空串并写 error。
QString sha256FileHex(const QString &path, QString *error = nullptr,
                      const std::function<bool()> &cancelled = {});
// 单段路径安全：拒空段、"."、含 ".."、斜杠/反斜杠、控制字符。
bool isSafePathSegment(const QString &segment);
// 工程相对受管路径 → 绝对路径；空路径/不安全/越界/符号链接回空串。
QString resolveManagedPath(const QString &projectDir, const QString &relativePath);
} // namespace paleo::managedpath
