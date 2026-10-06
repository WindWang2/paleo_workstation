// 层：功能
#pragma once
#include "chatmessage.h"
#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

// 一次会话的内存形态 + 磁盘形态。
//
// 落盘纪律（方向51）：会话文件只写用户的 AppLocalDataLocation 目录，
// **不进工程目录、不进 .qgz/.gpkg**——对话不是解释成果，混进去会污染
// 版本与发布面。路径由 ChatSessionStore::directory() 单点给出（测试按此断言
// 「不在工程里」）。
struct ChatSession {
  QString id;
  QString title;            // 用户可见标题（首条用户消息截取）
  QString model;            // 使用的模型名（配置快照，便于事后追溯）
  QDateTime createdAt;
  QDateTime updatedAt;
  QVector<ChatMessage> messages;

  bool isEmpty() const { return id.isEmpty() || messages.isEmpty(); }
  // 标题兜底：无标题时取首条用户消息的前 40 个字符。
  QString displayTitle() const;
  QJsonObject toJson() const;
  static ChatSession fromJson(const QJsonObject &object);
};

// 会话存储（静态函数集合；无状态，测试可直接驱动）。
class ChatSessionStore {
public:
  // 会话目录（不存在时创建）。
  static QString directory();
  static QString pathFor(const QString &sessionId);
  static QString newSessionId();
  static QStringList listIds();
  static bool save(const ChatSession &session, QString *error);
  static bool load(const QString &sessionId, ChatSession *out, QString *error);
  static bool remove(const QString &sessionId, QString *error);
  // 最近会话优先：按 updatedAt 倒序。
  static QVector<ChatSession> loadRecent(int limit = 20);
};
