// 层：功能
#include "llmkeystore.h"
#include "../keychainslot.h"

namespace {
const char kDisableEnv[] = "PALEO_LLM_NO_KEYCHAIN";
// PW-122：实现收敛到 KeychainSlot（与 WellFaciesKeyStore 共用）。
const KeychainSlot &slot() {
  static const KeychainSlot s{QStringLiteral("paleo_workstation/llm-chat"),
                              QStringLiteral("api-key"), kDisableEnv};
  return s;
}
} // namespace

QString LlmKeyStore::disabledByEnvFlagName() { return QString::fromUtf8(kDisableEnv); }

bool LlmKeyStore::available() { return slot().available(); }

void LlmKeyStore::read(QObject *context, ReadCallback done) {
  slot().read(context, std::move(done));
}

void LlmKeyStore::write(QObject *context, const QByteArray &key, WriteCallback done) {
  slot().write(context, key, std::move(done));
}

void LlmKeyStore::clear(QObject *context, WriteCallback done) {
  slot().clear(context, std::move(done));
}
