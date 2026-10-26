#pragma once

#include "configuration.h"

#if MESHTASTIC_PATH_COMMAND

#include "MeshTypes.h"
#include "mesh/generated/meshtastic/mesh.pb.h"
#include <stddef.h>
#include <stdint.h>

/**
 * Answers a local ".path"/".ruta" sent as a reply to a text on a Meshtastic channel or direct message.
 *
 * Meshtastic frames carry no per-hop hashes, so a report cannot name the whole path: it shows the hop count, the
 * node that relayed the copy we heard and the signal it arrived with. The last texts are kept in RAM for this.
 */
class MeshtasticPathCommand
{
  public:
    /// Texts remembered: one entry per text, the first copy heard.
    static constexpr size_t MEMORY = 8;

    /// A text as heard, kept so ".path" can describe the route it came by.
    struct Heard {
        uint32_t packetId = 0; // the id the client sees; a reply points at it
        uint32_t from = 0;
        uint32_t heardMs = 0;
        float snr = 0;
        int32_t rssi = 0;
        uint8_t channel = 0;
        int8_t hops = -1;  // -1 when the sender did not report hop_start
        uint8_t relay = 0; // last byte of the node that handed it to us, 0 when none
        bool hasRssi = false;
        bool viaMqtt = false;
        bool direct = false;
    };

    /// Node number a channel report is signed with, so it does not look like a text the user sent: the system bot.
    static constexpr uint32_t REPORT_NODE_NUM = NODENUM_SYSTEM_BOT;
    /// The "Ruta" node earlier builds signed reports with. Dropped from the NodeDB at boot so it stops taking a slot.
    static constexpr uint32_t LEGACY_REPORT_NODE_NUM = 0x52555441; // "RUTA"

    /// Record a heard text. Remote Meshtastic texts only; MeshCore channels keep their own routes.
    void noteHeard(const meshtastic_MeshPacket &p);

    /// Answer a local ".path"/".ruta" the client sent: true when it was answered here and must not be transmitted.
    bool handleCommand(const meshtastic_MeshPacket *p);

    /// Render the report for one heard text into out (NUL-terminated, never splits UTF-8). Public for tests.
    size_t formatReport(const Heard &entry, char *out, size_t cap) const;

    /// Forget every heard text (tests).
    void reset();

  private:
    /// Reports injected recently: the router hands each one back, and none is a heard text.
    static constexpr size_t REPORT_MEMORY = 4;

    Heard heard[MEMORY];
    size_t heardNext = 0;
    uint32_t injectedIds[REPORT_MEMORY] = {};
    size_t injectedNext = 0;

    const Heard *find(uint32_t packetId) const;

    bool wasInjected(uint32_t packetId) const;
    void rememberInjected(uint32_t packetId);

    /// Tell the client the command went through, as the mesh ACK it will never get would have.
    void reportDelivery(uint32_t fromNum, uint32_t packetId, uint8_t channel);
    /// Announce the "Ruta" node once, so the client has a name to render the channel report with.
    void ensureReportNode();
    /// Inject the report as a text from the reported sender (direct) or from REPORT_NODE_NUM (channel).
    void injectReport(uint32_t from, uint32_t to, uint8_t channel, const char *text, uint32_t replyId, bool direct,
                      const Heard *entry);
};

extern MeshtasticPathCommand meshtasticPathCommand;

#endif // MESHTASTIC_PATH_COMMAND
