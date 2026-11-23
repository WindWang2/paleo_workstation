// 层：测试壳
#pragma once
// H2 测试专用（tst_h2_sessionarchive / tst_h2_capacitygates 共用）：把文件字节读进
// 调用方给的变量；读不到就在该处让用例失败。
//
// QFile 临时对象未打开时 readAll() 返回空——直接比较会让「原文件字节保留」「磁盘
// 不变」这类断言退化成空对空的恒真。这里打开失败或读到空都判失败，且每次读取都是
// 一次独立的断言：不存在「两侧都读不到却相等」的退化路径。
// 只能在返回 void 的测试槽内调用（QtTest 的 QVERIFY 是 `if (!cond) return;`）。
#include <QByteArray>
#include <QFile>
#include <QString>
#include <QtTest>

inline void requireFileBytes(const QString &path, QByteArray *out, const char *what)
{
  QFile file(path);
  const bool opened = file.open(QIODevice::ReadOnly);
  QVERIFY2(opened, qPrintable(QStringLiteral("%1：无法打开 %2（%3）")
                                .arg(QLatin1String(what), path, file.errorString())));
  const QByteArray bytes = file.readAll();
  QVERIFY2(!bytes.isEmpty(), qPrintable(QStringLiteral("%1：%2 读到 0 字节")
                                            .arg(QLatin1String(what), path)));
  *out = bytes;
}
