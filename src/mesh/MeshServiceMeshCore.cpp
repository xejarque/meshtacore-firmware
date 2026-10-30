// MeshCore side of MeshService: the bridge from the client's texts to MeshCore, and the ".path" query. Kept out of
// MeshService.cpp so that file stays close to the jentastic upstream and a merge of origin/main does not collide with it.

#include "configuration.h"

#if !MESHTASTIC_EXCLUDE_GPS
#include "GPS.h"
#endif

#include "../concurrency/Periodic.h"
#include "BluetoothCommon.h" // needed for updateBatteryLevel, FIXME, eventually when we pull mesh out into a lib we shouldn't be whacking bluetooth from here
#include "MeshService.h"
#include "MessageStore.h"
#include "NodeDB.h"
#include "Power.h"
#include "PowerFSM.h"
#include "TypeConversions.h"
#include "UptimeClock.h"
#include "gps/GPSLog.h"
#include "gps/RTC.h"
#include "graphics/draw/MessageRenderer.h"
#include "main.h"
#include "mesh-pb-constants.h"
#include "meshUtils.h"
#include "modules/NexusModule.h"
#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
#include "MeshCoreCompanion.h"
#endif
#if MESHTASTIC_PATH_COMMAND
#include "MeshtasticPathCommand.h"
#endif
#include "modules/AdminModule.h"
#include "modules/NodeInfoModule.h"
#include "modules/PositionModule.h"
#include "modules/RoutingModule.h"
#include <assert.h>
#include <string>

#if ARCH_PORTDUINO
#include "PortduinoGlue.h"
#endif

#if MESHTASTIC_PATH_COMMAND
/// The network the replied-to text belongs to answers ".path": a MeshCore channel or contact through the Companion, any
/// other node through the Meshtastic command. Both answer only a reply, so a plain ".path" is sent as an ordinary text.
static bool answerPathCommand(const meshtastic_MeshPacket *p)
{
    if (p->which_payload_variant != meshtastic_MeshPacket_decoded_tag ||
        p->decoded.portnum != meshtastic_PortNum_TEXT_MESSAGE_APP)
        return false;
    const bool direct = !isBroadcast(p->to);
#if MESHTASTIC_MESHCORE_PATH_COMMAND
    if (direct ? meshCoreCompanion.hasContact(p->to) : channels.isMeshCoreChannel((ChannelIndex)p->channel))
        return meshCoreCompanion.handlePathCommand(p, direct) || meshCoreCompanion.handleTraceCommand(p, direct);
#endif
    return meshtasticPathCommand.handleCommand(p);
}
#endif

#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
bool MeshService::sendMeshCoreGroupText(const meshtastic_MeshPacket *p)
{

    uint8_t key[32];
    const size_t keyLen = channels.getMeshCoreKey(p->channel, key);

    // MeshCore timestamps are wall-clock: without a valid time peers would see 1970 or reject the message.
    const uint32_t now = getValidTime(RTCQualityFromNet);
    if (keyLen == 0 || now == 0) {
        LOG_WARN("MeshCore TX failed on ch=%u (keyLen=%u validTime=%d)", (unsigned)p->channel, (unsigned)keyLen, now != 0);
        return false;
    }

    // encodeGroupText cuts the text on a UTF-8 boundary once the "sender: " prefix is accounted for.
    char plain[sizeof(p->decoded.payload.bytes) + 1];
    memcpy(plain, p->decoded.payload.bytes, p->decoded.payload.size);
    plain[p->decoded.payload.size] = 0;
    char text[sizeof(plain) + 64]; // room for the "@[name] " of a reply and the brackets of mentions
    meshCoreCompanion.prepareGroupText(plain, p->decoded.reply_id, text, sizeof(text));

    const char *sender = owner.long_name[0] ? owner.long_name : (owner.short_name[0] ? owner.short_name : "Meshtastic");
    uint8_t frame[MeshCoreRepeater::MAX_FRAME_LEN];
    bool truncated = false;
    const size_t frameLen = MeshCoreCompanion::encodeGroupText(key, keyLen, sender, meshCoreCompanion.nextTxTimestamp(now), text,
                                                               frame, sizeof(frame), &truncated);
    RadioInterface *iface = router ? router->getRadioIface() : nullptr;
    if (frameLen == 0 || !iface || !iface->queueMeshCoreFrame(frame, frameLen)) {
        LOG_WARN("MeshCore TX failed on ch=%u (frameLen=%u)", (unsigned)p->channel, (unsigned)frameLen);
        return false;
    }
    if (truncated)
        LOG_WARN("MeshCore text cut to fit %u bytes with the sender prefix", (unsigned)MeshCoreCompanion::MAX_TEXT_LEN);
    meshCoreCompanion.trackGroupText(frame, frameLen, p->id, p->channel);
    LOG_INFO("MeshCore TX ch=%u len=%u", (unsigned)p->channel, (unsigned)frameLen);
    return true;
}
#endif

#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
bool MeshService::sendMeshCoreDirectText(const meshtastic_MeshPacket *p)
{

    // MeshCore timestamps are wall-clock: without a valid time the peer would reject or misorder the message.
    const uint32_t now = getValidTime(RTCQualityFromNet);
    char text[sizeof(p->decoded.payload.bytes) + 1];
    memcpy(text, p->decoded.payload.bytes, p->decoded.payload.size);
    text[p->decoded.payload.size] = 0;

    RadioInterface *iface = router ? router->getRadioIface() : nullptr;
    if (!meshCoreCompanion.sendDirectText(p->to, p->id, text, now, iface)) {
        LOG_WARN("MeshCore direct TX failed to=0x%08x (validTime=%d)", (unsigned)p->to, now != 0);
        return false;
    }
    LOG_INFO("MeshCore direct TX to=0x%08x id=%u", (unsigned)p->to, (unsigned)p->id);
    return true;
}
#endif

/**
 * Sends the packet a client handed to sendToMesh() to MeshCore, or answers it locally, when it is not Meshtastic traffic:
 * a ".path" query, a text for a MeshCore contact or a text on an "MC:" channel.
 * @return true when the packet was consumed (released to the pool, `result` set); false to carry on as Meshtastic.
 */
bool MeshService::consumeMeshCorePacket(meshtastic_MeshPacket *p, RxSource src, uint32_t mesh_packet_id, ErrorCode &result)
{
#if MESHTASTIC_PATH_COMMAND
    // A ".path" reply the client sends is answered from RAM by the network its text came from, never on air.
    if (src == RX_SRC_USER && answerPathCommand(p)) {
        releaseToPool(p);
        if (router) {
            meshtastic_QueueStatus qs = router->getQueueStatus();
            (void)sendQueueStatusToPhone(qs, ERRNO_OK, mesh_packet_id);
        }
        result = ERRNO_OK;
        return true;
    }
#endif

#if MESHTASTIC_ENABLE_MESHCORE_COMPANION
    // A text for a MeshCore contact leaves as a TXT_MSG. Only contacts we hold a key for: any other node number is a
    // Meshtastic node, whatever its top byte.
    if (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
        p->decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP && meshCoreCompanion.hasContact(p->to)) {
        const bool queued = sendMeshCoreDirectText(p);
        // Delivery is reported once a repeater re-floods it (relayed), then when the contact's ACK arrives (or times out).
        // Only a refusal is answered now.
        if (!queued && routingModule)
            routingModule->sendAckNak(meshtastic_Routing_Error_NO_INTERFACE, getFrom(p), p->id, p->channel, 0);
        if (router) {
            meshtastic_QueueStatus qs = router->getQueueStatus();
            (void)sendQueueStatusToPhone(qs, queued ? ERRNO_OK : ERRNO_UNKNOWN, mesh_packet_id);
        }
        releaseToPool(p);
        result = queued ? ERRNO_OK : ERRNO_UNKNOWN;
        return true;
    }

    if (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag && channels.isMeshCoreChannel(p->channel) && !isToUs(p)) {
        const bool isGroupText = p->decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP && isBroadcast(p->to);
        bool queued = false;
        if (!isGroupText) {
            // Anything but a group text would go out as Meshtastic traffic encrypted with the MeshCore key.
            LOG_WARN("Dropping port %u on MeshCore channel %u: only broadcast text is bridged", (unsigned)p->decoded.portnum,
                     (unsigned)p->channel);
        } else {
            queued = sendMeshCoreGroupText(p);
        }

        // MeshCore peers cannot ACK a channel text: it counts as delivered once a repeater is heard re-flooding it
        // (MeshCoreCompanion::settleEcho), or failed if none is. Only a refusal is answered now.
        if (isGroupText) {
            if (!queued && routingModule)
                routingModule->sendAckNak(meshtastic_Routing_Error_NO_INTERFACE, getFrom(p), p->id, p->channel, 0);
            if (router) {
                meshtastic_QueueStatus qs = router->getQueueStatus();
                (void)sendQueueStatusToPhone(qs, queued ? ERRNO_OK : ERRNO_UNKNOWN, mesh_packet_id);
            }
        }
        releaseToPool(p);
        result = queued ? ERRNO_OK : ERRNO_UNKNOWN;
        return true;
    }
#endif
    return false;
}
