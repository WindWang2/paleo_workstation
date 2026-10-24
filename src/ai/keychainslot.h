// 层：功能
#pragma once
// PW-122：LlmKeyStore 与 WellFaciesKeyStore 共用的唯一钥匙串实现。
// 头文件内联（不新增编译单元，免动 cmake）；只应被 keystore .cpp 包含。
//
// 纪律：null context → done 立即带错误回调（不会永不回调）；context 被销毁后
// 回调静默丢弃；没有 QtKeychain 或 disableEnv=1 时 available() 为 false。
#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <functional>

#ifdef PALEO_HAVE_QTKEYCHAIN
#if __has_include(<qt6keychain/keychain.h>)
#include <qt6keychain/keychain.h>
#else
#include <keychain.h>
#endif
#endif

struct KeychainSlot {
  QString service;
  QString account;
  const char *disableEnv = nullptr;

  using ReadCallback =
      std::function<void(bool ok, const QByteArray &key, const QString &error)>;
  using WriteCallback = std::function<void(bool ok, const QString &error)>;

  static QString nullContextError() {
    return QStringLiteral("internal error: keystore called without context");
  }
  static QString unavailableError() { return QObject::tr("系统钥匙串不可用"); }

  bool available() const {
#ifdef PALEO_HAVE_QTKEYCHAIN
    return !disableEnv || qEnvironmentVariable(disableEnv) != QLatin1String("1");
#else
    return false;
#endif
  }

  void read(QObject *context, ReadCallback done) const {
    if (!context) {
      done(false, {}, nullContextError());
      return;
    }
#ifdef PALEO_HAVE_QTKEYCHAIN
    if (available()) {
      auto *job = new QKeychain::ReadPasswordJob(service);
      job->setAutoDelete(true);
      job->setKey(account);
      QPointer<QObject> guard(context);
      QObject::connect(job, &QKeychain::Job::finished, context,
                       [job, guard, done](QKeychain::Job *) {
                         if (!guard)
                           return;
                         if (job->error() == QKeychain::NoError)
                           done(true, job->binaryData(), {});
                         else if (job->error() == QKeychain::EntryNotFound)
                           done(false, {}, {});
                         else
                           done(false, {}, job->errorString());
                       });
      job->start();
      return;
    }
#endif
    done(false, {}, unavailableError());
  }

  void write(QObject *context, const QByteArray &key, WriteCallback done) const {
    if (!context) {
      done(false, nullContextError());
      return;
    }
#ifdef PALEO_HAVE_QTKEYCHAIN
    if (available()) {
      auto *job = new QKeychain::WritePasswordJob(service);
      job->setAutoDelete(true);
      job->setKey(account);
      job->setBinaryData(key);
      QPointer<QObject> guard(context);
      QObject::connect(job, &QKeychain::Job::finished, context,
                       [job, guard, done](QKeychain::Job *) {
                         if (!guard)
                           return;
                         done(job->error() == QKeychain::NoError, job->errorString());
                       });
      job->start();
      return;
    }
#endif
    Q_UNUSED(key)
    done(false, unavailableError());
  }

  // 条不存在也算清干净了（幂等）。
  void clear(QObject *context, WriteCallback done) const {
    if (!context) {
      done(false, nullContextError());
      return;
    }
#ifdef PALEO_HAVE_QTKEYCHAIN
    if (available()) {
      auto *job = new QKeychain::DeletePasswordJob(service);
      job->setAutoDelete(true);
      job->setKey(account);
      QPointer<QObject> guard(context);
      QObject::connect(job, &QKeychain::Job::finished, context,
                       [job, guard, done](QKeychain::Job *) {
                         if (!guard)
                           return;
                         done(job->error() == QKeychain::NoError ||
                                  job->error() == QKeychain::EntryNotFound,
                              job->errorString());
                       });
      job->start();
      return;
    }
#endif
    done(false, unavailableError());
  }
};
