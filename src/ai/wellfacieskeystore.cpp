// 层：功能
#include "wellfacieskeystore.h"
#include <QObject>
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
const QString kService = QStringLiteral("paleo_workstation/well-facies");
const QString kAccount = QStringLiteral("api-key");
#endif
} // namespace

bool WellFaciesKeyStore::available() {
#ifdef PALEO_HAVE_QTKEYCHAIN
  return qEnvironmentVariable("PALEO_WELL_FACIES_NO_KEYCHAIN") !=
         QLatin1String("1");
#else
  return false;
#endif
}

void WellFaciesKeyStore::read(QObject *context, ReadCallback done) {
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
  done(false, {}, QObject::tr("系统钥匙串不可用"));
}

void WellFaciesKeyStore::write(QObject *context, const QByteArray &key,
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
  Q_UNUSED(context);
  Q_UNUSED(key);
  done(false, QObject::tr("系统钥匙串不可用"));
}
