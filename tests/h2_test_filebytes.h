// 层：测试壳
#pragma once
// H2 测试专用（tst_h2_sessionarchive / tst_h2_capacitygates 共用）：磁盘字节的读取。
//
// 为什么需要它：`QFile(path).readAll()` 用**未打开**的临时对象时恒返回空，直接比较
// 会把「原文件字节保留」「磁盘不变」这类断言退化成空对空的恒真；即便显式打开，读失败
// 时返回空也会让两侧都读不到的比较仍然相等。
//
// 用法：REQUIRE_BYTES(表达式, 目标变量);  —— 读不到即在该语句处让用例失败。
// QtTest 的 QVERIFY 展开为 `if (!ok) return;`，宏必须用在返回 void 的测试槽内（本目录
// 所有调用点都是）。断言写在调用点的槽里而不是 inline 辅助函数里：失败会直接从槽返回，
// 后续比较不会在空缓冲上继续执行。
#include <QByteArray>
#include <QFile>
#include <QString>
#include <QtTest>

inline bool readFileBytes(const QString &path, QByteArray *out)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return false;
  const QByteArray bytes = file.readAll();
  if (bytes.isEmpty())
    return false;
  *out = bytes;
  return true;
}

// expr 只求值一次：调用点允许传入非纯表达式（如 ChatSessionStore::pathFor，它经
// directory() 会做幂等 mkpath），也避免成功分支为了拼失败消息再求值一遍。
#define REQUIRE_BYTES(expr, var)                                              \
  do {                                                                        \
    const QString requirePath_ = (expr);                                      \
    QVERIFY2(::readFileBytes(requirePath_, &(var)),                           \
             qPrintable(QStringLiteral("无法读取文件（不存在/打不开/空文件）：%1") \
                            .arg(requirePath_)));                             \
  } while (false)
