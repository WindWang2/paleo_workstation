// 层：数据
#pragma once
#include <QString>
#include <functional>

namespace paleo::python {
// A deployment caller supplies the independent package anchor. This API never
// enables execution, starts Python, downloads packages or grants redistribution.
struct DeliveryRequest {
  QString packagePath, packageSha256, storeRoot;
};
struct DeliveryResult {
  QString code = "python_delivery_invalid";
  QString kind, root, manifestSha256, licenseStatus;
  bool alreadyInstalled = false;
  bool ready() const { return code == QLatin1String("ok"); }
};
DeliveryResult installDelivery(const DeliveryRequest &request,
                              const std::function<bool()> &cancelled = {});
} // namespace paleo::python
