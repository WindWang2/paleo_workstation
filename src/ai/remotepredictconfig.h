// 层：功能
#pragma once
#include <QJsonObject>
#include <QString>
#include <QUrl>

// 远端推理端点配置（方向51：把替身 Mock 换成真实可配路由的最后一块拼图）。
//
// 诚实纪律：
//  - 无缺省第三方地址：URL 必须显式配置（或由环境变量给出），缺省 = 未配置
//    = UI 明说「远端预测未配置，走本地引擎」，不再无感跑 Mock。
//  - 传输安全：只接受 https://；http:// 仅 loopback 或显式开关（同 WellFacies 口径）。
//  - 配置落在用户目录的配置文件里，**不进工程文件**（ addresses are user
//    environment, not project data）。
struct RemotePredictConfig {
  QUrl baseUrl;
  bool enabled = true;      // 配置了地址也要允许整机禁用（离线工区）
  QString model;            // 本地降级模型名（空 = 不用本地降级）

  static QString path();
  static RemotePredictConfig load(); // JSON + 环境变量（env 优先，便于测试/离群部署）
  bool save(QString *error) const;
  // 空串 = 可发起；非空 = 不可发 + 原因（面向用户，已翻译）。
  QString validate() const;
  bool usable() const { return enabled && validate().isEmpty(); }
  // UI 一句话状态（诚实：未配置就说未配置）。
  QString statusHint() const;

  QJsonObject toJson() const;
  static RemotePredictConfig fromJson(const QJsonObject &object);
  static RemotePredictConfig fromParts(const QUrl &baseUrl, bool enabled,
                                       const QString &model);
  static bool isLoopbackHost(const QString &host);
};
