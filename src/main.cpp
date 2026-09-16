// Process entry point for the FRITZ!Box sidecar: the adapter factory and the
// SDK's own main loop. The router runtime lives in fritz_instance, TR-064 in
// fritz_tr064 and fritz_soap.

#include <cstdlib>
#include <memory>
#include <optional>
#include <string>

#include "phi/adapter/sdk/loop_execution_backend.h"
#include "phi/adapter/sdk/sidecar.h"
#include "phi/runtime/loop.h"

#include "fritz_instance.h"
#include "fritz_json.h"
#include "fritz_probe.h"
#include "fritz_schema.h"
#include "fritz_session.h"

namespace v1 = phicore::adapter::v1;
namespace sdk = phicore::adapter::sdk;

using namespace phicore::fritz::ipc;

namespace {

class FritzIpcFactory final : public sdk::AdapterFactory
{
protected:
    // The probe opens a connection and waits for a router, so it does not run
    // on the host poll thread. It is a loop backend rather than a plain one
    // because the HTTP client needs somewhere to watch a descriptor.
    std::unique_ptr<sdk::InstanceExecutionBackend> createFactoryExecutionBackend() override
    {
        return sdk::createLoopExecutionBackend("fritz-factory");
    }

    std::unique_ptr<sdk::InstanceExecutionBackend> createInstanceExecutionBackend(
        const sdk::ExternalId &externalId) override
    {
        (void)externalId;
        return sdk::createLoopExecutionBackend("fritz-instance");
    }

    v1::Utf8String pluginType() const override { return kPluginType; }
    v1::Utf8String displayName() const override { return phicore::fritz::ipc::displayName(); }
    v1::Utf8String description() const override { return phicore::fritz::ipc::description(); }
    v1::Utf8String apiVersion() const override { return v1::kProtocolLabel; }
    v1::Utf8String iconSvg() const override { return phicore::fritz::ipc::iconSvg(); }
    int timeoutMs() const override { return 15000; }
    int maxInstances() const override { return 0; }

    v1::AdapterCapabilities capabilities() const override
    {
        return phicore::fritz::ipc::capabilities();
    }

    std::optional<v1::AdapterConfigSchema> configSchema() const override
    {
        return phicore::fritz::ipc::configSchema();
    }

    std::unique_ptr<sdk::AdapterInstance> createInstance(const sdk::ExternalId &externalId) override
    {
        (void)externalId;
        return makeInstance();
    }

    /**
     * @brief "Test connection", at the scope it is declared for.
     *
     * `capabilities()` has always put `probe` in `factoryActions` while the
     * only handler lived in the instance and this hook was never overridden -
     * so the SDK answered "Factory action handler not implemented" to the one
     * button whose whole purpose is to be pressed before an instance exists.
     */
    void onFactoryActionInvoke(const sdk::AdapterActionInvokeRequest &request) override
    {
        if (request.actionId != "probe") {
            answer(request.cmdId, v1::CmdStatus::NotSupported, "Factory action not supported");
            return;
        }

        const ProbeTarget target = probeTargetFromParams(parseObject(request.paramsJson));
        if (target.host.empty()) {
            answer(request.cmdId, v1::CmdStatus::InvalidArgument, "Probe requires host or ip");
            return;
        }

        // Created here rather than in the constructor: the constructor runs on
        // the main thread and this object belongs to the factory backend's
        // loop.
        if (!m_session) {
            phi::runtime::Loop *loop = phi::runtime::Loop::current();
            if (loop == nullptr) {
                answer(request.cmdId, v1::CmdStatus::Failure, "No loop on the factory thread");
                return;
            }
            m_session.emplace(*loop);
        }

        log(sdk::LogLevel::Debug, sdk::LogCategory::Discovery, "probing %1 (user set: %2)",
            {v1::Utf8String(target.endpoint()), !target.user.empty()});

        const v1::CmdId cmdId = request.cmdId;
        const std::string endpoint = target.endpoint();
        runProbe(*m_session, target, [this, cmdId, endpoint](ProbeOutcome outcome) {
            if (!outcome.ok) {
                answer(cmdId, v1::CmdStatus::Failure, outcome.error, "factory.action");
                return;
            }
            v1::ActionResponse response;
            response.id = cmdId;
            response.tsMs = 0;
            response.status = v1::CmdStatus::Success;
            response.resultType = v1::ActionResultType::String;
            response.resultValue = endpoint;
            send(std::move(response));
        });
    }

    /// The session belongs to the factory backend's loop, and this is the last
    /// callback that still runs on it.
    void onFactoryStopping() override { m_session.reset(); }

private:
    void answer(v1::CmdId cmdId, v1::CmdStatus status, const std::string &error,
                const std::string &context = {})
    {
        v1::ActionResponse response;
        response.id = cmdId;
        response.status = status;
        response.error = error;
        response.errorContext = context;
        response.resultType = v1::ActionResultType::None;
        send(std::move(response));
    }

    void send(v1::ActionResponse response)
    {
        v1::Utf8String error;
        if (!sendResult(response, &error))
            log(sdk::LogLevel::Error, sdk::LogCategory::Internal,
                "failed to send the factory.action.invoke result: %1", {error});
    }

    std::optional<Tr064Session> m_session;
};

} // namespace

int main(int argc, char **argv)
{
    const char *envSocketPath = std::getenv("PHI_ADAPTER_SOCKET_PATH");
    const v1::Utf8String socketPath = (argc > 1)
        ? argv[1]
        : (envSocketPath ? envSocketPath : v1::Utf8String("/tmp/phi-adapter-fritz-ipc.sock"));


    FritzIpcFactory factory;
    sdk::SidecarHost host(socketPath, factory);
    return sdk::runSidecarMain(host);
}
