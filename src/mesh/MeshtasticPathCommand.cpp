#include "MeshtasticPathCommand.h"

#if MESHTASTIC_PATH_COMMAND

#include "Channels.h"
#include "MeshTypes.h"
#include "NodeDB.h"
#include "PathReport.h"
#include "Router.h"
#include "UptimeClock.h"
#include "mesh-pb-constants.h"
#include "meshUtils.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/// Meshtastic hop fields are 3 bits; a nonzero hop_start keeps clients from showing '?'.
static constexpr uint8_t HOP_START = 7;

MeshtasticPathCommand meshtasticPathCommand;

/// snprintf into a bounded buffer; a token that does not fit is rolled back instead of splitting UTF-8.
struct TextBuf {
    char *out;
    size_t cap;
    size_t len = 0;

    bool addf(const char *fmt, ...)
    {
        va_list ap;
        va_start(ap, fmt);
        const size_t room = cap - len;
        const int n = room > 1 ? vsnprintf(out + len, room, fmt, ap) : -1;
        va_end(ap);
        if (n < 0 || (size_t)n >= room) {
            out[len] = 0;
            return false;
        }
        len += (size_t)n;
        return true;
    }
};

static const char *senderName(uint32_t nodeNum)
{
    if (!nodeDB || !nodeNum)
        return nullptr;
    const meshtastic_NodeInfoLite *n = nodeDB->getMeshNode(nodeNum);
    return n && nodeInfoLiteHasUser(n) && n->long_name[0] ? n->long_name : nullptr;
}

/// The name behind a relay byte, or false when none or several nodes claim it.
static bool relayName(uint8_t relay, char *out, size_t cap)
{
    if (!nodeDB || relay == NO_RELAY_NODE || !out || cap == 0)
        return false;
    NodeNum num = 0;
    if (!nodeDB->resolveUniqueLastByte(relay, false, &num) || !num)
        return false;
    const meshtastic_NodeInfoLite *n = nodeDB->getMeshNode(num);
    if (!n || !nodeInfoLiteHasUser(n))
        return false;
    if (cleanPathLabel(n->long_name, out, cap, cap - 1) == 0)
        return false;
    stripMeshCoreSuffix(out);
    return out[0] != 0;
}

void MeshtasticPathCommand::reset()
{
    for (Heard &h : heard)
        h = Heard();
    heardNext = 0;
    for (uint32_t &id : injectedIds)
        id = 0;
    injectedNext = 0;
}

/// Copy the route of one copy of a text into a ring entry, so a duplicate describes the copy last heard.
static void fillHeard(MeshtasticPathCommand::Heard &h, const meshtastic_MeshPacket &p)
{
    h.packetId = p.id;
    h.from = p.from;
    h.heardMs = Time::getMillis();
    h.snr = p.rx_snr;
    h.rssi = p.rx_rssi;
    h.hasRssi = p.has_rx_rssi;
    h.channel = p.channel;
    h.hops = getHopsAway(p);
    h.relay = (uint8_t)(p.relay_node & 0xFF);
    h.viaMqtt = p.via_mqtt;
    h.direct = !isBroadcast(p.to);
}

bool MeshtasticPathCommand::wasInjected(uint32_t packetId) const
{
    for (uint32_t id : injectedIds)
        if (id && id == packetId)
            return true;
    return false;
}

void MeshtasticPathCommand::rememberInjected(uint32_t packetId)
{
    injectedIds[injectedNext] = packetId;
    injectedNext = (injectedNext + 1) % REPORT_MEMORY;
}

void MeshtasticPathCommand::noteHeard(const meshtastic_MeshPacket &p)
{
    if (!nodeDB)
        return;
    if (p.which_payload_variant != meshtastic_MeshPacket_decoded_tag || p.decoded.portnum != meshtastic_PortNum_TEXT_MESSAGE_APP)
        return;
    if (isFromUs(&p) || p.from == REPORT_NODE_NUM || wasInjected(p.id))
        return; // our own loopback, a report we injected, or a report's own copy
    if (channels.isMeshCoreChannel((ChannelIndex)p.channel))
        return; // MeshCoreCompanion keeps the routes of its own texts

    // A duplicate copy may still reach here when a filter is disabled: it refreshes the route we report on.
    for (Heard &h : heard) {
        if (h.packetId && h.packetId == p.id && h.from == p.from) {
            fillHeard(h, p);
            return;
        }
    }

    Heard &h = heard[heardNext];
    heardNext = (heardNext + 1) % MEMORY;
    h = Heard();
    fillHeard(h, p);
}

const MeshtasticPathCommand::Heard *MeshtasticPathCommand::find(uint32_t packetId) const
{
    if (!packetId)
        return nullptr;
    for (const Heard &h : heard)
        if (h.packetId == packetId)
            return &h;
    return nullptr;
}

size_t MeshtasticPathCommand::formatReport(const Heard &e, char *out, size_t cap) const
{
    if (!out || cap == 0)
        return 0;
    out[0] = 0;
    TextBuf msg{out, cap};
    char line[160];

    // Sender. Bold only when there is a name: an absent one must not leave markers behind.
    {
        TextBuf t{line, sizeof(line)};
        t.addf("🛰️ Ruta");
        char label[48];
        if (cleanPathLabel(senderName(e.from), label, sizeof(label), sizeof(label) - 1) > 0) {
            stripMeshCoreSuffix(label);
            if (label[0])
                t.addf(" de **%s**", label);
        }
        msg.addf("%s", line);
    }

    // Route. The glyph is the hop count, so the text does not repeat it.
    {
        TextBuf t{line, sizeof(line)};
        char label[48];
        if (e.hops == 0) {
            t.addf("⚡ directo");
        } else if (e.hops < 0) {
            t.addf("❔ saltos desconocidos");
            if (relayName(e.relay, label, sizeof(label)))
                t.addf(" · probablemente **%s**", label);
            else if (e.relay != NO_RELAY_NODE)
                t.addf(" · `%02x`", (unsigned)e.relay);
        } else {
            if (const char *glyph = hopGlyph(e.hops))
                t.addf("%s ", glyph);
            else
                t.addf("%u saltos ", (unsigned)e.hops);
            // One name claims the byte only among the nodes we know: the relay may be another one, so it is a guess.
            if (relayName(e.relay, label, sizeof(label)))
                t.addf("probablemente **%s**", label);
            else if (e.relay != NO_RELAY_NODE)
                t.addf("`%02x`", (unsigned)e.relay);
            else
                t.addf("repetidor desconocido");
        }
        t.addf(" · SNR %.1f dB", (double)e.snr);
        if (e.hasRssi)
            t.addf(" · RSSI %d dBm", (int)e.rssi);
        msg.addf("\n%s", line);
    }

    // Where it was heard.
    {
        TextBuf t{line, sizeof(line)};
        if (e.direct) {
            t.addf("Mensaje directo");
        } else {
            char label[48];
            if (cleanPathLabel(channels.getName(e.channel), label, sizeof(label), sizeof(label) - 1) > 0)
                t.addf("Canal **%s**", label);
            else
                t.addf("Canal %u", (unsigned)e.channel);
        }
        if (e.viaMqtt)
            t.addf(" · vía MQTT");
        t.addf(" · hace %u s", (unsigned)((Time::getMillis() - e.heardMs) / 1000));
        msg.addf("\n%s", line);
    }

    // The one thing Meshtastic cannot tell us. Dropped whole rather than cut when the payload is full.
    msg.addf("\nMeshtastic no da los saltos intermedios.");

    sanitizeUtf8(out, cap);
    return strlen(out);
}

void MeshtasticPathCommand::reportDelivery(uint32_t fromNum, uint32_t packetId, uint8_t channel)
{
    if (!router || !nodeDB)
        return;
    meshtastic_Routing r = meshtastic_Routing_init_default;
    r.which_variant = meshtastic_Routing_error_reason_tag;
    r.error_reason = meshtastic_Routing_Error_NONE;

    meshtastic_MeshPacket *p = packetPool.allocZeroed();
    if (!p)
        return;
    p->from = fromNum;
    p->to = nodeDB->getNodeNum();
    p->id = generatePacketId();
    p->channel = channel;
    p->priority = meshtastic_MeshPacket_Priority_ACK;
    p->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p->decoded.portnum = meshtastic_PortNum_ROUTING_APP;
    p->decoded.request_id = packetId;
    p->decoded.payload.size =
        pb_encode_to_bytes(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), &meshtastic_Routing_msg, &r);
    // The router drops a packet whose hop_start is missing, so the delivery report carries real hop fields (direct).
    p->decoded.has_bitfield = true;
    p->decoded.bitfield = 0;
    p->hop_start = HOP_START;
    p->hop_limit = HOP_START;
    p->transport_mechanism = meshtastic_MeshPacket_TransportMechanism_TRANSPORT_LORA;
    router->enqueueReceivedMessage(p);
}

void MeshtasticPathCommand::ensureReportNode()
{
    if (!router || !nodeDB)
        return;
    const meshtastic_NodeInfoLite *known = nodeDB->getMeshNode(REPORT_NODE_NUM);
    if (known && nodeInfoLiteHasUser(known) && known->long_name[0])
        return;

    meshtastic_User user = {};
    strncpy(user.long_name, "Ruta", sizeof(user.long_name) - 1);
    strncpy(user.short_name, "RT", sizeof(user.short_name) - 1);
    user.hw_model = meshtastic_HardwareModel_UNSET;
    user.role = meshtastic_Config_DeviceConfig_Role_CLIENT;
    user.has_is_unmessagable = true;
    user.is_unmessagable = true;

    meshtastic_MeshPacket *p = packetPool.allocZeroed();
    if (!p)
        return;
    p->from = REPORT_NODE_NUM;
    p->to = NODENUM_BROADCAST;
    p->id = generatePacketId();
    p->channel = 0;
    p->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p->decoded.portnum = meshtastic_PortNum_NODEINFO_APP;
    p->decoded.payload.size =
        pb_encode_to_bytes(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), &meshtastic_User_msg, &user);
    // Local news for the client, never repeated: a next hop that is not us closes the relay gate.
    p->decoded.has_bitfield = true;
    p->decoded.bitfield = 0;
    p->hop_start = HOP_START;
    p->hop_limit = HOP_START - 1;
    p->next_hop = (uint8_t)(nodeDB->getLastByteOfNodeNum(nodeDB->getNodeNum()) + 1);
    if (p->next_hop == NO_NEXT_HOP_PREFERENCE)
        p->next_hop = 1;
    p->transport_mechanism = meshtastic_MeshPacket_TransportMechanism_TRANSPORT_LORA;
    router->enqueueReceivedMessage(p);
}

void MeshtasticPathCommand::injectReport(uint32_t from, uint32_t to, uint8_t channel, const char *text, uint32_t replyId,
                                         bool direct, const Heard *entry)
{
    if (!router || !nodeDB)
        return;
    meshtastic_MeshPacket *p = packetPool.allocZeroed();
    if (!p)
        return;
    p->from = from;
    p->to = to;
    p->id = generatePacketId();
    rememberInjected(p->id);
    p->channel = channel;
    p->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    if (replyId)
        p->decoded.reply_id = replyId;
    const size_t len = strlen(text);
    p->decoded.payload.size = len <= sizeof(p->decoded.payload.bytes) ? len : sizeof(p->decoded.payload.bytes);
    memcpy(p->decoded.payload.bytes, text, p->decoded.payload.size);
    p->decoded.has_bitfield = true;
    p->decoded.bitfield = 0;

    // The hop fields describe the reported text; a next hop that is not us keeps this local frame off the air.
    // An unknown hop count is sent as none: hop_start must stay set or PREHOP_DROP discards the frame.
    const uint8_t hops = entry && entry->hops > 0 ? (uint8_t)entry->hops : 0;
    p->hop_start = HOP_START;
    p->hop_limit = hops < HOP_START ? HOP_START - hops : 0;
    p->next_hop = (uint8_t)(nodeDB->getLastByteOfNodeNum(nodeDB->getNodeNum()) + 1);
    if (p->next_hop == NO_NEXT_HOP_PREFERENCE)
        p->next_hop = 1;
    p->transport_mechanism = meshtastic_MeshPacket_TransportMechanism_TRANSPORT_LORA;

    if (!direct) {
        // Clients show signal strength only for a direct frame; a relayed one shows its hop count.
        // Signal is attached only when the text was measured as direct: an unknown route must not look like one.
        if (entry && entry->hops == 0) {
            p->rx_snr = entry->snr;
            p->has_rx_rssi = true;
            p->rx_rssi = entry->rssi;
        }
    } else {
        // Clients file a direct text in the conversation with its sender, and only offer that for a keyed node.
        const meshtastic_NodeInfoLite *peer = nodeDB->getMeshNode(from);
        if (peer && peer->public_key.size == 32) {
            p->pki_encrypted = true;
            p->public_key.size = 32;
            memcpy(p->public_key.bytes, peer->public_key.bytes, 32);
        }
    }
    router->enqueueReceivedMessage(p);
}

bool MeshtasticPathCommand::handleCommand(const meshtastic_MeshPacket *p)
{
    if (!p || !nodeDB || p->which_payload_variant != meshtastic_MeshPacket_decoded_tag ||
        p->decoded.portnum != meshtastic_PortNum_TEXT_MESSAGE_APP)
        return false;

    char text[sizeof(p->decoded.payload.bytes) + 1];
    const size_t len =
        p->decoded.payload.size < sizeof(p->decoded.payload.bytes) ? p->decoded.payload.size : sizeof(p->decoded.payload.bytes);
    memcpy(text, p->decoded.payload.bytes, len);
    text[len] = 0;
    trimInPlace(text);
    if (strcasecmp(text, ".path") != 0 && strcasecmp(text, ".ruta") != 0)
        return false;
    // Only a reply is answered: a plain ".path" is an ordinary text and goes out.
    if (!p->decoded.reply_id)
        return false;

    const bool direct = !isBroadcast(p->to);
    const Heard *entry = find(p->decoded.reply_id);

    // Never on air: mark the command delivered and answer it in the same channel / chat.
    reportDelivery(direct ? p->to : REPORT_NODE_NUM, p->id, p->channel);
    if (!direct)
        ensureReportNode();

    // The command text is no longer needed, so the report reuses its buffer instead of a second stack copy.
    if (entry)
        formatReport(*entry, text, sizeof(text));
    else
        snprintf(text, sizeof(text), "🛰️ *Sin datos*: no he visto la ruta de ese mensaje.");
    sanitizeUtf8(text, sizeof(text));

    injectReport(direct ? p->to : REPORT_NODE_NUM, direct ? nodeDB->getNodeNum() : NODENUM_BROADCAST, direct ? 0 : p->channel,
                 text, entry ? entry->packetId : p->id, direct, entry);
    LOG_INFO("Meshtastic path command answered locally (direct=%d, heard=%d)", direct, entry != nullptr);
    return true;
}

#endif // MESHTASTIC_PATH_COMMAND
