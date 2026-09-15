#include "fritz_instance.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "phi/runtime/loop.h"
#include "phi/runtime/oneshots.h"
#include "phi/runtime/str.h"

#include "fritz_capabilities.h"
#include "fritz_devicemodel.h"
#include "fritz_json.h"
#include "fritz_schema.h"
#include "fritz_session.h"
#include "fritz_soap.h"
#include "fritz_state.h"
#include "fritz_tr064.h"

namespace phicore::fritz::ipc {

namespace str = phi::str;
namespace v1 = phicore::adapter::v1;
namespace sdk = phicore::adapter::sdk;

using namespace std::chrono_literals;

namespace {

/// How many fast polls go by before the things that change by the month are
/// asked for again: the WLAN switches, the firmware version, the update state.
constexpr int kSlowPollEvery = 12;

/// Three failed polls before connectivity is called lost.
constexpr int kFailuresBeforeDisconnected = 3;

std::int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::vector<std::string> macList(const Json &value)
{
    std::vector<std::string> macs;
    const auto append = [&macs](const std::string &raw) {
        const std::string mac = normalizeMac(raw);
        if (mac.empty() || std::find(macs.begin(), macs.end(), mac) != macs.end())
            return;
        macs.push_back(mac);
    };
    if (value.is_array()) {
        for (const Json &entry : value) {
            if (entry.is_string())
                append(entry.get<std::string>());
            else if (entry.is_object())
                append(jsonString(entry, entry.contains("value") ? "value" : "mac"));
        }
    } else if (value.is_string()) {
        append(value.get<std::string>());
    }
    return macs;
}

class FritzInstance final : public sdk::AdapterInstance
{
protected:
    bool start() override
    {
        m_loop = phi::runtime::Loop::current();
        if (m_loop == nullptr) {
            std::cerr << "fritz instance started off a loop; no timers are possible\n";
            return false;
        }
        m_session.emplace(*m_loop);
        m_shots.emplace(*m_loop);
        m_lifecycle = Lifecycle::Running;
        setConnected(false);
        // Nothing to poll until the first config.changed says where the router
        // is; the timer is armed there.
        return true;
    }

    void stop() override
    {
        m_lifecycle = Lifecycle::Stopped;
        answerPending("Instance stopped");
        setConnected(false);
        releaseLoopResources();
    }

    void onDisconnected() override
    {
        // phi-core went away, and comes back with the configuration. A pause,
        // not an end.
        m_lifecycle = Lifecycle::Paused;
        m_pollTimer.reset();
        m_pollRunning = false;
        m_pendingReports.clear();
        if (m_session)
            m_session->cancel();
        answerPending("Instance disconnected");
        m_reported.forget();
        m_txMeter.forget();
        m_rxMeter.forget();
        m_pollFailures = 0;
        setConnected(false);
    }

    void onConfigChanged(const sdk::ConfigChangedRequest &request) override
    {
        if (m_lifecycle == Lifecycle::Stopped)
            return;

        const std::string previousEndpoint = endpoint();
        m_info = request.adapter;
        m_meta = parseObject(request.adapter.metaJson);
        applyConfig();

        m_lifecycle = Lifecycle::Running;
        if (endpoint() != previousEndpoint) {
            // A different box. What the old one implemented says nothing about
            // this one, and neither does its byte counter.
            m_capabilities.forget();
            m_reported.forget();
            m_txMeter.forget();
            m_rxMeter.forget();
            m_routerAnnounced = false;
            setConnected(false);
        }
        m_pollFailures = 0;
        m_pollsSinceSlow = kSlowPollEvery;   // the first poll asks for everything

        std::cerr << "fritz-ipc config.changed adapterId=" << request.adapterId
                  << " externalId=" << m_info.externalId
                  << " tracked=" << m_trackedMacs.size() << '\n';

        armPollTimer();
        beginPoll();
    }

    void onChannelInvoke(const sdk::ChannelInvokeRequest &request) override
    {
        if (m_lifecycle != Lifecycle::Running || !m_session) {
            answerCommand(request.cmdId, v1::CmdStatus::TemporarilyOffline,
                          "Instance is not running");
            return;
        }
        if (request.deviceExternalId != kRouterDeviceId) {
            answerCommand(request.cmdId, v1::CmdStatus::NotSupported,
                          "Channel only supported for router device");
            return;
        }
        const std::optional<bool> enabled = scalarToBool(request.value);
        if (!enabled.has_value()) {
            answerCommand(request.cmdId, v1::CmdStatus::InvalidArgument, "Expected boolean value");
            return;
        }

        const bool is24 = request.channelExternalId == kChannelWlan24;
        const bool is5 = request.channelExternalId == kChannelWlan5;
        if (!is24 && !is5) {
            answerCommand(request.cmdId, v1::CmdStatus::NotSupported, "Channel not supported");
            return;
        }
        if (!m_session->addressable()) {
            answerCommand(request.cmdId, v1::CmdStatus::TemporarilyOffline, "Host/IP is required");
            return;
        }

        // A write is what somebody is waiting for; a poll is not.
        m_session->cancel();
        m_pollRunning = false;

        const Service &service = is24 ? kWlan24 : kWlan5;
        const std::string channel(request.channelExternalId);
        const v1::CmdId cmdId = request.cmdId;
        const bool wanted = *enabled;
        m_session->call(service, "SetEnable", {{"NewEnable", toSoapBoolean(wanted)}},
                        [this, cmdId, channel, wanted](Tr064Session::Reply reply) {
                            if (!reply.ok) {
                                answerCommand(cmdId, v1::CmdStatus::Failure,
                                              reply.error.empty() ? "SetEnable failed"
                                                                  : reply.error);
                                return;
                            }
                            v1::CmdResponse response = responseFor(cmdId, v1::CmdStatus::Success,
                                                                   {});
                            response.finalValue = wanted;
                            // The value is recorded as reported before it is
                            // sent, so the poll that is already on its way with
                            // the old reading cannot put the switch back.
                            report(kRouterDeviceId, channel, wanted);
                            sendCommandResponse(std::move(response));
                        });
    }

    void onAdapterActionInvoke(const sdk::AdapterActionInvokeRequest &request) override
    {
        const std::string actionId = str::trimmed(request.actionId);
        if (actionId == "settings") {
            handleSettings(request);
            return;
        }
        if (actionId == "browseHosts") {
            handleBrowseHosts(request);
            return;
        }
        answerAction(request.cmdId, v1::CmdStatus::NotSupported, "Adapter action not supported");
    }

    void onDeviceNameUpdate(const sdk::DeviceNameUpdateRequest &request) override
    {
        answerCommand(request.cmdId, v1::CmdStatus::NotImplemented, "Device rename not supported");
    }

    void onDeviceEffectInvoke(const sdk::DeviceEffectInvokeRequest &request) override
    {
        answerCommand(request.cmdId, v1::CmdStatus::NotImplemented, "Device effect not supported");
    }

    void onSceneInvoke(const sdk::SceneInvokeRequest &request) override
    {
        answerCommand(request.cmdId, v1::CmdStatus::NotImplemented,
                      "Scene invocation not supported");
    }

private:
    enum class Lifecycle { Idle, Running, Paused, Stopped };

    // --- configuration ----------------------------------------------------

    void applyConfig()
    {
        m_pollIntervalMs = std::clamp(jsonInt(m_meta, "pollIntervalMs", 5000), 1000, 300000);
        m_retryIntervalMs = std::clamp(jsonInt(m_meta, "retryIntervalMs", 10000), 1000, 300000);
        const std::uint16_t configured = normalizedPort(jsonInt(m_meta, "tr064Port", 0));
        const std::uint16_t discovered = normalizedPort(static_cast<int>(m_info.port));
        m_port = configured > 0 ? configured
                                : (discovered > 0 ? discovered : kDefaultTr064Port);

        m_trackedMacs.clear();
        if (m_meta.contains("trackedMacs"))
            m_trackedMacs = macList(m_meta.at("trackedMacs"));

        if (m_session) {
            m_session->setEndpoint(endpoint());
            m_session->setCredentials(m_info.user, m_info.password);
        }
    }

    std::string host() const
    {
        const std::string ip = str::trimmed(m_info.ip);
        if (!ip.empty())
            return ip;
        return str::trimmed(m_info.host);
    }

    std::string endpoint() const
    {
        const std::string address = host();
        if (address.empty())
            return {};
        // TLS is not offered by this client yet, and the adapter in the field
        // does not use it; a UseTls flag would produce an https URL the client
        // refuses with a plain message rather than a silent failure.
        const bool tls = v1::hasFlag(m_info.flags, v1::AdapterFlag::UseTls);
        return std::string(tls ? "https://" : "http://") + address + ":" + str::number(m_port);
    }

    // --- the loop ---------------------------------------------------------

    void armPollTimer()
    {
        if (!m_loop || m_lifecycle != Lifecycle::Running)
            return;
        const int interval = m_connected ? m_pollIntervalMs : m_retryIntervalMs;
        if (m_pollTimer && interval == m_pollTimerInterval)
            return;
        m_pollTimerInterval = interval;
        m_pollTimer = m_loop->timerEvery(std::chrono::milliseconds(interval), [this]() {
            beginPoll();
        });
    }

    void releaseLoopResources()
    {
        m_pollTimer.reset();
        m_pollTimerInterval = 0;
        m_shots.reset();
        if (m_session)
            m_session->cancel();
        m_session.reset();
        m_loop = nullptr;
    }

    // --- the poll ---------------------------------------------------------

    void beginPoll()
    {
        if (m_lifecycle != Lifecycle::Running || !m_session)
            return;
        if (m_pollRunning || m_session->busy())
            return;
        if (!m_session->addressable()) {
            logPollError("Host/IP is required");
            noteFailure();
            return;
        }

        const bool slow = ++m_pollsSinceSlow >= kSlowPollEvery;
        if (slow)
            m_pollsSinceSlow = 0;

        m_steps.clear();
        m_stepIndex = 0;
        m_pollRunning = true;
        m_pollAnswered = false;
        m_snapshot = RouterSnapshot{};

        // Reachability, uptime, and - once a minute - the firmware version.
        m_steps.push_back([this]() { stepDeviceInfo(); });

        // One small question per tracked device, instead of the router's whole
        // host table. On the box in the field that is three answers of about
        // 500 bytes against 110 separate SOAP calls, twelve times a minute.
        for (const std::string &mac : m_trackedMacs)
            m_steps.push_back([this, mac]() { stepHost(mac); });

        if (m_capabilities.worthTrying(Feature::ByteCounters)) {
            m_steps.push_back([this]() { stepBytes("GetTotalBytesSent", "NewTotalBytesSent",
                                                   m_txMeter, kChannelTxRate); });
            m_steps.push_back([this]() { stepBytes("GetTotalBytesReceived",
                                                   "NewTotalBytesReceived", m_rxMeter,
                                                   kChannelRxRate); });
        }

        if (slow) {
            m_steps.push_back([this]() { stepWlan(kWlan24, Feature::Wlan24, kChannelWlan24); });
            m_steps.push_back([this]() { stepWlan(kWlan5, Feature::Wlan5, kChannelWlan5); });
            if (m_capabilities.worthTrying(Feature::UserInterfaceInfo)
                || m_capabilities.worthTrying(Feature::AutoUpdateInfo))
                m_steps.push_back([this]() { stepUpdateState(); });
        }

        runStep();
    }

    void runStep()
    {
        if (m_lifecycle != Lifecycle::Running || !m_session) {
            m_pollRunning = false;
            return;
        }
        if (m_stepIndex >= m_steps.size()) {
            finishPoll();
            return;
        }
        const auto step = m_steps[m_stepIndex++];
        step();
    }

    /// A step that could not even be issued must not leave the poll hanging.
    void stepIssued(bool issued)
    {
        if (!issued)
            m_shots->runOnce(0ms, [this]() { runStep(); });
    }

    void finishPoll()
    {
        m_pollRunning = false;
        if (m_pollAnswered) {
            m_pollFailures = 0;
            setConnected(true);
            // The descriptor first, then the values it describes. A poll is
            // where this instance learns what the router implements, so the
            // channel a value belongs to may not exist - or may still have the
            // wrong type - until this call has been made. phi-core rejects a
            // value for a channel it does not know, and applies the wrong
            // semantics to one whose type it has not seen yet; neither reaches
            // the adapter, because the send itself succeeded.
            announceRouter();
        } else {
            noteFailure();
        }
        flushPendingReports();
        armPollTimer();
    }

    void flushPendingReports()
    {
        std::vector<std::function<void()>> pending;
        pending.swap(m_pendingReports);
        for (const auto &send : pending)
            send();
    }

    void noteFailure()
    {
        m_pollRunning = false;
        if (m_pollFailures < kFailuresBeforeDisconnected)
            ++m_pollFailures;
        if (m_pollFailures >= kFailuresBeforeDisconnected)
            setConnected(false);
        armPollTimer();
    }

    void stepDeviceInfo()
    {
        stepIssued(m_session->call(kDeviceInfo, "GetInfo", {}, [this](Tr064Session::Reply reply) {
            if (reply.ok) {
                m_pollAnswered = true;
                std::string value;
                if (soapValue(reply.payload, "NewUpTime", &value)) {
                    bool ok = false;
                    const long long uptime = str::toLongLong(value, &ok);
                    if (ok)
                        report(kRouterDeviceId, kChannelUptime,
                               static_cast<std::int64_t>(uptime));
                }
                if (soapValue(reply.payload, "NewSoftwareVersion", &value))
                    m_routerFirmware = str::trimmed(value);
                if (soapValue(reply.payload, "NewModelName", &value) && !str::trimmed(value).empty())
                    m_routerName = str::trimmed(value);
                else if (soapValue(reply.payload, "NewDescription", &value))
                    m_routerName = str::trimmed(value);
            } else {
                logPollError(reply.error.empty() ? "DeviceInfo unavailable" : reply.error);
            }
            runStep();
        }));
    }

    void stepHost(const std::string &mac)
    {
        stepIssued(m_session->call(kHosts, "GetSpecificHostEntry", {{"NewMACAddress", mac}},
                                   [this, mac](Tr064Session::Reply reply) {
                                       if (reply.ok) {
                                           m_pollAnswered = true;
                                           HostEntry entry;
                                           if (parseHostEntry(reply.payload, mac, &entry))
                                               publishHost(entry);
                                       } else if (!reply.invalidAction) {
                                           // The router knows the action; this
                                           // address is simply not in its table
                                           // any more. The device stays, showing
                                           // offline - removing it would take
                                           // its room and its history with it.
                                           report(mac, kChannelOnline,
                                                  static_cast<std::int64_t>(
                                                      v1::ConnectivityStatus::Disconnected));
                                       }
                                       runStep();
                                   }));
    }

    void stepBytes(const char *action, const char *field, RateMeter &meter, const char *channel)
    {
        const std::string channelId(channel);
        stepIssued(m_session->call(kWanCommon, action, {},
                                   [this, field, &meter, channelId](Tr064Session::Reply reply) {
                                       if (reply.ok) {
                                           m_pollAnswered = true;
                                           m_capabilities.markPresent(Feature::ByteCounters);
                                           std::string value;
                                           if (soapValue(reply.payload, field, &value)) {
                                               bool ok = false;
                                               const long long total =
                                                   str::toLongLong(value, &ok);
                                               if (ok && total >= 0) {
                                                   const auto rate = meter.sample(
                                                       static_cast<std::uint64_t>(total), nowMs());
                                                   if (rate)
                                                       report(kRouterDeviceId, channelId, *rate);
                                               }
                                           }
                                       } else if (reply.invalidAction) {
                                           m_capabilities.markAbsent(Feature::ByteCounters);
                                       }
                                       runStep();
                                   }));
    }

    void stepWlan(const Service &service, Feature feature, const char *channel)
    {
        const std::string channelId(channel);
        stepIssued(m_session->call(service, "GetInfo", {},
                                   [this, feature, channelId](Tr064Session::Reply reply) {
                                       if (reply.ok) {
                                           m_pollAnswered = true;
                                           m_capabilities.markPresent(feature);
                                           std::string value;
                                           if (soapValue(reply.payload, "NewEnable", &value))
                                               report(kRouterDeviceId, channelId,
                                                      isTruthy(value));
                                       } else if (reply.invalidAction) {
                                           m_capabilities.markAbsent(feature);
                                       }
                                       runStep();
                                   }));
    }

    /**
     * @brief Whether a newer firmware is waiting.
     *
     * `UserInterface:1 GetInfo` is where FRITZ!OS keeps it, and it is asked
     * first. `X_AVM-DE_GetAutoUpdateInfo` - the only place the adapter used to
     * look - answers Invalid Action on the box in the field, which is why this
     * channel had never carried anything but the "Unknown" placeholder.
     */
    void stepUpdateState()
    {
        if (m_capabilities.worthTrying(Feature::UserInterfaceInfo)) {
            stepIssued(m_session->call(kUserInterface, "GetInfo", {},
                                       [this](Tr064Session::Reply reply) {
                                           if (reply.ok) {
                                               m_pollAnswered = true;
                                               m_capabilities.markPresent(
                                                   Feature::UserInterfaceInfo);
                                               applyUpdateState(reply.payload, "NewUpgradeAvailable");
                                               runStep();
                                               return;
                                           }
                                           if (reply.invalidAction) {
                                               m_capabilities.markAbsent(Feature::UserInterfaceInfo);
                                               // Try the older place before
                                               // giving up on the channel.
                                               stepLegacyUpdateInfo();
                                               return;
                                           }
                                           runStep();
                                       }));
            return;
        }
        stepLegacyUpdateInfo();
    }

    void stepLegacyUpdateInfo()
    {
        if (!m_capabilities.worthTrying(Feature::AutoUpdateInfo)) {
            runStep();
            return;
        }
        stepIssued(m_session->call(kDeviceInfo, "X_AVM-DE_GetAutoUpdateInfo", {},
                                   [this](Tr064Session::Reply reply) {
                                       if (reply.ok) {
                                           m_pollAnswered = true;
                                           m_capabilities.markPresent(Feature::AutoUpdateInfo);
                                           applyUpdateState(reply.payload, "NewUpdateAvailable");
                                       } else if (reply.invalidAction) {
                                           // Neither place has it. Asked once
                                           // each, and then never again.
                                           m_capabilities.markAbsent(Feature::AutoUpdateInfo);
                                       }
                                       runStep();
                                   }));
    }

    void applyUpdateState(const std::string &payload, const char *availableField)
    {
        std::string value;
        if (!soapValue(payload, availableField, &value))
            return;
        std::string status = isTruthy(value) ? "UpdateAvailable" : "UpToDate";

        // The router also says what it is doing about it. Only a failure is
        // worth overriding "up to date" with; a download in progress is still
        // an update that is available.
        std::string state;
        if (soapValue(payload, "NewX_AVM-DE_UpdateState", &state)
            && str::containsIgnoreCase(state, "error")) {
            status = "UpdateFailed";
        }

        // The status is what a history row and an automation condition see -
        // it is the field the kind names as this channel's projection. The two
        // versions ride along for a person to read.
        v1::ChannelValueFields fields;
        fields.emplace_back("status", status);
        std::string current;
        if (soapValue(payload, "NewX_AVM-DE_CurrentFwVersion", &current)
            || !m_routerFirmware.empty()) {
            const std::string version = current.empty() ? m_routerFirmware : current;
            if (!version.empty())
                fields.emplace_back("currentVersion", version);
        }
        std::string target;
        if (soapValue(payload, "NewX_AVM-DE_Version", &target) && !str::trimmed(target).empty())
            fields.emplace_back("targetVersion", str::trimmed(target));

        reportObject(kRouterDeviceId, kChannelSoftwareUpdate, fields);
    }

    // --- publishing -------------------------------------------------------

    void announceRouter()
    {
        const bool hasWlan24 =
            m_capabilities.availability(Feature::Wlan24) == RouterCapabilities::Availability::Present;
        const bool hasWlan5 =
            m_capabilities.availability(Feature::Wlan5) == RouterCapabilities::Availability::Present;
        const bool hasRates =
            m_capabilities.availability(Feature::ByteCounters)
            == RouterCapabilities::Availability::Present;
        const bool hasUpdate =
            m_capabilities.availability(Feature::UserInterfaceInfo)
                == RouterCapabilities::Availability::Present
            || m_capabilities.availability(Feature::AutoUpdateInfo)
                == RouterCapabilities::Availability::Present;

        const std::string fingerprint = routerFingerprint(m_routerName, m_routerFirmware,
                                                          hasWlan24, hasWlan5, hasRates,
                                                          hasUpdate);
        if (!m_reported.descriptorIsNews(kRouterDeviceId, fingerprint))
            return;

        v1::Utf8String error;
        if (!sendDeviceUpdated(buildRouterDevice(m_routerName, m_routerFirmware),
                               buildRouterChannels(hasWlan24, hasWlan5, hasRates, hasUpdate),
                               &error)) {
            std::cerr << "failed to send deviceUpdated(router): " << error << '\n';
            return;
        }
        m_routerAnnounced = true;
        // The descriptor changed, so this may be the first time phi-core knows
        // about one of these channels - and a value sent before it did was
        // dropped as an unknown channel id, silently, because the send
        // succeeded. Say everything again on the next poll.
        m_reported.forgetValues(kRouterDeviceId);
    }

    void publishHost(const HostEntry &host)
    {
        if (host.mac.empty())
            return;
        if (m_reported.descriptorIsNews(host.mac, hostFingerprint(host))) {
            v1::Utf8String error;
            if (!sendDeviceUpdated(buildHostDevice(host), buildHostChannels(host), &error)) {
                std::cerr << "failed to send deviceUpdated(host): " << error << '\n';
            } else {
                // A host that just grew an RSSI channel is the same case as the
                // router growing one: what was recorded as reported was never
                // accepted.
                m_reported.forgetValues(host.mac);
            }
        }
        report(host.mac, kChannelOnline,
               static_cast<std::int64_t>(host.active ? v1::ConnectivityStatus::Connected
                                                     : v1::ConnectivityStatus::Disconnected));
        if (host.hasSignal)
            report(host.mac, kChannelRssi, static_cast<double>(host.signalDbm));
    }

    /**
     * @brief The same "only if it is news" rule for a composite value.
     *
     * Deduplicated on the whole object, not on the projection: a version that
     * moved while the status stayed "UpToDate" is a change worth sending.
     */
    void reportObject(const std::string &deviceId, const std::string &channelId,
                      const v1::ChannelValueFields &fields)
    {
        std::string fingerprint;
        for (const auto &[name, value] : fields) {
            fingerprint += name;
            fingerprint += '=';
            fingerprint += scalarToText(value);
            fingerprint += ';';
        }
        if (!m_reported.isNews(deviceId, channelId, fingerprint))
            return;
        defer([this, deviceId, channelId, fields]() {
            v1::Utf8String error;
            if (!sendChannelObjectStateUpdated(deviceId, channelId, fields, nowMs(), &error))
                std::cerr << "failed to send channelStateUpdated(" << channelId
                          << "): " << error << '\n';
        });
    }

    static std::string scalarToText(const v1::ScalarValue &value)
    {
        if (const auto *v = std::get_if<v1::Utf8String>(&value))
            return *v;
        if (const auto *v = std::get_if<std::int64_t>(&value))
            return str::number(*v);
        if (const auto *v = std::get_if<double>(&value))
            return str::number(*v);
        if (const auto *v = std::get_if<bool>(&value))
            return *v ? "1" : "0";
        return {};
    }

    void report(const std::string &deviceId, const std::string &channelId,
                const v1::ScalarValue &value)
    {
        if (!m_reported.isNews(deviceId, channelId, value))
            return;
        defer([this, deviceId, channelId, value]() {
            v1::Utf8String error;
            if (!sendChannelStateUpdated(deviceId, channelId, value, nowMs(), &error))
                std::cerr << "failed to send channelStateUpdated(" << channelId
                          << "): " << error << '\n';
        });
    }

    /// Runs now, or after this poll's descriptor if one is in flight.
    void defer(std::function<void()> send)
    {
        if (!m_pollRunning) {
            send();
            return;
        }
        m_pendingReports.push_back(std::move(send));
    }

    void setConnected(bool connected)
    {
        if (m_connected == connected)
            return;
        m_connected = connected;
        armPollTimer();
        v1::Utf8String error;
        if (!sendConnectionStateChanged(m_connected, &error))
            std::cerr << "failed to send connectionStateChanged: " << error << '\n';
    }

    void logPollError(const std::string &error)
    {
        const std::int64_t now = nowMs();
        if (error == m_lastPollError && (now - m_lastPollErrorMs) < m_retryIntervalMs)
            return;
        m_lastPollError = error;
        m_lastPollErrorMs = now;
        std::cerr << "fritz-ipc poll failed: " << error << '\n';
    }

    // --- the actions ------------------------------------------------------

    void handleSettings(const sdk::AdapterActionInvokeRequest &request)
    {
        const Json params = parseObject(request.paramsJson);
        Json patch = Json::object();
        for (const auto &entry : params.items()) {
            if (entry.key() == "trackedMacs") {
                Json macs = Json::array();
                for (const std::string &mac : macList(entry.value()))
                    macs.push_back(mac);
                patch["trackedMacs"] = macs;
                continue;
            }
            patch[entry.key()] = entry.value();
        }

        if (!patch.empty()) {
            for (const auto &entry : patch.items())
                m_meta[entry.key()] = entry.value();
            m_info.metaJson = dump(m_meta);
            applyConfig();
            v1::Utf8String error;
            if (!sendAdapterMetaUpdated(dump(patch), &error))
                std::cerr << "failed to send adapterMetaUpdated(settings): " << error << '\n';
        }

        answerAction(request.cmdId, v1::CmdStatus::Success, {}, formValues(), fieldChoices());

        if (m_lifecycle == Lifecycle::Running) {
            armPollTimer();
            m_pollsSinceSlow = kSlowPollEvery;
            beginPoll();
        }
    }

    /**
     * @brief The host picker: the router's whole table, once, on request.
     *
     * The only place the full list is needed, and the reason the poll no longer
     * fetches it. `X_AVM-DE_GetHostListPath` is asked for first because it is
     * the one this firmware has; the standard name comes back Invalid Action,
     * and the fallback below - one call per host - is what the poll used to do
     * every five seconds.
     */
    void handleBrowseHosts(const sdk::AdapterActionInvokeRequest &request)
    {
        if (m_lifecycle != Lifecycle::Running || !m_session || !m_session->addressable()) {
            answerAction(request.cmdId, v1::CmdStatus::TemporarilyOffline, "Host/IP is required");
            return;
        }
        m_session->cancel();
        m_pollRunning = false;

        const v1::CmdId cmdId = request.cmdId;
        const char *action = m_capabilities.worthTrying(Feature::AvmHostListPath)
            ? "X_AVM-DE_GetHostListPath"
            : "GetHostListPath";
        const Feature feature = m_capabilities.worthTrying(Feature::AvmHostListPath)
            ? Feature::AvmHostListPath
            : Feature::StandardHostListPath;

        m_session->call(kHosts, action, {}, [this, cmdId, feature](Tr064Session::Reply reply) {
            std::string path;
            if (reply.ok && (soapValue(reply.payload, "NewX_AVM-DE_HostListPath", &path)
                             || soapValue(reply.payload, "NewHostListPath", &path))
                && !path.empty()) {
                m_capabilities.markPresent(feature);
                fetchHostList(cmdId, path);
                return;
            }
            if (reply.invalidAction)
                m_capabilities.markAbsent(feature);
            enumerateHosts(cmdId);
        });
    }

    void fetchHostList(v1::CmdId cmdId, const std::string &path)
    {
        m_session->get(path, [this, cmdId](Tr064Session::Reply reply) {
            if (!reply.ok) {
                enumerateHosts(cmdId);
                return;
            }
            std::vector<HostEntry> hosts = parseHostList(reply.payload);
            if (hosts.empty()) {
                enumerateHosts(cmdId);
                return;
            }
            completeBrowse(cmdId, hosts);
        });
    }

    /// One call per host. Slow, and only reached when the router has no list
    /// document to offer.
    void enumerateHosts(v1::CmdId cmdId)
    {
        m_session->call(kHosts, "GetHostNumberOfEntries", {},
                        [this, cmdId](Tr064Session::Reply reply) {
                            std::string value;
                            int total = 0;
                            if (reply.ok && soapValue(reply.payload, "NewHostNumberOfEntries",
                                                      &value)) {
                                bool ok = false;
                                total = str::toInt(value, &ok);
                                if (!ok || total < 0)
                                    total = 0;
                            }
                            auto hosts = std::make_shared<std::vector<HostEntry>>();
                            enumerateHostAt(cmdId, 0, total, hosts);
                        });
    }

    void enumerateHostAt(v1::CmdId cmdId, int index, int total,
                         std::shared_ptr<std::vector<HostEntry>> hosts)
    {
        if (index >= total || m_lifecycle != Lifecycle::Running || !m_session) {
            completeBrowse(cmdId, *hosts);
            return;
        }
        m_session->call(kHosts, "GetGenericHostEntry", {{"NewIndex", str::number(index)}},
                        [this, cmdId, index, total, hosts](Tr064Session::Reply reply) {
                            if (reply.ok) {
                                HostEntry entry;
                                if (parseHostEntry(reply.payload, {}, &entry)
                                    && !entry.mac.empty())
                                    hosts->push_back(std::move(entry));
                            }
                            enumerateHostAt(cmdId, index + 1, total, hosts);
                        });
    }

    void completeBrowse(v1::CmdId cmdId, const std::vector<HostEntry> &hosts)
    {
        std::map<std::string, Json> known;
        if (m_meta.contains("knownHosts") && m_meta.at("knownHosts").is_array()) {
            for (const Json &entry : m_meta.at("knownHosts")) {
                if (!entry.is_object())
                    continue;
                const std::string mac = normalizeMac(jsonString(entry, "mac"));
                if (!mac.empty())
                    known[mac] = entry;
            }
        }

        for (const HostEntry &host : hosts) {
            if (host.mac.empty())
                continue;
            Json merged = known.count(host.mac) ? known[host.mac] : Json::object();
            merged["mac"] = host.mac;
            if (!host.name.empty())
                merged["name"] = host.name;
            if (!host.ip.empty())
                merged["ip"] = host.ip;
            known[host.mac] = merged;
        }
        // A selection never silently vanishes because a device dropped off.
        for (const std::string &mac : m_trackedMacs) {
            if (known.count(mac))
                continue;
            Json entry = Json::object();
            entry["mac"] = mac;
            known[mac] = entry;
        }

        Json knownHosts = Json::array();
        for (const auto &[mac, entry] : known)
            knownHosts.push_back(entry);

        Json patch = Json::object();
        patch["knownHosts"] = knownHosts;
        Json tracked = Json::array();
        for (const std::string &mac : m_trackedMacs)
            tracked.push_back(mac);
        patch["trackedMacs"] = tracked;

        for (const auto &entry : patch.items())
            m_meta[entry.key()] = entry.value();
        m_info.metaJson = dump(m_meta);

        v1::Utf8String error;
        if (!sendAdapterMetaUpdated(dump(patch), &error))
            std::cerr << "failed to send adapterMetaUpdated(browseHosts): " << error << '\n';

        std::cerr << "fritz-ipc browseHosts hosts=" << hosts.size()
                  << " known=" << known.size() << " tracked=" << m_trackedMacs.size() << '\n';

        answerAction(cmdId, v1::CmdStatus::Success, {}, formValues(), fieldChoices());
    }

    v1::AdapterFormValues formValues() const
    {
        v1::ScalarList tracked;
        for (const std::string &mac : m_trackedMacs)
            tracked.emplace_back(mac);
        return {{"trackedMacs", tracked}};
    }

    v1::AdapterFieldChoicesList fieldChoices() const
    {
        return {{"trackedMacs", buildTrackedOptions(m_meta)}};
    }

    // --- answering --------------------------------------------------------

    static v1::CmdResponse responseFor(v1::CmdId cmdId, v1::CmdStatus status,
                                       const std::string &error)
    {
        v1::CmdResponse response;
        response.id = cmdId;
        response.tsMs = nowMs();
        response.status = status;
        response.error = error;
        return response;
    }

    void sendCommandResponse(v1::CmdResponse response)
    {
        v1::Utf8String error;
        if (!sendResult(response, &error))
            std::cerr << "fritz-ipc sendResult failed: " << error << '\n';
    }

    void answerCommand(v1::CmdId cmdId, v1::CmdStatus status, const std::string &error)
    {
        sendCommandResponse(responseFor(cmdId, status, error));
    }

    void answerAction(v1::CmdId cmdId, v1::CmdStatus status, const std::string &error,
                      v1::AdapterFormValues formValues = {},
                      v1::AdapterFieldChoicesList fieldChoices = {})
    {
        v1::ActionResponse response;
        response.id = cmdId;
        response.tsMs = nowMs();
        response.status = status;
        response.error = error;
        response.resultType = v1::ActionResultType::None;
        if (!formValues.empty()) {
            response.formValues = std::move(formValues);
            response.fieldChoices = std::move(fieldChoices);
            response.reloadLayout = true;
        }
        v1::Utf8String sendError;
        if (!sendResult(response, &sendError))
            std::cerr << "fritz-ipc sendResult failed: " << sendError << '\n';
    }

    /// Nothing is queued here - the session runs one call at a time and a user
    /// action cancels a poll rather than waiting behind it - so there is no
    /// backlog to answer. The hook stays for symmetry with the lifecycle.
    void answerPending(const std::string &reason) { (void)reason; }

    static std::optional<bool> scalarToBool(const v1::ScalarValue &value)
    {
        if (const auto *v = std::get_if<bool>(&value))
            return *v;
        if (const auto *v = std::get_if<std::int64_t>(&value))
            return *v != 0;
        if (const auto *v = std::get_if<double>(&value))
            return *v != 0.0;
        if (const auto *v = std::get_if<v1::Utf8String>(&value)) {
            const std::string text = str::toLower(str::trimmed(*v));
            if (text == "true" || text == "1" || text == "on")
                return true;
            if (text == "false" || text == "0" || text == "off")
                return false;
        }
        return std::nullopt;
    }

    // --- members ----------------------------------------------------------

    phi::runtime::Loop *m_loop = nullptr;
    std::optional<Tr064Session> m_session;
    std::optional<phi::runtime::OneShots> m_shots;
    phi::runtime::Timer m_pollTimer;
    int m_pollTimerInterval = 0;

    v1::Adapter m_info;
    Json m_meta = Json::object();
    std::vector<std::string> m_trackedMacs;
    std::uint16_t m_port = kDefaultTr064Port;
    int m_pollIntervalMs = 5000;
    int m_retryIntervalMs = 10000;

    RouterCapabilities m_capabilities;
    ReportedValues m_reported;
    RateMeter m_txMeter;
    RateMeter m_rxMeter;
    RouterSnapshot m_snapshot;

    std::vector<std::function<void()>> m_steps;
    /// Channel values waiting for the descriptor of the poll that produced them.
    std::vector<std::function<void()>> m_pendingReports;
    std::size_t m_stepIndex = 0;
    bool m_pollRunning = false;
    bool m_pollAnswered = false;
    int m_pollsSinceSlow = 0;
    int m_pollFailures = 0;

    Lifecycle m_lifecycle = Lifecycle::Idle;
    bool m_connected = false;
    bool m_routerAnnounced = false;
    std::string m_routerName;
    std::string m_routerFirmware;
    std::string m_lastPollError;
    std::int64_t m_lastPollErrorMs = 0;
};

} // namespace

std::unique_ptr<sdk::AdapterInstance> makeInstance()
{
    return std::make_unique<FritzInstance>();
}

} // namespace phicore::fritz::ipc
