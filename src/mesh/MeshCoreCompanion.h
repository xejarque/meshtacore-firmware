#pragma once

#include "configuration.h"

#if MESHTASTIC_ENABLE_MESHCORE_COMPANION

#include "Channels.h"
#include "MeshCoreDedup.h"
#include "MeshTypes.h"
#include "MeshCoreNodeFile.h"
#include "MeshCoreNodeStore.h"
#include "MeshCoreRepeater.h"
#include "MeshCoreSecretCache.h"
#include <stddef.h>
#include <stdint.h>

class RadioInterface;

/// How a MeshCore frame reached us, carried by the packet that announces the node it came from: the repeaters it crossed
/// and, when it came straight from the node (none crossed), its signal. Without a frame: one hop and no signal.
struct MeshCoreRx {
    uint8_t pathHashCount = 1;
    float snr = 0;
    int32_t rssi = 0;

    MeshCoreRx() = default;
    MeshCoreRx(uint8_t hops, float snrDb, int32_t rssiDbm) : pathHashCount(hops), snr(snrDb), rssi(rssiDbm) {}
};

/**
 * MeshCore group-text bridge for the Meshtastic client.
 *
 * Channels named "MC:..." carry a MeshCore group PSK. Text sent on one is encoded as a MeshCore
 * GRP_TXT flood (timestamp + "name: text", AES-128 + 2-byte HMAC); GRP_TXT frames we can decrypt
 * are injected into the Meshtastic router as a broadcast from PSEUDO_NODE_NUM, so the phone shows
 * them on that channel alongside Meshtastic traffic.
 */
class MeshCoreCompanion
{
  public:
    /// Node number all bridged MeshCore messages appear to come from: the system bot, shared with the ".path" reports.
    static constexpr uint32_t PSEUDO_NODE_NUM = NODENUM_SYSTEM_BOT;

    /// MeshCore MAX_TEXT_LEN: the encrypted body's limit, including the "name: " prefix.
    static constexpr size_t MAX_TEXT_LEN = 160;

    /// Largest ciphertext a valid GRP_TXT carries: 5-byte header + MAX_TEXT_LEN, padded to the AES block.
    static constexpr size_t MAX_CIPHER_LEN = 176;

    /// Effective secret length (16 or 32): a 32-byte key with an all-zero second half is a 16-byte secret.
    /// Returns 0 for any other key length.
    static size_t normalizedKeyLen(const uint8_t *key, size_t keyLen);

    /// First byte of SHA-256 over the effective secret, MeshCore's GroupChannel::hash.
    static bool channelHash(const uint8_t *key, size_t keyLen, uint8_t *out);

    /**
     * Encode a GRP_TXT flood frame. sender is prefixed as "sender: "; text is truncated to
     * MAX_TEXT_LEN on a UTF-8 boundary; *truncated (optional) reports whether text lost its tail.
     * Returns the frame length, or 0 if it does not fit.
     */
    static size_t encodeGroupText(const uint8_t *key, size_t keyLen, const char *sender, uint32_t timestamp, const char *text,
                                  uint8_t *out, size_t outCap, bool *truncated = nullptr);

    /**
     * Decrypt and validate a GRP_TXT frame for key. textOut receives the decrypted "sender: text"
     * body (NUL-terminated, UTF-8 sanitised). Returns false on a wrong channel hash, bad MAC,
     * malformed ciphertext or unsupported text type.
     */
    static bool decodeGroupText(const uint8_t *key, size_t keyLen, const uint8_t *frame, size_t len,
                                const MeshCoreRepeater::FrameInfo &info, char *textOut, size_t textOutCap,
                                uint32_t *timestampOut);

    /// MeshCore ECDH: our Ed25519 seed and the peer's Ed25519 public key give the 32-byte secret both sides
    /// derive (orlp ed25519_key_exchange: X25519 over the clamped SHA-512 scalar and the key's Montgomery u).
    /// False for a malformed or weak peer key.
    static bool sharedSecret(const uint8_t seed[32], const uint8_t peerPub[32], uint8_t out[32]);

    /// Largest ciphertext a valid TXT_MSG carries: 5-byte header + MAX_TEXT_LEN, padded to the AES block.
    static constexpr size_t MAX_DIRECT_CIPHER_LEN = MAX_CIPHER_LEN;

    /// TXT_MSG flood flags byte: text type in bits 2-7 (0 = plain), send attempt in bits 0-1.
    static constexpr uint8_t TXT_FLAGS_PLAIN = 0;

    /**
     * Encode a TXT_MSG flood frame for a peer: dest_hash | src_hash | MAC | AES(timestamp, flags, text). hashes are the
     * first byte of each public key. text is truncated to MAX_TEXT_LEN on a UTF-8 boundary. Returns the frame length,
     * or 0 if it does not fit.
     */
    static size_t encodeDirectText(const uint8_t secret[32], uint8_t destHash, uint8_t srcHash, uint32_t timestamp, uint8_t flags,
                                   const char *text, uint8_t *out, size_t outCap, bool *truncated = nullptr);

    /**
     * Verify and decrypt a TXT_MSG for secret. textOut gets the NUL-terminated, sanitised text; the timestamp and flags
     * are returned as sent. False on a bad MAC, malformed ciphertext or a non-plain text type.
     */
    static bool decodeDirectText(const uint8_t secret[32], const uint8_t *frame, size_t len,
                                 const MeshCoreRepeater::FrameInfo &info, char *textOut, size_t textOutCap,
                                 uint32_t *timestampOut, uint8_t *flagsOut);

    /// The 4-byte ACK a peer answers a TXT_MSG with: SHA-256(timestamp | flags | text | senderPub)[:4], where
    /// senderPub belongs to whoever wrote the text (BaseChatMesh::onPeerDataRecv).
    static uint32_t ackHash(uint32_t timestamp, uint8_t flags, const char *text, const uint8_t senderPub[32]);

    /// MeshCore answers a flooded TXT_MSG with a PATH frame that carries the ACK inside (Mesh::createPathReturn): path_len,
    /// path, extra type (ACK), hash, all encrypted with the shared secret. These two build and read that frame.
    static size_t encodePathAck(const uint8_t secret[32], uint8_t destHash, uint8_t srcHash, uint32_t hash, uint8_t *out,
                                size_t outCap);
    static bool decodePathAck(const uint8_t secret[32], const uint8_t *frame, size_t len, const MeshCoreRepeater::FrameInfo &info,
                              uint32_t *hashOut);

    /// Encode an ACK flood frame carrying hash. Returns the frame length, or 0 if it does not fit.
    static size_t encodeAck(uint32_t hash, uint8_t *out, size_t outCap);

    /// Meshtastic hop fields for a frame that crossed pathHashCount repeaters. Clients show hops as
    /// hop_start - hop_limit, so 0 hops stays "direct" and anything beyond HOP_START is capped there.
    static constexpr uint8_t HOP_START = 7;
    static void hopFields(uint8_t pathHashCount, uint8_t *hopStart, uint8_t *hopLimit);

#if MESHTASTIC_MESHCORE_PATH_COMMAND
    /// How a bridged text was heard, kept so ".path" can report it. RAM only: the ring evicts it, a reboot loses it.
    struct HeardPath {
        static constexpr uint8_t DIRECT_CHANNEL = 0xFF; // channel of a direct message: no MeshCore channel has this index

        uint32_t packetId = 0;  // the id the client sees; a reply points at it
        uint32_t frameHash = 0; // hashFrame(): the same text over another route carries the same hash
        uint32_t fromNum = 0;   // sender node, to find the newest text from a contact
        uint32_t heardMs = 0;
        float snr = 0;
        int32_t rssi = 0;
        uint8_t channel = 0;
        uint8_t routeType = 0;
        uint8_t hashSize = 0;
        uint8_t hopCount = 0;
        bool own = false;      // our own transmission: the echoes of it borrow its id, but it is no route itself
        uint8_t path[64] = {}; // MeshCore MAX_PATH_SIZE, so every hop of the route is kept and reported
    };
    /// One route per copy of a text: a flood reaches us over several, and ".path" lists them all.
    static constexpr size_t PATH_MEMORY = 16;

    /**
     * Record one route by which a bridged text was heard, so ".path" can report it. The same route heard again only
     * refreshes its signal. packetId is the id the client sees on the first copy; a later copy passes 0 and borrows
     * it from the routes already recorded for frameHash.
     */
    void noteHeardRoute(uint32_t packetId, uint32_t frameHash, uint32_t fromNum, uint8_t channel, const uint8_t *path,
                        uint8_t hashSize, uint8_t hopCount, uint8_t routeType, float snr, int32_t rssi);

    /// Record a text we transmitted, so the repeaters that re-flood it are heard as its routes. Carries no route itself.
    void noteSentText(uint32_t packetId, uint32_t frameHash, uint32_t fromNum, uint8_t channel);

    /// Answer a local ".path"/".ruta" the client sent: true when it was answered here and must not be transmitted.
    /// direct = a text for a MeshCore contact (TXT_MSG), false = a broadcast on a MeshCore channel (GRP_TXT).
    bool handlePathCommand(const meshtastic_MeshPacket *p, bool direct);

    /// Receives one client-sized piece of the report. Nothing is ever truncated: a report longer than one message is
    /// split into as many pieces as it needs.
    using ReportSink = void (*)(void *ctx, const char *text, size_t len);

    /// Render every known route, shortest first, into client-sized pieces through sink. Names come from the node DB
    /// and the contact table; several candidates for one hash are listed. Public for tests.
    size_t formatPathReport(const HeardPath *const *routes, size_t count, const char *sender, ReportSink sink,
                            void *ctx) const;
#endif

    /// Create the synthetic "MeshCore" node if it is missing, so the client has a sender name to render.
    /// It stands in for any text that carries no "name: " prefix.
    void ensurePseudoNode();

    /// Split a decoded "name: text" body at the first ": ". Returns false when there is no usable name
    /// (missing separator or empty name); otherwise nameOut holds the name, cut to nameCap-1 bytes on a
    /// UTF-8 boundary, and *body points at the text after the separator.
    static bool splitSender(const char *text, char *nameOut, size_t nameCap, const char **body);

    /// Node number standing in for a MeshCore sender: 'M' in the top byte, 24 bits hashed from the name.
    static uint32_t senderNodeNum(const char *name);

    /// How long a heard node keeps its last-heard before the next frame from it is announced again.
    static constexpr uint32_t HEARD_REFRESH_SECS = 10 * 60;

    /// The user a bridged sender is announced with: long name is the name plus " (MC)", short name its first 4 bytes
    /// (never splitting a UTF-8 character).
    /// messagable=true marks a direct-message contact: the client lets the user write to it. publicKey (32 bytes, the
    /// contact's MeshCore key) goes into the user's key field, because clients only offer a direct message to a node that
    /// has one. The text itself is encrypted by MeshCore, not with that key.
    static meshtastic_User senderUser(const char *name, bool messagable, const uint8_t *publicKey,
                                      meshtastic_Config_DeviceConfig_Role role);

    /// Whether a frame from the node behind lite (null if unknown) is worth announcing: the node is new, was never heard over
    /// the radio, differs from user, or was last heard HEARD_REFRESH_SECS ago (now is epoch seconds, 0 when the clock is
    /// untrusted).
    static bool needsAnnounce(const meshtastic_NodeInfoLite *lite, const meshtastic_User &user, uint32_t now);

    /// Announce a bridged sender to the router when needsAnnounce says so. The node is born and updated by that packet, as
    /// any Meshtastic node is by the ones it sends; nothing is written to the node DB here. rx is how the frame was heard.
    void ensureSenderNode(uint32_t nodeNum, const char *name, bool messagable = false, const uint8_t *publicKey = nullptr,
                          meshtastic_Config_DeviceConfig_Role role = meshtastic_Config_DeviceConfig_Role_CLIENT,
                          const MeshCoreRx &rx = MeshCoreRx());

    /// Record a node's position from an ADVERT (degrees x 1e6, as MeshCore sends it) and tell the client. Nothing is done
    /// for 0,0 (no position) or a position the node DB already holds.
    void updatePosition(uint32_t nodeNum, int32_t latMicro, int32_t lonMicro);

    /// Remember who wrote a bridged channel text (by the packet id the client sees), so a reply to it can name them.
    void noteRecentText(uint32_t packetId, const char *sender);
    const char *recentSender(uint32_t packetId) const;

    /// A MeshCore reply opens with "@[name] " naming the text it answers. Returns the packet id of the newest bridged
    /// text by that name and, when rest is not null, points *rest past the tag. Returns 0 and leaves *rest at text when
    /// there is no tag or no recent text.
    uint32_t replyTarget(const char *text, const char **rest) const;

    /// Rewrite an outgoing channel text the way MeshCore apps expect: "@[name] " in front of a reply (replyId is the packet
    /// id of the text answered, 0 if none) and "@name" as "@[name]" (a known name whole, otherwise the next word). Returns the
    /// length.
    size_t prepareGroupText(const char *text, uint32_t replyId, char *out, size_t cap);

    /// Wait for a repeater to echo the channel text frame we just queued, then tell the client packetId was delivered; if
    /// nobody does within ACK_TIMEOUT_MS it is reported as failed. False if frame is not a MeshCore frame.
    bool trackGroupText(const uint8_t *frame, size_t len, uint32_t packetId, uint8_t channel);

    /// Timestamp for the next outgoing text: strictly increasing, so two texts sent within the same
    /// second still encrypt differently and are not dropped as duplicates by repeaters.
    uint32_t nextTxTimestamp(uint32_t now);

    /// Consume a received MeshCore frame: GRP_TXT on any MC: channel and TXT_MSG for us are injected into the
    /// router, an ACK settles a pending direct message, an ADVERT teaches us a contact.
    void handleFrame(const uint8_t *raw, size_t len, float snr, int32_t rssi);

    /// Install our MeshCore identity (Ed25519 seed and public key); direct messages need it.
    void setIdentity(const uint8_t seed[32], const uint8_t pub[32]);
    /// The relay alias belongs to this node too: its ADVERT is ours coming back, not a repeater to learn.
    void setRelayAliasPub(const uint8_t pub[32]);
    bool hasIdentity() const { return identityReady; }

    /// True when node is a direct-message contact we hold a public key for.
    bool hasContact(uint32_t nodeNum) { return contactByNodeNum(nodeNum) != nullptr; }

    /// Learn or refresh a chat contact, numbered from its name the first time and keeping that number afterwards.
    MeshCoreNode *learnContact(const uint8_t pub[32], const char *name);
    /// The next node, from slot cursor on, that a client which just synced has not been told about: in the store but no
    /// longer in the node DB. A consistent copy goes to out (the radio may be writing the store meanwhile) and cursor moves
    /// past it. False when the walk is done.
    bool nextReplayNode(size_t &cursor, MeshCoreNode &out) const;

    /// Learn or refresh a repeater, numbered from its public key.
    MeshCoreNode *learnRepeater(const uint8_t pub[32], const char *name);

    /// Queue a TXT_MSG for the contact behind nodeNum and wait for a repeater to re-flood it, then for its ACK, answering
    /// packetId to the client. text is cut to MAX_TEXT_LEN on a UTF-8 boundary. False if not queued.
    bool sendDirectText(uint32_t nodeNum, uint32_t packetId, const char *text, uint32_t now, RadioInterface *iface);

    /// How long a direct message waits for its ACK before the client is told it failed.
    static constexpr uint32_t ACK_TIMEOUT_MS = 30000;

    /// Run periodically: report direct messages whose ACK never came.
    void service();

    /// The MeshCore nodes worth keeping across a reboot as a flash image (MeshCoreNodeFile): favourites, then
    /// conversations, then the rest. Returns the image length, 0 if cap is below MeshCoreNodeFile::IMAGE_MAX.
    size_t exportContacts(uint8_t *buf, size_t cap);

    /// Restore nodes from an exportContacts() image, or from the contacts file of earlier builds. Returns how many loaded.
    size_t importContacts(const uint8_t *buf, size_t len);

    /// Checksum of the saved set (not its order), to save only when it changed.
    uint32_t contactsFingerprint();

    /// Mark one of our own transmitted frames seen, so hearing it back does not inject it.
    void noteTransmitted(const uint8_t *raw, size_t len);

    uint32_t rxSeen = 0, rxDecoded = 0, rxDup = 0, rxIgnored = 0, txSent = 0;
    uint32_t contactsLearned = 0, dmRx = 0, dmTx = 0, dmAcked = 0, dmTimedOut = 0, groupEchoed = 0, groupTimedOut = 0;

    /// Every MeshCore node we know, kept apart from the Meshtastic node DB.
    MeshCoreNodeStore<MESHCORE_NODES_MAX> nodes;

  private:
    static constexpr size_t DEDUP_ENTRIES = 64;

    /// Cached channelHash() per channel slot, recomputed when the key bytes change.
    struct HashCacheEntry {
        uint8_t key[32] = {};
        size_t keyLen = 0;
        uint8_t hash = 0;
    };

    MeshCoreDedup<DEDUP_ENTRIES> dedup; // hashFrame() never returns 0, the empty-slot marker
    HashCacheEntry hashCache[MAX_NUM_CHANNELS];
    uint32_t lastTxTimestamp = 0;

    uint8_t identitySeed[32] = {};
    uint8_t identityPub[32] = {};
    bool identityReady = false;
    uint8_t aliasPub[32] = {};
    bool aliasReady = false;

    /// A direct message on air whose ACK we are waiting for.
    struct PendingAck {
        uint32_t ackHash = 0;
        uint32_t frameHash = 0; // the TXT_MSG as sent: a repeater re-flooding it is the relay echo
        uint32_t packetId = 0;
        uint32_t contactNum = 0;
        uint32_t sentMs = 0;
        bool used = false;
        bool group = false;   // a channel text waiting to be heard back, not a direct message waiting for an ACK
        bool relayed = false; // a repeater re-flooded it: the client has been told, the ACK is still awaited
    };
    static constexpr size_t PENDING_ACKS = 8;
    PendingAck pending[PENDING_ACKS];

    void handleGroupText(const uint8_t *raw, size_t len, const MeshCoreRepeater::FrameInfo &info, float snr, int32_t rssi);
    void handleAdvert(const uint8_t *raw, size_t len, const MeshCoreRepeater::FrameInfo &info, float snr, int32_t rssi);
    void handleDirectText(const uint8_t *raw, size_t len, const MeshCoreRepeater::FrameInfo &info, float snr, int32_t rssi);
    void handleAck(const uint8_t *raw, size_t len, const MeshCoreRepeater::FrameInfo &info);
    void handlePathAck(const uint8_t *raw, size_t len, const MeshCoreRepeater::FrameInfo &info);
    /// An ACK with this hash arrived (plain or inside a PATH): report the matching direct message delivered.
    void settleAck(uint32_t hash);
    /// A repeater re-flooded the direct message with this frame hash: report it relayed, through the hop with this hash byte.
    void settleRelay(uint32_t hash, uint8_t relayHop);

    /// Drop the unmessagable node an earlier channel text created for this name, now that the name is a contact.
    void retireGroupNode(const char *name);
    /// Drop the node an earlier build made for this contact under a public-key-derived number.
    void retireKeyedNode(const uint8_t pub[32], uint32_t keepNum);

    /// Peers a 1-byte MeshCore hash can stand for: the MAC picks the right one.
    static constexpr size_t HASH_CANDIDATES_MAX = 8;
    static constexpr size_t SECRETS_MAX = 32;
    MeshCoreSecretCache<SECRETS_MAX> secrets;

    /// The ECDH secret shared with n, derived on a cache miss; null without an identity or for a bad key.
    const uint8_t *secretFor(const MeshCoreNode &n);
    MeshCoreNode *contactByPub(const uint8_t pub[32]);
    MeshCoreNode *contactByNodeNum(uint32_t nodeNum);
    /// Mark our nodes in the node DB as MeshCore (evicted early) and copy back the client's favourite stars, which it sets
    /// there.
    void syncWithNodeDB();
    /// Keep what an ADVERT says about n: its timestamp, position, and RF reception metrics.
    void noteAdvert(MeshCoreNode *n, uint32_t advertTime, int32_t latMicro, int32_t lonMicro, float snr, int32_t rssi,
                    uint8_t hops);
    /// Inject a bridged text as received: the one place that builds the synthetic packet, for both channels and DMs.
    /// contact is the sender when direct; hop count and signal mirror the frame being reported.
    uint32_t injectText(uint32_t fromNum, uint32_t to, uint8_t channel, const char *text, uint32_t replyId, bool direct,
                        const MeshCoreNode *contact, uint8_t pathHashCount, float snr, int32_t rssi);
    /// Inject a received direct text; returns the packet id the client sees, or 0 when it could not be injected.
    uint32_t injectDirectText(const MeshCoreNode &c, const char *text, const MeshCoreRepeater::FrameInfo &info, float snr,
                              int32_t rssi);
#if MESHTASTIC_MESHCORE_PATH_COMMAND
    HeardPath heard[PATH_MEMORY];
    size_t heardNext = 0;
    const HeardPath *heardFind(uint32_t packetId) const;
    /// Names behind a path hash: the node DB first (repeaters live there), then the MeshCore node store, whose keys outlive
    /// a node DB eviction. Duplicates (the same node listed twice) collapse, so an ambiguous hash still shows names.
    /// Returns the total found; up to max names are written to out.
    size_t pathHopNames(const uint8_t *hash, uint8_t size, const char **out, size_t max) const;

    /// Everything injectReportChunk() needs to send one report piece as a text from the right sender.
    struct ReportInject {
        MeshCoreCompanion *self;
        uint32_t fromNum, to, replyId;
        uint8_t channel;
        bool direct;
        const MeshCoreNode *contact;
        uint8_t hops;
        float snr;
        int32_t rssi;
    };
    static void injectReportChunk(void *ctx, const char *text, size_t len);
#endif
    void queueAck(uint32_t hash);
    /// Tell the client how a direct message ended, as if the contact itself had answered. relayNode >= 0 marks a relay.
    void reportDelivery(uint32_t fromNum, uint32_t packetId, meshtastic_Routing_Error err, int relayNode = -1);
    PendingAck *addPending(uint32_t ackHash, uint32_t packetId, uint32_t contactNum, bool group = false);
    /// A repeater re-flooded the channel text with this frame hash: report it delivered.
    void settleEcho(uint32_t hash);
    void announcePosition(uint32_t nodeNum, int32_t latitudeI, int32_t longitudeI);

    struct RecentText {
        uint32_t packetId = 0;
        char sender[MeshCoreNode::NAME_LEN] = {};
    };
    static constexpr size_t RECENT_TEXTS = 32;
    RecentText recent[RECENT_TEXTS];
    size_t recentNext = 0;

    /// Tell the router, and through it the client, about a node: a NODEINFO packet from it carrying how the frame was heard.
    /// That packet is what puts the node in the node DB with its name, last-heard, signal, hops and the LoRa slot it was
    /// heard on, exactly as for a Meshtastic node.
    void announceUser(uint32_t nodeNum, const meshtastic_User &user, const MeshCoreRx &rx);
    void announceIfNeeded(uint32_t nodeNum, const meshtastic_User &user, const MeshCoreRx &rx);

    bool cachedChannelHash(ChannelIndex ch, const uint8_t *key, size_t keyLen, uint8_t *out);
    bool wasRecentlySeen(uint32_t hash) const { return dedup.contains(hash); }
    void rememberFrame(uint32_t hash) { dedup.remember(hash); }
};

extern MeshCoreCompanion meshCoreCompanion;

#endif // MESHTASTIC_ENABLE_MESHCORE_COMPANION
