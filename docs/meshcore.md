# How the MeshCore bridge works

This document describes the MeshCore implementation in Meshtacore: how a single SX126x listens to two protocols, how an incoming frame is classified, and what the repeater and the Meshtastic client bridge do with it.

The work lives in the `meshtacore` branch and is experimental. See [Status](../README.md#status) in the README for what has and has not been verified.

## Contents

- [Build surface](#build-surface)
- [Radio layer](#radio-layer)
- [Receive pipeline](#receive-pipeline)
- [Frame format](#frame-format)
- [Flood relay](#flood-relay)
- [Direct, TRACE and CONTROL](#direct-trace-and-control)
- [Identity and ADVERTs](#identity-and-adverts)
- [Client bridge: group text](#client-bridge-group-text)
- [Client bridge: direct messages](#client-bridge-direct-messages)
- [Threads, timers and coexistence](#threads-timers-and-coexistence)
- [Persistence](#persistence)
- [Limits and known failure modes](#limits-and-known-failure-modes)
- [Tests and debugging](#tests-and-debugging)
- [Code map](#code-map)

## Build surface

Every MeshCore feature is compiled in by default; pass `-D<flag>=0` to opt out. The release environment `thinknode_m1_meshcore` builds all of them:

| Flag                                     | Enables                                                                    |
| ---------------------------------------- | -------------------------------------------------------------------------- |
| `MESHTASTIC_ENABLE_MESHCORE_SNIFFER`     | Frame parsing and RX classification. Everything else requires it.          |
| `MESHTASTIC_ENABLE_MESHCORE_RELAY`       | Flood and direct forwarding.                                               |
| `MESHTASTIC_ENABLE_MESHCORE_COMPANION`   | `MC:` channel bridge, direct messages, contacts.                           |
| `MESHTASTIC_ENABLE_MESHCORE_HYBRID_SYNC` | The SX126x hybrid receive register.                                        |
| `MESHTASTIC_MESHCORE_RX_SYNCWORD`        | Value written to the sync word register while receiving. Default `0x14B4`. |

Dependencies are enforced with `#error` in `src/configuration.h`: relay, companion and hybrid sync all require the sniffer, and the companion requires the radio's raw TX path.

## Radio layer

### Hybrid receive

Meshtastic transmits with sync word `0x2B`; MeshCore uses RadioLib's private sync word `0x12`. The SX126x compares the received sync word at the register level, packed nibble-wise together with its control bits:

- Meshtastic: register `0x24B4` (sync word `0x2B`, control bits `0x44`).
- MeshCore: register `0x1424` (sync word `0x12`, same control bits).
- Hybrid receive: register `0x14B4`, which combines MeshCore's first nibble with Meshtastic's second.

`0x14B4` was measured SX1262 to SX1262 and receives both networks. The mirror candidate `0x2424` received Meshtastic only. The hybrid register is therefore a bench-verified value, not a protocol guarantee: reception quality is expected to depend on the radio and the preset, and packet loss has not been measured yet.

The register is written in `SX126xInterface::startReceive()`, immediately before the radio is armed for RX. `setSyncWordRegister()` performs the nibble split; it exists because RadioLib's public API only accepts a sync word plus control bits, not the raw register value.

### Transmit

`SX126xInterface::configHardwareForSend()` picks the sync word per frame:

- raw MeshCore frames (`isRawSending()`) go out with `0x12`;
- Meshtastic packets go out with `0x2B`.

MeshCore receivers also expect a longer preamble at low spreading factors: `32` symbols at SF8 and below, `16` above. The driver sets it for a raw frame and restores the Meshtastic preamble in `startReceive()`, because the SX126x RX duty cycle is sized from it.

The relay never mixes the two paths: `sendRawFrame()` refuses to start while the Meshtastic TX path is active, and the caller performs listen-before-talk first.

## Receive pipeline

The interception point is `RadioLibInterface::handleReceiveInterrupt()`. A MeshCore frame has no Meshtastic `PacketHeader`, so the bytes cannot be interpreted as Meshtastic first.

1. `MeshCoreRepeater::looksLikeMeshCoreFrame()` parses the bytes and applies three tests:
   - header validity: payload version 0, a known payload type, a valid path length byte, path and payload within MeshCore's limits;
   - payload shape: encrypted payload types must carry a 2-byte MAC followed by whole 16-byte AES blocks. A random Meshtastic frame passes this about one time in sixteen;
   - Meshtastic veto: if bytes 0..3 or 4..7 decode to our own NodeNum, the frame is Meshtastic by near-certainty and is never swallowed.
2. `isMeshtasticOnOurChannels()` trial-decrypts the payload with our channel keys (`decodesOnOurChannels()` in `Router.cpp`). A frame that decrypts and decodes as a Meshtastic `Data` message is Meshtastic, whatever it looked like.
3. Only a frame that passes both is handled as MeshCore: `meshCoreCompanion.handleFrame()` first, then `meshCoreRepeater.handleReceivedFrame()`. It never reaches the Meshtastic router.

This ordering is why Meshtastic broadcast traffic and any unicast to or from this node cannot be misclassified. The known ambiguous case is a Meshtastic unicast between two other nodes whose bytes happen to parse as MeshCore; it may be sniffed or relayed instead of routed. Broadcasts and local unicasts are excluded, and the payload-shape test makes the remaining window small.

## Frame format

MeshCore's on-air frame, as parsed by `MeshCoreRepeater::parse()`:

| Field       | Size         | Notes                                                                 |
| ----------- | ------------ | --------------------------------------------------------------------- |
| Header      | 1            | route type in bits 0-1, payload type in bits 2-5, version in bits 6-7 |
| Transport   | 0 or 4       | two 16-bit codes, present only for the transport route types          |
| Path length | 1            | hash size minus one in the top 2 bits, hop count in the bottom 6      |
| Path        | count x size | one hash per repeater the frame crossed                               |
| Payload     | up to 184    | type-specific, see below                                              |

Route types: `0` transport flood, `1` flood, `2` direct, `3` transport direct. Path hash sizes are 1 to 3 bytes, path bytes are capped at 64, and path mode 3 is reserved.

Payload types used by this implementation:

| Type | Name      | Used for                                               |
| ---- | --------- | ------------------------------------------------------ |
| 0x02 | TXT_MSG   | direct text                                            |
| 0x03 | ACK       | direct message acknowledgement                         |
| 0x04 | ADVERT    | signed node announcement                               |
| 0x05 | GRP_TXT   | group text                                             |
| 0x08 | PATH      | return path, can carry an ACK as an extra block        |
| 0x09 | TRACE     | route discovery with per-hop SNR                       |
| 0x0A | MULTIPART | fragmented message; only ACK forwarding is implemented |
| 0x0B | CONTROL   | neighbour discovery request and response               |

## Flood relay

### Deduplication

`hashFrame()` is FNV-1a over the payload type and the payload only. It deliberately excludes the path: MeshCore appends a hop hash at every forward, so hashing the whole frame would make every path variant look new and re-broadcast it. TRACE is the exception and includes its path length byte, because a TRACE may legitimately pass the same node twice.

Seen hashes live in a fixed ring of 160 entries with no time expiry (`MeshCoreDedup`), matching MeshCore's own table. A copy arriving late over a slow spreading factor must still be recognised. A hash is remembered when the frame is queued, not when it is heard: a frame dropped because the queue was full or the path was full can still be relayed if it arrives again. If a queued frame is evicted, its hash is forgotten for the same reason.

### What is forwarded

Only payload types MeshCore itself floods are relayed: ACK, REQ, RESPONSE, TXT_MSG, PATH, ANON_REQ, GRP_TXT, GRP_DATA and ADVERT, each with the minimum payload MeshCore requires. TRACE, MULTIPART, CONTROL and RAW_CUSTOM are not flood-forwarded.

Additional bounds:

- an ADVERT flood is dropped once its path holds 8 hops (`flood_max_advert`);
- any flood whose next path would exceed 64 bytes is dropped;
- our own ADVERT coming back is dropped;
- forwarding is disabled for a `CLIENT_MUTE` role or `rebroadcast_mode = NONE`, the same policy as Meshtastic's own relay.

With an identity installed, the relay appends its own path hash (the public key prefix) to the forwarded frame, so MeshCore receivers can show the route. Without an identity, the frame is relayed byte-for-byte.

### Adaptive delay

The flood delay follows the ZephCore `ContentionTracker` / `computeAdaptiveFloodDelay` design:

```
delay = 20 ms + random(0, 5 x airtime x factor)
capped at 6 x airtime and 2000 ms
```

The 20 ms floor gives direct neighbours time to return to RX. `factor` comes from an exponential moving average of the duplicates observed per relay:

- EMA of the per-frame dupe count, alpha 1/8, fast convergence for the first 4 samples;
- `factor = 0.40 + 0.60 x est / (est + 4)`, so 0.60 at four observed duplicates;
- 0.5 before the estimate is trusted (fewer than 4 samples);
- lazy decay of 1/8 per 5 s once the relay has been idle for 5 minutes, at most 8 steps per call.

Hearing a duplicate of a frame that is still queued postpones it by `0.2 x airtime` per duplicate, capped at `12 x airtime` or 2000 ms cumulatively, and within a 10 s window. This places our retransmission after the neighbour's instead of on top of it. The dupe count becomes the contention sample when the frame is released.

### Queue and backoff

The queue holds 8 frames. Priority order:

1. routed traffic (direct forwarding);
2. our own frames (ADVERT, discovery reply, client text);
3. floods, ordered by hop count, fewest hops first.

When the queue is full, a new frame may evict the worst queued flood if it ranks above it. Routed traffic and our own frames are never evicted; a new own frame can evict a flood, but never another own frame. A flood that has waited longer than 10 s without going out is dropped and its hash forgotten.

Two further randomisation layers keep repeaters from transmitting together:

- after any transmission, the next MeshCore frame is held back for 1 to 3 slots (minimum slot 20 ms);
- if the radio or channel is busy when a frame is due, the retry waits a random 1 to `2^n` slots, with `n` growing from 1 to 4 (2 to 16 slots) on consecutive deferrals.

## Direct, TRACE and CONTROL

### Direct forwarding

A direct frame is forwarded only when we are its next hop: the first path hash equals our public key prefix. The forward removes that hash (`Mesh::removeSelfFromPath` equivalent) and keeps the rest of the path. Direct traffic is never relayed by path-independence alone and is never flooded.

### TRACE

A direct TRACE carries the list of hashes to visit in its payload and collects one SNR byte per hop in its path field. When the next hash in the list is ours, the relay appends its own receive SNR, quantised as `snr x 4` in a signed byte, and forwards the frame unchanged otherwise. The path length byte grows by one.

### MULTIPART ACK

A MULTIPART frame whose low nibble is ACK is forwarded as a plain direct ACK, with the first path hop removed and the multipart counter stripped. The remaining-packet count in the high nibble adds `(remaining + 1) x 300 ms` to the delay so the ACK follows the rest of the sequence.

### Neighbour discovery

A zero-hop direct CONTROL frame carrying `NODE_DISCOVER_REQ` (0x80) is answered with `NODE_DISCOVER_RESP` (0x90) when the request filter includes our node type. Requests with a `since` timestamp are ignored because no modification time is tracked. Replies are rate-limited to 4 per 2 minutes, as in MeshCore's `simple_repeater`, and spread over a random delay widened by four because several repeaters answer the same request.

## Identity and ADVERTs

### Key material

The MeshCore identity is an Ed25519 key pair derived from a 32-byte seed stored in `/prefs/meshcore_identity.bin`. The seed is created from the hardware RNG on first use and written with `SafeFile`, which performs an atomic replace under the SPI lock (a bare write appends on nRF52 LittleFS). A seed whose derived public key starts with `0x00` or `0xFF` is rejected, because those are reserved 1-byte path hashes, and a fresh key is generated.

Without a filesystem the seed cannot be persisted: the key is recreated on every boot, which changes the MeshCore identity and invalidates direct-message contacts.

### ADVERT layout

```
header (0x04 << 2 | route)
path_len
public key (32)
timestamp (4, little-endian)
signature (64)
app data:
  flags (1)      node type in the low nibble, 0x80 when a name follows
  name (optional)
```

The signature covers `public key || timestamp || app data`, exactly the bytes MeshCore verifies. `buildAdvert()` produces either a flood ADVERT or a zero-hop direct one (`route = DIRECT`, empty path, like `Mesh::sendZeroHop`); the schedule only sends floods.

The announced node type follows the Meshtastic role: `ROUTER`, `ROUTER_LATE` and `REPEATER` advertise as repeaters, every other role advertises as a chat node, which is what MeshCore applications will start a conversation with.

### Relay alias

A node that relays while its main key announces itself as a chat node signs the hop it appends to the path with a second Ed25519 key, the _relay alias_: `SHA256("meshtastic-meshcore-relay" || seed || attempt)`, derived from the stored seed and never written anywhere (the attempt counter only moves when MeshCore would reject the derived key). MeshCore applications name a path hop only from a contact they hold as a repeater, so the alias also sends a repeater-typed ADVERT, with the same name plus ` (relay)`, in the same slot as the main flood. Without it the hop shows as an unknown repeater and the key prefix; with it the hop resolves to the node's name while the chat identity stays in the clients' contact list. A repeater-typed main identity (`ROUTER`, `ROUTER_LATE`, `REPEATER`) needs no alias, and a node that does not relay (`CLIENT_MUTE`, or rebroadcast `NONE`) derives none.

Direct frames and traces whose next hop is the alias are forwarded like any other (a hop naming the main key still is, because a peer messaging the chat contact addresses us by it); the alias only claims a hop while it is announced. An own alias ADVERT heard back is neither relayed nor learned as a contact. Neighbour discovery answers with the alias for a repeater request and with the main key for a chat one, so a filter covering both gets two replies and spends two slots of the shared rate limit (four replies per two minutes).

The alias is a repeater persona only: direct messages addressed to it are not answered (the companion reads only the main key), so a MeshCore client that logs in to the ` (relay)` contact gets no reply. Use the chat contact to message the node.

### Scheduling

A dedicated thread runs the advert and housekeeping service:

- identity is loaded about 5 seconds after boot;
- the first ADVERT is a flood 3 minutes after boot, then every 12 hours;
- every ADVERT is a flood: it reaches the direct neighbours too, and it is the only kind worth the airtime;
- when a relay alias is in use, its ADVERT is queued on the next tick, once the main one is on air; it bypasses the 12-hour deadline while pending, and gives up after three failed queue attempts until the next slot;
- an ADVERT is postponed while there is no valid wall-clock time, because MeshCore clients order adverts by timestamp;
- the name is the Meshtastic owner long name, with a `Meshtastic-xxxx` fallback when the owner is empty.

Signature handling is deliberately placed at the last moment. Flooded ADVERTs are not verified on receipt; verification runs in `takePendingFrame()`, just before transmission and while the radio is still listening, because an Ed25519 verification takes long enough to deafen the receiver. A bad signature drops the frame and the next one is tried. The companion additionally verifies ADVERTs before learning a contact.

## Client bridge: group text

### Channels

A Meshtastic channel is a MeshCore group channel when it is enabled and its name starts with `MC:`. Its PSK must be 16 or 32 bytes. A 32-byte key whose second half is all zeroes is treated as a 16-byte key, matching MeshCore's representation.

On a fresh install, `Channels::initDefaults()` seeds three secondaries: `MC:Public`, `MC:#test` and `MC:#bot`. Existing channels are never overwritten, so user preferences win.

### Channel hash and encryption

The 1-byte channel hash is the first byte of `SHA-256(effective key)`. Group text is AES-128 in ECB-style block encryption, with a 2-byte MAC derived from `HMAC-SHA256(secret, ciphertext)`.

Frame layout:

```
header (GRP_TXT, route flood)
path_len
channel hash (1)
MAC (2)
ciphertext (whole 16-byte blocks)
```

Plaintext layout:

```
timestamp (4, little-endian)
text type (1)        0 = plain
body                 "sender: text"
```

The body is limited to 160 bytes including the `sender: ` prefix, and truncation respects UTF-8 boundaries. The sender name comes from the Meshtastic owner; if that is empty, the short name is used, then the literal `Meshtastic`.

### Outbound

`MeshService::sendToMesh()` intercepts a broadcast text on an `MC:` channel before the Meshtastic path:

1. it obtains the effective key from the channel;
2. it requires a valid wall-clock time, because MeshCore timestamps are wall-clock;
3. it encodes a `GRP_TXT` flood with a strictly increasing timestamp, so two texts sent in the same second still encrypt differently and are not dropped as duplicates;
4. it queues the frame through `queueMeshCoreFrame()`, which applies the same duty-cycle guard as Meshtastic TX;
5. it reports a local implicit ACK to the client, because MeshCore peers cannot acknowledge a Meshtastic packet.

Any other port number on an `MC:` channel is dropped: sending it would put non-MeshCore traffic on the air encrypted with the MeshCore key.

### Inbound

`MeshCoreCompanion::handleFrame()` tries every `MC:` channel, comparing the frame's channel hash against a cached per-channel hash before doing HMAC and AES work. A matching frame is decrypted and injected into the Meshtastic router as a `TEXT_MESSAGE_APP` broadcast on that channel, so the client renders it alongside Meshtastic traffic.

The sender is presented as a synthetic node:

- a contact with that name becomes the contact's node, messagable and carrying its public key;
- otherwise a node number is hashed from the sender name (`0x4D` prefix);
- a body without a `sender: ` prefix falls back to the generic `MeshCore` node (`0x4D430001`, unmessagable).

Hop fields map the MeshCore path length to Meshtastic hops (cap 7). SNR and RSSI are attached only for zero-hop frames, because clients show signal strength only for direct traffic.

## Client bridge: direct messages

### Key exchange

MeshCore direct messages use ECDH over the Ed25519 identities, not over separate X25519 keys. `sharedSecret()` performs the conversion:

- the peer's Edwards `y` is mapped to a Montgomery `u` with `u = (1 + y) / (1 - y)`;
- our scalar is the clamped first half of `SHA-512(seed)`;
- `Curve25519::eval()` produces the shared secret and weak points are rejected.

This path depends on the XEdDSA field arithmetic being compiled in. On a build with `MESHTASTIC_EXCLUDE_XEDDSA`, `sharedSecret()` returns false and direct messages are unavailable.

### Contacts

Contacts are learned from signed ADVERTs. Only named chat nodes are accepted: repeaters and rooms are infrastructure, not people to message. A known contact with an unchanged name only refreshes its LRU stamp and skips the signature check; anything new is verified first.

The RAM table holds 32 entries and evicts the least recently used. Each entry keeps the public key, name, node number, cached shared secret and LRU stamp. Node numbers are hashed from the public key with the `0x4D` top byte, and collisions are moved on.

The 16 most recently used contacts are persisted, because flash space and write endurance matter more than a complete history. Contacts reach the client as synthetic nodes with `is_unmessagable = false` and the MeshCore public key in the node's key field, which is what makes the client offer a direct message.

### Outbound

`MeshService::sendToMesh()` recognises a text addressed to a node that is a known contact and calls `sendDirectText()`:

1. the shared secret is derived and cached;
2. the timestamp is strictly increasing;
3. the text is cut to 160 bytes on a UTF-8 boundary;
4. a `TXT_MSG` flood is encoded;
5. the ACK hash is stored in a pending table (8 slots) together with the Meshtastic packet id;
6. the frame is queued for raw transmission.

Frame layout:

```
header (TXT_MSG)
path_len
destination hash (1)     first byte of the peer's public key
source hash (1)          first byte of our public key
MAC (2)
ciphertext
```

Plaintext:

```
timestamp (4, little-endian)
flags (1)                text type in bits 2-7, send attempt in bits 0-1
text
```

The ACK hash is the first 4 bytes (little-endian) of `SHA-256(timestamp || flags || text || sender public key)`. A MeshCore peer can answer with a plain `ACK` frame carrying that hash, or with a `PATH` frame that carries the ACK as an extra block; both are handled.

When an ACK arrives, the matching pending entry reports `Routing` success to the client. If no ACK arrives within 30 seconds, the entry reports `MAX_RETRANSMIT` and the client marks the message failed. If the pending table is full, the oldest entry is resolved the same way.

### Inbound

An incoming `TXT_MSG` is ours when its destination hash equals the first byte of our public key. The source hash is only 1 byte, so several contacts can share it; the MAC decides which shared secret decrypts the frame. On success:

- the text is injected into the router as a direct text from that contact;
- the contact's LRU stamp is refreshed;
- an ACK carrying `ackHash(timestamp, flags, text, peer public key)` is queued as a flood ACK.

Text with a non-plain type is ignored.

## Threads, timers and coexistence

The radio notification slot is single and already carries Meshtastic RX/TX, so MeshCore work is isolated:

- `MeshCoreAdvertThread` runs identity loading, advert scheduling, contact persistence and direct-message timeout checks, with a maximum 5 s interval;
- `MeshCoreRelayThread` is disabled while the queue is empty and is woken by RX activity, TX completion and the advert thread. While a raw frame is on air it polls every 100 ms to recover a lost TX-done;
- `maybeSendMeshCoreFrame()` performs listen-before-talk: it waits for the radio to be idle, runs channel activity detection, re-checks that a Meshtastic TX did not start during the scan, and only then transmits;
- a MeshBeacon holding the radio on another preset defers MeshCore TX;
- every raw frame is counted against the duty cycle, using the same effective duty-cycle guard as Meshtastic;
- `MESHCORE_TX_POLL_MS` and the post-TX hold-off keep a raw frame from colliding with the normal TX path.

All time comparisons use `Throttle`, so deadlines survive the 32-bit `millis()` wrap.

## Persistence

| Path                           | Contents                                                         |
| ------------------------------ | ---------------------------------------------------------------- |
| `/prefs/meshcore_identity.bin` | 32-byte Ed25519 seed, written atomically with `SafeFile`.        |
| `/prefs/meshcore_contacts.bin` | Magic `MCC`, version 1, count, then 16 records of key plus name. |

Contacts are saved at most every 10 minutes and only when a fingerprint over keys and names changes. LRU order is not part of the fingerprint, so an ordinary ADVERT does not cause a flash write. On restore, records are loaded oldest first so the newest ends up most recently used.

## Limits and known failure modes

- **Misclassification**: a Meshtastic unicast between two other nodes that happens to parse as MeshCore can be sniffed or relayed. Broadcasts and frames to or from this node are excluded by the Meshtastic veto and the trial decrypt.
- **Hybrid sync loss**: the register value is measured, not characterised. Sensitivity and packet loss versus a normal build are unknown.
- **SX126x only**: the hybrid receive is a register feature of that family. Other radios can run the code, but will not hear MeshCore with a Meshtastic sync word.
- **No region scoping**: MeshCore repeaters can enforce region scoping through transport codes. This relay has no region map and forwards scoped floods like any other.
- **No runtime configuration**: identity, advert intervals and relay on/off are build flags. The MeshCore side has no admin or config surface in the client yet.
- **Wall-clock time required**: group text, direct messages and ADVERTs are postponed until a valid time is available.
- **Filesystem required for continuity**: without `FSCom`, the identity is regenerated at boot and contacts are not persisted.
- **XEdDSA required for DMs**: excluding it disables key exchange, so direct messages cannot be sent or received.
- **Flash wear**: the identity is written once; contacts are rate-limited to one write per 10 minutes.

## Tests and debugging

Native tests:

- `test/test_meshcore_repeater` covers parsing and the Meshtastic veto, deduplication and path independence, flood bounds, queue ordering and eviction, adaptive delay bounds and contention factor, duplicate postponement, wrap safety, stale drops, direct/TRACE/MULTIPART/CONTROL handling, path rewriting, ADVERT signing and verification, and role mapping.
- `test/test_meshcore_companion` covers channel hashes and interoperability vectors, group text round trips, MAC and tamper rejection, UTF-8 truncation, key normalisation, ECDH symmetry, direct text and ACK hashes, PATH ACK decoding, contact LRU and persistence, and node numbering.

Run them with the rest of the suite:

```bash
./bin/run-tests.sh
```

Diagnostic environment:

- `thinknode_m1_meshcore_rxtest` receives on MeshCore's own sync word (`0x1424`), isolating the hybrid value from a radio or parameter problem.

Useful log lines: `MeshCore frame:` (classification), `MeshCore relay queued:` (delay and priority), `MeshCore TX own/relay` (raw transmission), `MeshCore identity ready` and `MeshCore ADVERT queued`.

## Code map

| File                                | Responsibility                                                                            |
| ----------------------------------- | ----------------------------------------------------------------------------------------- |
| `src/configuration.h`               | Feature flags and defaults.                                                               |
| `src/mesh/SX126xInterface.cpp`      | Hybrid sync word on RX, per-protocol sync word and preamble on TX.                        |
| `src/mesh/RadioLibInterface.cpp`    | RX interception, raw TX, relay and advert threads, identity and contact persistence.      |
| `src/mesh/MeshCoreRepeater.h/.cpp`  | Frame parser, dedupe policy, relay queue and delays, identity, ADVERT, discovery replies. |
| `src/mesh/MeshCoreDedup.h`          | Fixed ring of seen frame hashes.                                                          |
| `src/mesh/MeshCoreCompanion.h/.cpp` | Group and direct text codecs, contacts, ACKs, injection into the Meshtastic router.       |
| `src/mesh/MeshCoreContacts.h`       | LRU contact table and node numbering.                                                     |
| `src/mesh/Channels.cpp`             | `MC:` channel detection, key normalisation and default seeding.                           |
| `src/mesh/MeshService.cpp`          | Intercepts outbound text and routes it to the MeshCore bridge.                            |
| `src/mesh/Router.cpp`               | `decodesOnOurChannels()`, the Meshtastic-first trial decrypt.                             |

## Credits

The MeshCore protocol and its frame, crypto and ADVERT formats come from [MeshCore](https://github.com/meshcore-dev/MeshCore). The relay's contention tracking, adaptive flood delay, duplicate postponement and queue ordering follow the design of [ZephCore](https://github.com/liquidraver/ZephCore), the MeshCore Zephyr port. The base firmware, radio abstraction and client protocol come from [Meshtastic firmware](https://github.com/meshtastic/firmware).
