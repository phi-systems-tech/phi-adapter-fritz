#pragma once

// What this particular router implements, learned rather than assumed.
//
// TR-064 action names differ by model line and firmware, and a router says so
// plainly: an action it does not have comes back as a SOAP fault with error 401,
// "Invalid Action". The adapter used to ignore that - `if (result.success)` -
// and ask again five seconds later, forever. On the box in the field that was
// three of its six router readings, and the answer to two of them had never
// reached a channel.
//
// A feature that has answered once is asked for again. One that has said it
// does not exist is not.

#include <array>
#include <cstddef>

namespace phicore::fritz::ipc {

enum class Feature {
    /// `X_AVM-DE_GetHostListPath`, which is the one that works, against the
    /// plain `GetHostListPath` the standard names.
    AvmHostListPath,
    StandardHostListPath,
    /// `UserInterface:1 GetInfo`, which is where the update state lives.
    UserInterfaceInfo,
    /// `DeviceInfo X_AVM-DE_GetAutoUpdateInfo`, the older place for it.
    AutoUpdateInfo,
    /// `GetAddonInfos`, which carries a ready-made rate.
    AddonInfos,
    /// `GetTotalBytesSent`/`Received`, from which a rate can be derived.
    ByteCounters,
    Wlan24,
    Wlan5,

    Count,
};

class RouterCapabilities
{
public:
    enum class Availability {
        Unknown,   ///< never tried, or tried and the call failed for another reason
        Present,
        Absent,    ///< the router said Invalid Action
    };

    [[nodiscard]] Availability availability(Feature feature) const
    {
        return m_state[static_cast<std::size_t>(feature)];
    }

    /// Whether it is worth issuing. Unknown is worth one try; Absent never is.
    [[nodiscard]] bool worthTrying(Feature feature) const
    {
        return availability(feature) != Availability::Absent;
    }

    void markPresent(Feature feature)
    {
        m_state[static_cast<std::size_t>(feature)] = Availability::Present;
    }

    void markAbsent(Feature feature)
    {
        m_state[static_cast<std::size_t>(feature)] = Availability::Absent;
    }

    /**
     * @brief Forgets everything learned.
     *
     * For a new endpoint or a firmware that may have changed under us. Not for
     * a failed poll: a router that is unreachable has not told us anything
     * about what it implements.
     */
    void forget() { m_state.fill(Availability::Unknown); }

private:
    std::array<Availability, static_cast<std::size_t>(Feature::Count)> m_state{};
};

} // namespace phicore::fritz::ipc
