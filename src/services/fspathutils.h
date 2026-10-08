// 层：数据
// #291：QString ↔ std::filesystem::path 的唯一正确通道。Windows/MSVC 上
// path 窄字符构造按 ANSI 代码页解码、path::string() 返回 ANSI 字节——
// toStdString()/fromStdString() 会在中文路径上进退皆乱码。必须走 UTF-16。
#pragma once

#include <QString>
#include <string>
#include <filesystem>

namespace paleo {

// QString(UTF-16) → path：宽构造，中文/Unicode 路径在所有平台无损。
// 不用 toStdString()——MSVC 上窄字符构造按活动代码页（ACP）解码，中文路径
// 进去即乱码。
inline std::filesystem::path toFsPath(const QString &q)
{
  return std::filesystem::path(q.toStdU16String());
}

// path → QString：u16string() 原样取回宽字符，不经过 ANSI 窄字节。
// 不用 path::string()——MSVC 上返回 ACP 字节，遇 ACP 无法表示的字符还会抛
// std::system_error。
inline QString fromFsPath(const std::filesystem::path &p)
{
  return QString::fromStdU16String(p.u16string());
}

} // namespace paleo
