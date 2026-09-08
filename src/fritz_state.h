#pragma once

// What has already been said, so it is not said again.
//
// The adapter reported every value on every poll and left the filtering to
// phi-core. Core does drop unchanged values - and the device path was hardened
// for exactly this (F-66) - but the work still happens: a descriptor parsed, a
// registry reconciled, channels marked stale and purged, three times every five
// seconds, to conclude that nothing moved.

#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include "phi/adapter/sdk/sidecar.h"

namespace phicore::fritz::ipc {

class ReportedValues
{
public:
    /// True when this differs from the last value reported for the channel,
    /// and records it. False means there is nothing to send.
    bool isNews(const std::string &deviceId,
                const std::string &channelId,
                const phicore::adapter::v1::ScalarValue &value);

    /**
     * @brief The same question for a device descriptor.
     *
     * `fingerprint` is whatever the descriptor is built from; two equal ones
     * describe the same device, so the second need not be sent.
     */
    bool descriptorIsNews(const std::string &deviceId, const std::string &fingerprint);

    void forget();
    void forgetDevice(const std::string &deviceId);

    /**
     * @brief Forgets the values of one device but not its descriptor.
     *
     * For the moment a device is announced with a channel it did not have
     * before. phi-core rejects a state update for a channel it does not know
     * yet - "Received invalid channel id" - and the adapter never hears about
     * it, because the send itself succeeded. Anything already recorded as
     * reported would then never be sent again. Forgetting the values, and only
     * the values, makes the next poll say them all once more.
     */
    void forgetValues(const std::string &deviceId);

private:
    std::map<std::string, phicore::adapter::v1::ScalarValue> m_values;
    std::map<std::string, std::string> m_descriptors;
};

/**
 * @brief Turns a monotonic byte counter into a rate.
 *
 * The router this was measured against does not implement `GetAddonInfos`,
 * which is where the rate used to come from, so it is derived from the totals
 * instead. The counter is 32 bits on a FRITZ!Box and wraps; a sample that went
 * backwards is dropped rather than reported as a negative rate or a very large
 * positive one.
 */
class RateMeter
{
public:
    /// Kilobits per second since the previous sample, or nothing for the first
    /// one, a wrap, or two samples too close together to divide by.
    std::optional<double> sample(std::uint64_t totalBytes, std::int64_t nowMs);
    void forget();

private:
    bool m_hasPrevious = false;
    std::uint64_t m_previousBytes = 0;
    std::int64_t m_previousMs = 0;
};

} // namespace phicore::fritz::ipc
