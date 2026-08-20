#include "fritz_runtime_convert.h"

#include <algorithm>
#include <type_traits>
#include <variant>

#include <QDateTime>
#include <QJsonDocument>
#include <QStringList>

#include "fritz_tr064.h"

namespace phicore::fritz::ipc {

namespace v1 = phicore::adapter::v1;

std::int64_t nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

QJsonObject parseJsonObject(const std::string &text)
{
    if (text.empty())
        return {};
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(text), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return {};
    return doc.object();
}

std::string toJson(const QJsonObject &obj)
{
    return QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString();
}

QSet<QString> parseTrackedMacSelection(const QJsonValue &value)
{
    QSet<QString> out;
    auto addValue = [&out](const QJsonValue &entry) {
        QString mac;
        if (entry.isString()) {
            mac = normalizeMac(entry.toString());
        } else if (entry.isObject()) {
            const QJsonObject obj = entry.toObject();
            mac = normalizeMac(obj.value(QStringLiteral("value")).toString());
            if (mac.isEmpty())
                mac = normalizeMac(obj.value(QStringLiteral("mac")).toString());
        }
        if (!mac.isEmpty())
            out.insert(mac);
    };

    if (value.isArray()) {
        const QJsonArray arr = value.toArray();
        for (const QJsonValue &entry : arr)
            addValue(entry);
        return out;
    }

    addValue(value);
    return out;
}

QJsonArray sortedMacArray(const QSet<QString> &macs)
{
    QStringList sorted;
    sorted.reserve(macs.size());
    for (const QString &mac : macs)
        sorted.push_back(mac);
    std::sort(sorted.begin(), sorted.end(), [](const QString &a, const QString &b) {
        return a.localeAwareCompare(b) < 0;
    });
    QJsonArray out;
    for (const QString &mac : sorted)
        out.append(mac);
    return out;
}

std::optional<bool> scalarToBool(const v1::ScalarValue &value)
{
    if (const auto *v = std::get_if<bool>(&value))
        return *v;
    if (const auto *v = std::get_if<std::int64_t>(&value))
        return *v != 0;
    if (const auto *v = std::get_if<double>(&value))
        return *v != 0.0;
    if (const auto *v = std::get_if<v1::Utf8String>(&value)) {
        const QString text = QString::fromStdString(*v).trimmed().toLower();
        if (text == QLatin1String("true") || text == QLatin1String("1") || text == QLatin1String("on"))
            return true;
        if (text == QLatin1String("false") || text == QLatin1String("0") || text == QLatin1String("off"))
            return false;
    }
    return std::nullopt;
}

} // namespace phicore::fritz::ipc
