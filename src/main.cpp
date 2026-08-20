// Process entry point for the FRITZ!Box sidecar: the adapter factory and the Qt
// main loop that drives the SDK host. The router runtime lives in
// fritz_sidecar, the TR-064 wire format in fritz_tr064.

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>

#include <QCoreApplication>
#include <QTimer>

#include "phi/adapter/sdk/qt/instance_execution_backend_qt.h"
#include "phi/adapter/sdk/qt/sidecar_driver_qt.h"
#include "phi/adapter/sdk/sidecar.h"

#include "fritz_schema.h"
#include "fritz_sidecar.h"

namespace v1 = phicore::adapter::v1;
namespace sdk = phicore::adapter::sdk;

using namespace phicore::fritz::ipc;

namespace {

std::atomic_bool g_running{true};

void handleSignal(int)
{
    g_running.store(false);
}

class FritzIpcFactory final : public sdk::AdapterFactory
{
public:
    // TR-064 polling blocks in nested event loops; on the SDK's default
    // (plain-thread) backend those instance callbacks had no Qt event loop of
    // their own. A Qt backend gives every instance its own event loop, which is
    // also what QNetworkAccessManager and the poll timer need.
    std::unique_ptr<sdk::InstanceExecutionBackend> createInstanceExecutionBackend(
        const sdk::ExternalId &externalId) override
    {
        (void)externalId;
        return sdk::qt::createInstanceExecutionBackend();
    }

    v1::Utf8String pluginType() const override
    {
        return kPluginType;
    }

    v1::Utf8String displayName() const override
    {
        return phicore::fritz::ipc::displayName();
    }

    v1::Utf8String description() const override
    {
        return phicore::fritz::ipc::description();
    }

    v1::Utf8String apiVersion() const override
    {
        return v1::kProtocolLabel;
    }

    v1::Utf8String iconSvg() const override
    {
        return phicore::fritz::ipc::iconSvg();
    }

    int timeoutMs() const override
    {
        return 15000;
    }

    int maxInstances() const override
    {
        return 0;
    }

    v1::AdapterCapabilities capabilities() const override
    {
        return phicore::fritz::ipc::capabilities();
    }

    v1::JsonText configSchemaJson() const override
    {
        return phicore::fritz::ipc::configSchemaJson();
    }

    std::unique_ptr<sdk::AdapterInstance> createInstance(const sdk::ExternalId &externalId) override
    {
        (void)externalId;
        return makeInstance();
    }
};

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    const char *envSocketPath = std::getenv("PHI_ADAPTER_SOCKET_PATH");
    const v1::Utf8String socketPath = (argc > 1)
        ? argv[1]
        : (envSocketPath ? envSocketPath : v1::Utf8String("/tmp/phi-adapter-fritz-ipc.sock"));

    std::cerr << "starting " << (argc > 0 && argv && argv[0] ? argv[0] : "phi_adapter_fritz")
              << " for pluginType=" << kPluginType
              << " socket=" << socketPath << '\n';

    FritzIpcFactory factory;
    sdk::SidecarHost host(socketPath, factory);

    // The driver watches the host's poll descriptor from the Qt event loop:
    // no polling interval, no idle wakeups, and the Qt event loop is no longer
    // starved by a blocking poll (HTTP requests and timers run on time).
    sdk::qt::SidecarDriver driver(host);

    v1::Utf8String error;
    if (!driver.start(&error)) {
        std::cerr << "failed to start sidecar host: " << error << '\n';
        return 1;
    }

    // Signal handlers only flip a flag; a slow timer turns it into a clean
    // Qt shutdown.
    QTimer shutdownTimer;
    QObject::connect(&shutdownTimer, &QTimer::timeout, [&]() {
        if (!g_running.load(std::memory_order_relaxed))
            app.quit();
    });
    shutdownTimer.start(250);

    const int execResult = app.exec();
    driver.stop();
    std::cerr << "stopping phi_adapter_fritz_ipc" << '\n';
    return execResult;
}
