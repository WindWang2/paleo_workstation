// 层：视图
#pragma once

#include <QCoreApplication>
#include <QSettings>
#include <QStringList>

// paleomainwindow_internal — 主窗家族 TU 共用的文件域助手（方向 83 自本体
// 匿名 namespace 收敛成内部契约头，先例：方向 65 canvas 拆分的内部头；
// 页序表 kPageIds 仍在 pages/pageshared.h）。翻译列表保持函数局部 static
// 惰性构建——首调发生在 buildShell（QApplication 已在场），命名空间级
// 常量会在 main 前静态初始化，漏翻译。
namespace paleo::mainwindow_internal {

// Tab order = reading order = right-panel stack order：页序表收敛到
// pages/pageshared.h 的 kPageIds（W4：attach 接线 TU 同查页序）。
// ribbon 页签文案（2026-10-05：数据管理后插入独立地层对比）。
inline const QStringList &pageLabels()
{
  static const QStringList labels = {
      QCoreApplication::translate("PaleoMainWindow", "数据管理"),
      QCoreApplication::translate("PaleoMainWindow", "地层对比"),
      QCoreApplication::translate("PaleoMainWindow", "智能预测"),
      QCoreApplication::translate("PaleoMainWindow", "单因素图"),
      QCoreApplication::translate("PaleoMainWindow", "智能编图"),
      QCoreApplication::translate("PaleoMainWindow", "验证"),
  };
  return labels;
}

// 右侧 dock 标题随页：数据页是属性面，编图页是参数面，验证页是结果面。
inline const QStringList &pageDockTitles()
{
  static const QStringList titles = {
      QCoreApplication::translate("PaleoMainWindow", "数据属性"),
      QCoreApplication::translate("PaleoMainWindow", "地层对比连接"),
      QCoreApplication::translate("PaleoMainWindow", "预测参数"),
      QCoreApplication::translate("PaleoMainWindow", "单因素参数"),
      QCoreApplication::translate("PaleoMainWindow", "编图参数"),
      QCoreApplication::translate("PaleoMainWindow", "验证结果"),
  };
  return titles;
}

inline QStringList readRecentProjects()
{
  QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
  QStringList recent = s.value(QStringLiteral("recentProjects")).toStringList();
  if (recent.isEmpty()) // alternate flat-key spelling, kept as a courtesy
    recent = QSettings().value(QStringLiteral("paleo/recentProjects")).toStringList();
  return recent;
}

inline void writeRecentProjects(const QStringList &recent)
{
  QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
  s.setValue(QStringLiteral("recentProjects"), recent);
}

} // namespace paleo::mainwindow_internal
