# phi-adapter-fritz

## Overview

Integrates local AVM FRITZ!Box systems with phi-core.

## Supported Devices / Systems

- AVM FRITZ!Box devices exposing TR-064 endpoints

## Cloud Functionality

- Cloud required: `no`
- Local network integration only

## Known Issues

- Authentication and endpoint behavior can vary across FRITZ!OS versions.

## License

See `LICENSE`.

---

## Developer Documentation

### Purpose

Provides local network TR-064 integration for FRITZ!Box via IPC sidecar.

### Features

- IPC sidecar executable using `phi-adapter-sdk`
- Descriptor-driven config schema (`configSchema`) sent during bootstrap
- Factory action `probe` (`Test connection`)
- Instance actions `settings` and `browseHosts`
- Router channels for uptime, update state, WLAN toggles, TX/RX rates
- Tracked client devices with connectivity + RSSI updates

### Runtime Requirements

- phi-core with IPC adapter runtime enabled
- Network access to FRITZ!Box endpoint

### Build Requirements

- `cmake`
- Qt6 modules: `Core`, `Network`
- `phi-adapter-sdk` (local checkout in `../phi-adapter-sdk` or installed package)

### Configuration

- No dedicated config file in this repository
- Adapter settings are configured through phi-core
- Factory scope fields:
  - `host`
  - `port` (default `49000`)
  - `user`
  - `password`
  - `pollIntervalMs`
  - `retryIntervalMs`
- Instance scope fields:
  - `trackedMacs` (select list, populated via `browseHosts`)

### Build

```bash
cmake -S . -B build
cmake --build build --parallel
```

### Installation

- Build output: `build/plugins/adapters/phi_adapter_fritz_ipc`
- Deploy to: `/opt/phi/plugins/adapters/`

### Troubleshooting

- Error: `Invalid credentials`
- Cause: invalid username/password or missing TR-064 rights
- Fix: verify FRITZ!Box user permissions and credentials

### Maintainers

- Phi Systems Tech team

### Issue Tracker

- https://github.com/phi-systems-tech/phi-adapter-fritz/issues

### Releases / Changelog

- https://github.com/phi-systems-tech/phi-adapter-fritz/releases
- https://github.com/phi-systems-tech/phi-adapter-fritz/tags
