#include "fritz_state.h"

namespace phicore::fritz::ipc {

namespace v1 = phicore::adapter::v1;

namespace {

std::string key(const std::string &deviceId, const std::string &channelId)
{
    return deviceId + "\x1f" + channelId;
}

} // namespace

bool ReportedValues::isNews(const std::string &deviceId,
                            const std::string &channelId,
                            const v1::ScalarValue &value)
{
    const std::string k = key(deviceId, channelId);
    const auto it = m_values.find(k);
    if (it != m_values.end() && it->second == value)
        return false;
    m_values[k] = value;
    return true;
}

bool ReportedValues::descriptorIsNews(const std::string &deviceId, const std::string &fingerprint)
{
    const auto it = m_descriptors.find(deviceId);
    if (it != m_descriptors.end() && it->second == fingerprint)
        return false;
    m_descriptors[deviceId] = fingerprint;
    return true;
}

void ReportedValues::forget()
{
    m_values.clear();
    m_descriptors.clear();
}

void ReportedValues::forgetDevice(const std::string &deviceId)
{
    m_descriptors.erase(deviceId);
    forgetValues(deviceId);
}

void ReportedValues::forgetValues(const std::string &deviceId)
{
    const std::string prefix = deviceId + "\x1f";
    for (auto it = m_values.begin(); it != m_values.end();) {
        if (it->first.rfind(prefix, 0) == 0)
            it = m_values.erase(it);
        else
            ++it;
    }
}

std::optional<double> RateMeter::sample(std::uint64_t totalBytes, std::int64_t nowMs)
{
    const bool hadPrevious = m_hasPrevious;
    const std::uint64_t previousBytes = m_previousBytes;
    const std::int64_t previousMs = m_previousMs;

    m_hasPrevious = true;
    m_previousBytes = totalBytes;
    m_previousMs = nowMs;

    if (!hadPrevious)
        return std::nullopt;
    if (totalBytes < previousBytes)
        return std::nullopt;   // the counter wrapped, or the router restarted
    const std::int64_t elapsedMs = nowMs - previousMs;
    if (elapsedMs <= 0)
        return std::nullopt;

    const double bytesPerSecond =
        static_cast<double>(totalBytes - previousBytes) * 1000.0 / static_cast<double>(elapsedMs);
    return (bytesPerSecond * 8.0) / 1000.0;
}

void RateMeter::forget()
{
    m_hasPrevious = false;
    m_previousBytes = 0;
    m_previousMs = 0;
}

} // namespace phicore::fritz::ipc
