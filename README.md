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
- Network access to the router's TR-064 port (49000 by default)

### Build Requirements

- `cmake`, `ninja-build`
- `phi-adapter-sdk` >= 0.10.0 (local checkout in `../phi-adapter-sdk` or installed package)
- `nlohmann-json3-dev`
- No Qt. The loop, the timers and the strings come from `phi-runtime` through
  the SDK; HTTP with digest authentication from the SDK's own client; JSON is
  nlohmann, which is what phi-core uses.

### Layout

| File | What it is about |
| --- | --- |
| `fritz_soap` | the SOAP envelope that goes out and reading the one that comes back |
| `fritz_tr064` | which services and actions exist, and what their values mean |
| `fritz_session` | one TR-064 call at a time, on the loop |
| `fritz_capabilities` | what this particular router turns out to implement |
| `fritz_state` | what has already been said, and a rate from two counter samples |
| `fritz_devicemodel` | the device and channel descriptors phi-core is given |
| `fritz_probe` | "Test connection", shared by the factory and the instance |
| `fritz_schema` | adapter identity and the config schema phi-ui renders |
| `fritz_instance` | the lifecycle that ties them together |

### What this router implements

Action names differ by model line and firmware, and a router says so plainly: a
SOAP fault with error 401, `Invalid Action`. Measured against a FRITZ!Box
6850 5G on firmware 258.08.25:

| Action | |
| --- | --- |
| `DeviceInfo GetInfo` | works |
| `Hosts GetSpecificHostEntry` | works |
| `Hosts GetHostNumberOfEntries` | works |
| `Hosts X_AVM-DE_GetHostListPath` | works |
| `Hosts GetHostListPath` | **Invalid Action** |
| `WLANConfiguration:1/2 GetInfo` | works |
| `WANCommonInterfaceConfig GetTotalBytesSent/Received` | works, on `/upnp/control/wancommonifconfig1` |
| `WANCommonInterfaceConfig GetAddonInfos` | **Invalid Action**, on either path |
| `DeviceInfo X_AVM-DE_GetAutoUpdateInfo` | **Invalid Action** |

An action that comes back Invalid Action is recorded as absent and not issued
again, and the channel it would have filled is not advertised. A channel that
cannot carry a value is worse than a missing one: `tx_rate` and `rx_rate` stood
in the interface from the day the box was set up and phi-core's database still
reads "never a value" for both.

### Runtime State Machine

One execution thread per instance, one event loop on it, one TR-064 call in
flight at a time. Nothing blocks the loop, so no callback runs inside another.

- `Idle` — started, but no configuration yet, so no endpoint to poll.
- `Running + Disconnected` — the poll timer uses `retryIntervalMs`. Three polls
  in a row that nothing answered turn connectivity off.
- `Running + Connected` — the poll timer uses `pollIntervalMs`.
- `Paused` — phi-core disconnected. It comes back with the configuration, which
  starts the instance again.
- `Stopped` — terminal, and where everything belonging to the loop is released:
  the destructor runs on the host thread, where the runtime aborts on a
  wrong-thread handle.

### The poll

Split by how fast things actually change:

- **Every `pollIntervalMs`** (5 s by default): `DeviceInfo GetInfo` for the
  uptime and for whether the router is there at all; one
  `GetSpecificHostEntry` per tracked address; the two byte counters, from which
  the current rate is derived.
- **Every twelfth poll** (a minute by default): the WLAN switches, the firmware
  version, the update state - things that change by the month.

The router's full host table is fetched only by `browseHosts`, which is what
fills the picker, and only when somebody presses it. The poll used to fetch it
every five seconds and throw away everything that was not tracked; on the box
in the field the list document was unreachable under the wrong action name, so
what actually happened was `GetHostNumberOfEntries` followed by 110 separate
`GetGenericHostEntry` calls - about 1300 SOAP requests a minute, measured at
31 MB an hour, to keep three devices up to date.

A user action - a WLAN switch, a settings save, a host browse - cancels a
running poll rather than queueing behind it. The next poll is five seconds away.

### Configuration

- Factory scope: `host`, `tr064Port`, `user`, `password`, `pollIntervalMs`,
  `retryIntervalMs`
- Instance scope: `trackedMacs`, filled by the `browseHosts` action
- `knownHosts` is the picker's memory, written by `browseHosts`

### Known Rough Edges

- `https` is not supported: the SDK's HTTP client does not do TLS yet. The
  adapter in the field does not use it (its `UseTls` flag is clear), and an
  endpoint that asks for it fails with a plain message rather than silently.
- The router serves a third WLAN (`wlanconfig3`) that this adapter does not
  offer.
- A configured hostname is resolved with `getaddrinfo` on the loop thread until
  phi-core hands down an address.

### Troubleshooting

- Symptom: "Invalid credentials" — the router was reached and refused. TR-064
  needs a FRITZ!Box user with the "smart home and other permissions" right.
- Symptom: nothing is tracked — no addresses are selected. Open Settings and
  press "Probe WLAN", which fetches the host table and fills the picker.

### Maintainers

- Phi Systems Tech team
