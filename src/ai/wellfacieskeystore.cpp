// 层：功能
#include "wellfacieskeystore.h"
#include "keychainslot.h"

namespace {
// PW-122：实现收敛到 KeychainSlot（与 LlmKeyStore 共用）。
const KeychainSlot &slot() {
  static const KeychainSlot s{QStringLiteral("paleo_workstation/well-facies"),
                              QStringLiteral("api-key"), "PALEO_WELL_FACIES_NO_KEYCHAIN"};
  return s;
}
} // namespace

bool WellFaciesKeyStore::available() { return slot().available(); }

void WellFaciesKeyStore::read(QObject *context, ReadCallback done) {
  slot().read(context, std::move(done));
}

void WellFaciesKeyStore::write(QObject *context, const QByteArray &key,
                               WriteCallback done) {
  slot().write(context, key, std::move(done));
}
