<div align="center" markdown="1">

# Meshtacore

**One radio, two meshes.**

A Meshtastic firmware fork that hears [MeshCore](https://github.com/meshcore-dev/MeshCore) on the same SX126x, repeats it, and bridges its groups and direct messages into the Meshtastic client.

![Status](https://img.shields.io/badge/status-experimental-orange)
![License](https://img.shields.io/badge/license-GPL--3.0-blue)
![Base](https://img.shields.io/badge/based%20on-Meshtastic%20firmware%202.8.1-lightgrey)

</div>

## Why Meshtacore

Meshtastic and MeshCore are two LoRa mesh ecosystems that normally live on separate radios. When both networks share the same frequency, bandwidth and spreading factor, the only thing separating them is the sync word. Meshtacore turns one SX126x into a node that listens to both:

- it repeats MeshCore traffic, extending a MeshCore mesh with hardware you already have deployed;
- it exposes MeshCore group channels and direct messages inside the Meshtastic client, so one phone can talk to both worlds;
- it keeps Meshtastic working as before, on the same radio and the same antenna.

No second radio, no second antenna, no second power budget.

Good fits: clubs and events that run both ecosystems, Meshtastic solar or repeater sites that can also cover a MeshCore mesh, and anyone testing MeshCore interoperability without dedicating a node to it.

## Features

### One radio, two networks

- **Hybrid sync word RX**: register `0x14B4` receives Meshtastic (`0x2B`) and MeshCore (`0x12`) on one SX126x. TX still uses each protocol's own sync word, so both meshes hear a native frame.
- **Meshtastic first**: a frame that parses as MeshCore is first trial-decrypted with our channels. Only if that fails is it handled as MeshCore, so Meshtastic traffic is never sacrificed for the bridge.
- **SX126x only**: the hybrid receive is a radio-register trick, not a protocol change.

### MeshCore repeater

- Flood relay with path-independent dedupe (last 160 packet hashes, no expiry), so a copy arriving late over a slow spreading factor is still recognised as a duplicate.
- Adaptive delay based on observed duplicates (the [ZephCore](https://github.com/liquidraver/ZephCore) contention-tracker design), a priority queue of 8, and listen-before-talk with random doubling backoff.
- Its own thread, so relay timers never compete for the radio thread's single notification slot.
- Direct frames forwarded when we are the next hop (our hop removed), direct TRACE with our SNR appended, MULTIPART ACK downgraded to a plain direct ACK.
- ADVERT floods capped at 8 hops.

### Bridge to the Meshtastic client

- **Group text**: channels named `MC:<name>` carry a MeshCore group PSK. A fresh install seeds `MC:Public`, `MC:#test` and `MC:#bot`. Text sent on one of those channels goes out as a MeshCore `GRP_TXT`; received group text appears in the app with the real sender name, the path's hop count and, on direct reception, live SNR/RSSI.
- **Direct messages**: one-to-one chat with MeshCore contacts from the Meshtastic client. ECDH derived from the repeater identity, `TXT_MSG` and ACK handling, contacts learned from signed ADVERTs (the 16 most recent are kept in flash), delivery confirmed by the MeshCore ACK or reported as failed after 30 s.
- **`/stats`**: a direct message to the node answers with per-protocol contention measurements (CAD hits, airtime) for Meshtastic and MeshCore.

### Real repeater identity

- Ed25519 identity persisted with `SafeFile` in `/prefs/meshcore_identity.bin`, so the MeshCore public key survives reboots.
- Signed ADVERTs named after the Meshtastic owner: zero-hop every 2 minutes for direct neighbours, flood every 47 hours for the whole mesh.
- Answers `NODE_DISCOVER_REQ` and appends its own hash to the path. The node shows up in the MeshCore companion's neighbour search.
- Advertises as a chat node unless the Meshtastic role is `ROUTER`, `ROUTER_LATE` or `REPEATER`.

### Built to coexist

- ADVERT signatures are verified before forwarding and again just before TX.
- Relay traffic is never decrypted; group and direct messages are only decoded when we hold the key.
- Receive prioritises Meshtastic, transmit always uses the protocol that owns the frame.

## Quick start

### Requirements

- An SX126x radio. Tested on a ThinkNode M1 (nRF52840 + SX1262), env `thinknode_m1_meshcore`.
- [PlatformIO](https://platformio.org/).
- Both networks configured with the same frequency, bandwidth and spreading factor. Meshtastic and MeshCore use different sync words, which is exactly what the hybrid receive exploits.

### Build and flash

```bash
git clone --branch meshtacore https://github.com/xejarque/Meshtacore.git
cd Meshtacore
pio run -e thinknode_m1_meshcore
pio run -e thinknode_m1_meshcore -t upload --upload-port <port>
```

| Env                              | Purpose                                                                      |
| -------------------------------- | ---------------------------------------------------------------------------- |
| `thinknode_m1_meshcore`          | Release build: sniffer, relay, companion and hybrid sync word.               |
| `thinknode_m1_meshcore_debug`    | Logs every RX/preamble event and the real sync word register.                |
| `thinknode_m1_meshcore_rxtest`   | Receives on MeshCore's own sync word (`0x1424`) to isolate the hybrid value. |
| `nrf52_promicro_diy_tcxo_meshcore` | DIY nRF52840 Pro Micro with a TCXO SX126x, MeshCore enabled.               |

### Build flags

MeshCore support is on by default; pass `-D<flag>=0` to opt out.

| Flag                                     | Default  | Effect                                                                                        |
| ---------------------------------------- | -------- | --------------------------------------------------------------------------------------------- |
| `MESHTASTIC_ENABLE_MESHCORE_SNIFFER`     | `1`      | Parse MeshCore frames from the radio without entering the Meshtastic routing pipeline.        |
| `MESHTASTIC_ENABLE_MESHCORE_RELAY`       | `1`      | Re-broadcast MeshCore floods (implies the sniffer).                                           |
| `MESHTASTIC_ENABLE_MESHCORE_COMPANION`   | `1`      | Expose `MC:` group channels and MeshCore direct messages to the client (implies the sniffer). |
| `MESHTASTIC_ENABLE_MESHCORE_HYBRID_SYNC` | `1`      | SX126x hybrid RX sync word (implies the sniffer).                                             |
| `MESHTASTIC_MESHCORE_RX_SYNCWORD`        | `0x14B4` | Raw SX126x sync word register value used in RX.                                               |
| `MESHTASTIC_MESHCORE_RX_DEBUG`           | `0`      | Log every RX/preamble event and the sync word register the chip really holds. Debug only.     |

### Tuning the hybrid sync word

`0x14B4` (MeshCore's first sync nibble, Meshtastic's second) was measured SX1262 to SX1262 on a ThinkNode M1 and receives both networks. The mirror candidate `0x2424` received Meshtastic only. If reception is poor on your hardware, build `thinknode_m1_meshcore_rxtest` to confirm the MeshCore side works on its own sync word, then treat the hybrid value as the variable.

## How it works

1. The radio receives with the hybrid sync word, so preamble and header detection accept either network.
2. A candidate MeshCore frame is trial-decrypted with our Meshtastic channels first. If it decrypts, it goes through the normal Meshtastic path.
3. Otherwise it is parsed as MeshCore and routed: floods are relayed, direct and TRACE frames forwarded when we are on the path, group and direct text delivered to the client if we hold the key.
4. Transmissions use the sync word of the protocol that owns the frame.
5. A service thread handles the repeater identity, ADVERTs and discovery replies.

Full implementation notes, wire formats, threading and failure modes: **[docs/meshcore.md](docs/meshcore.md)**.

## Status

Meshtacore is experimental and developed against a single board. It is honest about what has been verified and what has not.

Verified:

- Hybrid receive of both networks on a ThinkNode M1.
- Group text and direct messages on air against a MeshCore companion, in both directions.
- Relay and forwarding behaviour covered by the native tests.

Not verified yet:

- Hybrid sync word packet loss and sensitivity over a long run. The value was measured for reception, never for loss.
- Builds and flash fit on boards other than the ThinkNode M1.
- Other regions and modem presets.
- Region-scope filtering: the relay currently forwards region-scoped floods like any other.

If hybrid loss turns out too high on your hardware, the fallback is a second SX1262.

## Roadmap

- Measure hybrid sync word packet loss against a normal build over a long run.
- Bring up and size the build on the other SX126x boards in this repository, then run the full native suite.
- Test on more hardware, regions and presets.
- Region-scope filtering for floods.
- Admin and config for the MeshCore side (identity, advert intervals, relay on/off) from the client, instead of build flags.
- Documentation upstream in the [meshtastic/meshtastic](https://github.com/meshtastic/meshtastic) docs repository.

## Tests

The MeshCore work is covered by `test/test_meshcore_repeater` and `test/test_meshcore_companion`, plus the MeshCore channel and interoperability vectors.

```bash
./bin/run-tests.sh
```

The suite reports GREEN (0), RED (1), AMBER (2) or FILTERED (3). Run the full suite before trusting a filtered one: filtering removes the suites that create shared state for later tests.

## Contributing

Work happens on the `meshtacore` branch. Open issues and pull requests on this fork. Before proposing a change, run `./bin/run-tests.sh` and `trunk fmt`; the repository rejects unformatted code.

## Credits

Meshtacore builds on four projects:

- **[Meshtastic firmware](https://github.com/meshtastic/firmware)**: the base firmware, radio abstraction, module system and client protocol.
- **[Jentastic/Nexus](https://github.com/derpyspike/jentastic)**: the soft-fork this repository descends from, including the Nexus module and its remote administration tooling.
- **[MeshCore](https://github.com/meshcore-dev/MeshCore)**: the protocol implemented here, including frame format, encryption, ADVERTs and companion behaviour.
- **[ZephCore](https://github.com/liquidraver/ZephCore)**: the MeshCore Zephyr port. The relay's contention tracking and adaptive flood delay, duplicate-driven postponement and priority queue ordering follow its design, and parts of the implementation were adapted from it.

## License and attribution

Meshtacore is distributed under the [GNU General Public License v3.0](LICENSE). The inherited Nexus module and its accompanying scripts remain under the [GNU Affero General Public License v3](LICENSE-AGPL); AGPL files are tagged as such at the top of the file. It is a fork of [Meshtastic firmware](https://github.com/meshtastic/firmware) 2.8.1 through Jentastic/Nexus.

Meshtastic and MeshCore are independent projects with their own trademarks and communities. Meshtacore is not affiliated with or endorsed by either. MeshCore protocol details follow [meshcore-dev/MeshCore](https://github.com/meshcore-dev/MeshCore).

---

## About Meshtastic

This firmware keeps the full Meshtastic feature set. Meshtastic is an open-source LoRa mesh networking project for long-range, low-power communication without internet or cellular infrastructure, supporting ESP32, nRF52, RP2040/RP2350 and Linux devices. It provides text messaging, location sharing and telemetry over a decentralised mesh.

- [Meshtastic website](https://meshtastic.org)
- [Meshtastic documentation](https://meshtastic.org/docs/)
- [Building instructions](https://meshtastic.org/docs/development/firmware/build)
- [Flashing instructions](https://meshtastic.org/docs/getting-started/flashing-firmware/)

Meshtastic® is a registered trademark of Meshtastic LLC.
