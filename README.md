# phi-adapter-fritz

## Overview

Integrates a local AVM FRITZ!Box with phi-core.

## Supported Devices / Systems

- AVM FRITZ!Box devices exposing the expected local interface

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

Provides a FRITZ!Box adapter plugin for phi-core.

### Features

- Local endpoint connectivity
- Authenticated requests
- Logging category `phi-core.adapters.fritz`

### Runtime Requirements

- phi-core with plugin loading enabled
- Network access from phi-core host to FRITZ!Box

### Build Requirements

- `cmake`
- Qt6 modules: `Core`, `Network`
- `phi-adapter-api` (local checkout or installed package)

### Configuration

- No dedicated config file in this repository
- Runtime settings are provided through phi-core adapter configuration

### Build

```bash
cmake -S . -B build
cmake --build build --parallel
```

### Installation

- Build output: `build/plugins/adapters/libphi_adapter_fritz.so`
- Deploy to: `/opt/phi/plugins/adapters/`

### Troubleshooting

- Error: `401 Unauthorized`
- Cause: invalid credentials or missing permissions
- Fix: verify credentials and FRITZ!Box account rights

### Maintainers

- Phi Systems Tech team

### Issue Tracker

- https://github.com/phi-systems-tech/phi-adapter-fritz/issues

### Releases / Changelog

- https://github.com/phi-systems-tech/phi-adapter-fritz/releases
- https://github.com/phi-systems-tech/phi-adapter-fritz/tags
