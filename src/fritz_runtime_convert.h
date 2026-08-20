#pragma once

// Conversions between the Qt-free SDK value types and the Qt types this adapter
// works in, plus the MAC-selection helpers shared by schema and runtime.

#include <cstdint>
#include <optional>
#include <string>

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QSet>
#include <QString>

#include "phi/adapter/sdk/sidecar.h"

namespace phicore::fritz::ipc {

std::int64_t nowMs();

std::string toJson(const QJsonObject &obj);

/// Returns an empty object for empty, malformed or non-object text.
QJsonObject parseJsonObject(const std::string &text);

std::optional<bool> scalarToBool(const phicore::adapter::v1::ScalarValue &value);

/// Accepts a bare string, an array of strings, or objects carrying `value`/`mac`.
QSet<QString> parseTrackedMacSelection(const QJsonValue &value);

/// Stable, locale-aware ordering so a meta patch does not churn on reordering.
QJsonArray sortedMacArray(const QSet<QString> &macs);

} // namespace phicore::fritz::ipc
