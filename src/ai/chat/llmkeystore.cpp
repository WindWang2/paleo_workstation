// 层：功能
#include "llmkeystore.h"
#include <QPointer>

#ifdef PALEO_HAVE_QTKEYCHAIN
#if __has_include(<qt6keychain/keychain.h>)
#include <qt6keychain/keychain.h>
#else
#include <keychain.h>
#endif
#endif

namespace {
#ifdef PALEO_HAVE_QTKEYCHAIN
const QString kService = QStringLiteral("paleo_workstation/llm-chat");
const QString kAccount = QStringLiteral("api-key");
#endif
const char kDisableEnv[] = "PALEO_LLM_NO_KEYCHAIN";
} // namespace

QString LlmKeyStore::disabledByEnvFlagName() {
  return QString::fromUtf8(kDisableEnv);
}

bool LlmKeyStore::available() {
#ifdef PALEO_HAVE_QTKEYCHAIN
  return qEnvironmentVariable(kDisableEnv) != QLatin1String("1");
#else
  return false;
#endif
}

void LlmKeyStore::read(QObject *context, ReadCallback done) {
#ifdef PALEO_HAVE_QTKEYCHAIN
  if (available()) {
    auto *job = new QKeychain::ReadPasswordJob(kService);
    job->setAutoDelete(true);
    job->setKey(kAccount);
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
  Q_UNUSED(context)
  done(false, {}, QObject::tr("系统钥匙串不可用"));
}

void LlmKeyStore::write(QObject *context, const QByteArray &key,
                        WriteCallback done) {
#ifdef PALEO_HAVE_QTKEYCHAIN
  if (available()) {
    auto *job = new QKeychain::WritePasswordJob(kService);
    job->setAutoDelete(true);
    job->setKey(kAccount);
    job->setBinaryData(key);
    QPointer<QObject> guard(context);
    QObject::connect(job, &QKeychain::Job::finished, context,
                     [job, guard, done](QKeychain::Job *) {
                       if (!guard)
                         return;
                       done(job->error() == QKeychain::NoError,
                            job->errorString());
                     });
    job->start();
    return;
  }
#endif
  Q_UNUSED(context)
  Q_UNUSED(key)
  done(false, QObject::tr("系统钥匙串不可用"));
}

void LlmKeyStore::clear(QObject *context, WriteCallback done) {
#ifdef PALEO_HAVE_QTKEYCHAIN
  if (available()) {
    auto *job = new QKeychain::DeletePasswordJob(kService);
    job->setAutoDelete(true);
    job->setKey(kAccount);
    QPointer<QObject> guard(context);
    QObject::connect(job, &QKeychain::Job::finished, context,
                     [job, guard, done](QKeychain::Job *) {
                       if (!guard)
                         return;
                       // 条不存在也算清干净了（幂等）。
                       done(job->error() == QKeychain::NoError ||
                              job->error() == QKeychain::EntryNotFound,
                            job->errorString());
                     });
    job->start();
    return;
  }
#endif
  Q_UNUSED(context)
  done(false, QObject::tr("系统钥匙串不可用"));
}
