// Unit tests for src/mesh/MeshtasticPathCommand.{h,cpp} - the local ".path" answered on a Meshtastic channel or DM.
//
// Meshtastic frames carry no per-hop hashes, so there is no full path to report. What must hold is that the command is
// answered from RAM (never transmitted), that the report describes the text it answers with the hops, last relay and
// signal we actually measured, and that nothing it injects can be mistaken for a heard text or relayed on air.
#include "Arduino.h"
#include "TestUtil.h"
#include <unity.h>

#include "Channels.h"
#include "CryptoEngine.h"
#include "NodeDB.h"
#include "Router.h"
#include "UptimeClock.h"
#include "mesh/MeshtasticPathCommand.h"
#include "meshUtils.h"
#include <string.h>
#include <vector>

class CaptureRouter : public Router
{
  public:
    ~CaptureRouter()
    {
        delete cryptLock;
        cryptLock = nullptr;
    }

    void enqueueReceivedMessage(meshtastic_MeshPacket *p) override
    {
        received.push_back(*p);
        packetPool.release(p);
    }

    std::vector<meshtastic_MeshPacket> received;
};

// Swaps in a NodeDB with a named primary channel and a router that only records what the command injects.
// Built in setUp/tearDown, not in a fixture: Unity longjmps out of a failed test, which would skip a stack
// fixture's destructor and leave the router registered with the thread controller.
static CaptureRouter *capture = nullptr;
static NodeDB *previousNodeDB = nullptr;
static Router *previousRouter = nullptr;
static meshtastic_Channel previousChannel = {};
static meshtastic_Channel previousChannel1 = {};

static void addNode(uint32_t num, const char *name)
{
    meshtastic_NodeInfoLite *n = nodeDB->getOrCreateMeshNode(num);
    TEST_ASSERT_NOT_NULL(n);
    n->bitfield |= NODEINFO_BITFIELD_HAS_USER_MASK;
    strncpy(n->long_name, name, sizeof(n->long_name) - 1);
    n->long_name[sizeof(n->long_name) - 1] = 0;
}

// A relay byte only resolves for a plausible relay: a direct neighbor, a favorite or a router.
static void addRelay(uint32_t num, const char *name)
{
    addNode(num, name);
    meshtastic_NodeInfoLite *n = nodeDB->getMeshNode(num);
    n->has_hops_away = true;
    n->hops_away = 0;
}

// A text as the router would hand it over after decoding: hops, signal and relay byte as measured on air.
static meshtastic_MeshPacket heardText(uint32_t from, uint8_t channel, uint32_t id, int hops, uint8_t relay, float snr,
                                       int32_t rssi, bool direct = false, uint32_t to = NODENUM_BROADCAST)
{
    meshtastic_MeshPacket p = {};
    p.from = from;
    p.to = to;
    p.id = id;
    p.channel = channel;
    p.hop_start = 7;
    p.hop_limit = (uint8_t)(7 - hops);
    p.relay_node = relay;
    p.rx_snr = snr;
    p.has_rx_rssi = true;
    p.rx_rssi = rssi;
    p.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    p.decoded.has_bitfield = true;
    const char *text = "hola";
    p.decoded.payload.size = strlen(text);
    memcpy(p.decoded.payload.bytes, text, p.decoded.payload.size);
    return p;
}

static meshtastic_MeshPacket command_(uint8_t channel, uint32_t to, uint32_t replyId, const char *text)
{
    meshtastic_MeshPacket p = {};
    p.id = 0xCAFE;
    p.from = 0;
    p.to = to;
    p.channel = channel;
    p.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    p.decoded.reply_id = replyId;
    p.decoded.payload.size = strlen(text);
    memcpy(p.decoded.payload.bytes, text, p.decoded.payload.size);
    return p;
}

static const char *bodyOf(const meshtastic_MeshPacket &p, char *buf, size_t cap)
{
    const size_t n = p.decoded.payload.size < cap - 1 ? p.decoded.payload.size : cap - 1;
    memcpy(buf, p.decoded.payload.bytes, n);
    buf[n] = 0;
    return buf;
}

static bool sawDelivery(const CaptureRouter &capture, uint32_t commandId)
{
    for (const meshtastic_MeshPacket &q : capture.received)
        if (q.which_payload_variant == meshtastic_MeshPacket_decoded_tag && q.decoded.portnum == meshtastic_PortNum_ROUTING_APP &&
            q.decoded.request_id == commandId)
            return true;
    return false;
}

// The command is answered from RAM with the sender, hop count, last relay and signal of the text it answers.
void test_answers_locally_with_route_info(void)
{
    addNode(0x00001111, "Ana");
    addRelay(0x00002222, "Repetidor X");

    meshtasticPathCommand.noteHeard(heardText(0x00001111, 0, 0x00ABC123, 2, 0x22, 6.5f, -95));

    meshtastic_MeshPacket cmd = command_(0, NODENUM_BROADCAST, 0x00ABC123, ".path");
    const size_t before = capture->received.size();
    TEST_ASSERT_TRUE(meshtasticPathCommand.handleCommand(&cmd));

    TEST_ASSERT_TRUE(sawDelivery(*capture, cmd.id));
    TEST_ASSERT_TRUE(capture->received.size() > before);

    bool announced = false;
    for (size_t i = before; i < capture->received.size(); i++)
        if (capture->received[i].from == MeshtasticPathCommand::REPORT_NODE_NUM &&
            capture->received[i].decoded.portnum == meshtastic_PortNum_NODEINFO_APP)
            announced = true;
    TEST_ASSERT_TRUE(announced);

    const meshtastic_MeshPacket &report = capture->received.back();
    TEST_ASSERT_EQUAL(meshtastic_PortNum_TEXT_MESSAGE_APP, report.decoded.portnum);
    TEST_ASSERT_EQUAL_UINT32(MeshtasticPathCommand::REPORT_NODE_NUM, report.from);
    TEST_ASSERT_EQUAL_UINT32(NODENUM_BROADCAST, report.to);
    TEST_ASSERT_EQUAL_UINT32(0x00ABC123, report.decoded.reply_id);
    TEST_ASSERT_EQUAL_UINT8(0, report.channel);

    char body[256];
    bodyOf(report, body, sizeof(body));
    TEST_ASSERT_NOT_NULL(strstr(body, "🛰️"));
    TEST_ASSERT_NOT_NULL(strstr(body, "de **Ana**"));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(body, "2️⃣"), body); // the glyph is the hop count
    TEST_ASSERT_NOT_NULL(strstr(body, "probablemente **Repetidor X**"));
    TEST_ASSERT_NOT_NULL(strstr(body, "SNR 6.5 dB"));
    TEST_ASSERT_NOT_NULL(strstr(body, "RSSI -95 dBm"));
    TEST_ASSERT_NOT_NULL(strstr(body, "Canal **LongFast**"));
}

// The local frame must never be relayed: a real hop_start, and a next hop that is not us.
void test_injected_report_cannot_be_relayed(void)
{
    addNode(0x00001111, "Ana");
    meshtasticPathCommand.noteHeard(heardText(0x00001111, 0, 0x00ABC124, 1, 0, -5.0f, -80));

    meshtastic_MeshPacket cmd = command_(0, NODENUM_BROADCAST, 0x00ABC124, ".path");
    TEST_ASSERT_TRUE(meshtasticPathCommand.handleCommand(&cmd));

    const uint8_t ourRelay = nodeDB->getLastByteOfNodeNum(nodeDB->getNodeNum());
    for (const meshtastic_MeshPacket &q : capture->received) {
        if (q.to != NODENUM_BROADCAST)
            continue; // a delivery report is addressed to us and cannot be relayed anyway
        TEST_ASSERT_NOT_EQUAL_UINT8(0, q.hop_start);
        TEST_ASSERT_NOT_EQUAL_UINT8(NO_NEXT_HOP_PREFERENCE, q.next_hop);
        TEST_ASSERT_NOT_EQUAL_UINT8(ourRelay, q.next_hop);
    }
}

// Only a reply is answered: without a reply target the ".path" is an ordinary text, not intercepted or injected.
void test_plain_path_is_not_intercepted(void)
{
    addNode(0x00001111, "Ana");
    meshtasticPathCommand.noteHeard(heardText(0x00001111, 0, 0x00ABC200, 1, 0, -5.0f, -80));

    meshtastic_MeshPacket cmd = command_(0, NODENUM_BROADCAST, 0, ".path");
    TEST_ASSERT_FALSE(meshtasticPathCommand.handleCommand(&cmd));
    TEST_ASSERT_TRUE(capture->received.empty());
}

// Nothing heard yet: a short local notice, and the command still does not go out.
void test_without_data_is_a_hint(void)
{
    meshtastic_MeshPacket cmd = command_(0, NODENUM_BROADCAST, 0x00ABC999, ".ruta");
    TEST_ASSERT_TRUE(meshtasticPathCommand.handleCommand(&cmd));

    char body[256];
    bodyOf(capture->received.back(), body, sizeof(body));
    TEST_ASSERT_NOT_NULL(strstr(body, "Sin datos"));
    TEST_ASSERT_FALSE(capture->received.back().has_rx_rssi);
}

// Anything that is not exactly the command is left to the normal send path.
void test_ignores_other_text(void)
{
    meshtastic_MeshPacket dotted = command_(0, NODENUM_BROADCAST, 0, ".paths");
    TEST_ASSERT_FALSE(meshtasticPathCommand.handleCommand(&dotted));
    meshtastic_MeshPacket bare = command_(0, NODENUM_BROADCAST, 0, "path");
    TEST_ASSERT_FALSE(meshtasticPathCommand.handleCommand(&bare));
    meshtastic_MeshPacket withArgs = command_(0, NODENUM_BROADCAST, 0, ".path foo");
    TEST_ASSERT_FALSE(meshtasticPathCommand.handleCommand(&withArgs));
    TEST_ASSERT_TRUE(capture->received.empty());
}

// A relay no node claims is shown as hex, not as a guess.
void test_unknown_relay_is_hex(void)
{
    addNode(0x00001111, "Ana");
    meshtasticPathCommand.noteHeard(heardText(0x00001111, 0, 0x00ABC300, 1, 0x77, -5.0f, -80));

    meshtastic_MeshPacket cmd = command_(0, NODENUM_BROADCAST, 0x00ABC300, ".path");
    TEST_ASSERT_TRUE(meshtasticPathCommand.handleCommand(&cmd));

    char body[256];
    bodyOf(capture->received.back(), body, sizeof(body));
    TEST_ASSERT_NOT_NULL(strstr(body, "`77`"));
}

// hop_start == 0 from pre-2.5 firmware is unknown, not direct.
void test_unknown_hops_are_not_reported_as_direct(void)
{
    addNode(0x00001111, "Ana");
    meshtastic_MeshPacket heard = heardText(0x00001111, 0, 0x00ABC400, 0, 0, -5.0f, -80);
    heard.hop_start = 0;
    heard.decoded.has_bitfield = false;
    meshtasticPathCommand.noteHeard(heard);

    meshtastic_MeshPacket cmd = command_(0, NODENUM_BROADCAST, 0x00ABC400, ".path");
    TEST_ASSERT_TRUE(meshtasticPathCommand.handleCommand(&cmd));

    char body[256];
    bodyOf(capture->received.back(), body, sizeof(body));
    TEST_ASSERT_NOT_NULL(strstr(body, "saltos desconocidos"));
    TEST_ASSERT_NULL(strstr(body, "directo"));
    // The local frame carries no signal either: it would otherwise pass for a direct reception.
    TEST_ASSERT_FALSE(capture->received.back().has_rx_rssi);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, capture->received.back().rx_snr);
}

// A direct message is answered in the same chat, from the peer and with its key, as a PKI reply.
void test_answers_a_direct_message(void)
{
    const uint32_t peer = 0x00003333;
    addNode(peer, "Bea");
    meshtastic_NodeInfoLite *bea = nodeDB->getMeshNode(peer);
    bea->public_key.size = 32;
    for (size_t i = 0; i < 32; i++)
        bea->public_key.bytes[i] = (uint8_t)(0x40 + i);

    meshtasticPathCommand.noteHeard(heardText(peer, 0, 0x00ABC500, 1, 0, -7.5f, -95, true, nodeDB->getNodeNum()));

    meshtastic_MeshPacket cmd = command_(0, peer, 0x00ABC500, ".path");
    TEST_ASSERT_TRUE(meshtasticPathCommand.handleCommand(&cmd));

    const meshtastic_MeshPacket &report = capture->received.back();
    TEST_ASSERT_EQUAL_UINT32(peer, report.from);
    TEST_ASSERT_EQUAL_UINT32(nodeDB->getNodeNum(), report.to);
    TEST_ASSERT_TRUE(report.pki_encrypted);
    TEST_ASSERT_EQUAL_MEMORY(bea->public_key.bytes, report.public_key.bytes, 32);
    TEST_ASSERT_EQUAL_UINT32(0x00ABC500, report.decoded.reply_id);

    char body[256];
    bodyOf(report, body, sizeof(body));
    TEST_ASSERT_NOT_NULL(strstr(body, "Mensaje directo"));
    TEST_ASSERT_NOT_NULL(strstr(body, "SNR -7.5 dB"));
}

// MeshCore channels keep their own routes, our own texts and our own reports are not recorded.
void test_records_only_remote_meshtastic_texts(void)
{
    addNode(0x00001111, "Ana");

    meshtastic_Channel &mc = channels.getByIndex(1);
    mc = meshtastic_Channel();
    mc.has_settings = true;
    mc.role = meshtastic_Channel_Role_SECONDARY;
    mc.settings.psk.size = 16; // a MeshCore channel needs its 16-byte key as well as the "MC:" name
    strncpy(mc.settings.name, "MC:Public", sizeof(mc.settings.name) - 1);

    meshtasticPathCommand.noteHeard(heardText(0x00001111, 1, 0x00ABC600, 1, 0, -5.0f, -80));
    meshtasticPathCommand.noteHeard(heardText(nodeDB->getNodeNum(), 0, 0x00ABC601, 1, 0, -5.0f, -80));
    meshtasticPathCommand.noteHeard(heardText(MeshtasticPathCommand::REPORT_NODE_NUM, 0, 0x00ABC602, 1, 0, -5.0f, -80));

    // Own, MeshCore and report texts are not heard, so replying to them finds nothing.
    const uint32_t notHeard[] = {0x00ABC600, 0x00ABC601, 0x00ABC602};
    for (uint32_t id : notHeard) {
        meshtastic_MeshPacket cmd = command_(0, NODENUM_BROADCAST, id, ".path");
        TEST_ASSERT_TRUE(meshtasticPathCommand.handleCommand(&cmd));
        char body[256];
        bodyOf(capture->received.back(), body, sizeof(body));
        TEST_ASSERT_NOT_NULL(strstr(body, "Sin datos"));
    }
}

// Every report injected is remembered, not just the last one. A direct report comes back from the peer's own number,
// so only the id ring keeps it out of the heard texts and out of the next command's fallback.
void test_own_reports_are_not_heard(void)
{
    const uint32_t peer = 0x00004545;
    addNode(peer, "Ana");
    meshtasticPathCommand.noteHeard(heardText(peer, 0, 0x00ABC700, 1, 0, -5.0f, -80, true, nodeDB->getNodeNum()));

    meshtastic_MeshPacket first = command_(0, peer, 0x00ABC700, ".path");
    TEST_ASSERT_TRUE(meshtasticPathCommand.handleCommand(&first));
    const meshtastic_MeshPacket firstReport = capture->received.back();

    meshtastic_MeshPacket second = command_(0, peer, 0x00ABC700, ".path");
    TEST_ASSERT_TRUE(meshtasticPathCommand.handleCommand(&second));
    const meshtastic_MeshPacket secondReport = capture->received.back();

    TEST_ASSERT_EQUAL_UINT32(peer, firstReport.from);
    meshtasticPathCommand.noteHeard(firstReport);
    meshtasticPathCommand.noteHeard(secondReport);

    meshtastic_MeshPacket again = command_(0, peer, 0x00ABC700, ".path");
    TEST_ASSERT_TRUE(meshtasticPathCommand.handleCommand(&again));
    TEST_ASSERT_EQUAL_UINT32(0x00ABC700, capture->received.back().decoded.reply_id);
}

// The ring keeps the newest texts only: a reply to an evicted text finds nothing, a reply to the newest one is answered.
void test_ring_keeps_only_the_newest_texts(void)
{
    addNode(0x00001111, "Ana");
    for (uint32_t i = 0; i < MeshtasticPathCommand::MEMORY + 1; i++)
        meshtasticPathCommand.noteHeard(heardText(0x00001111, 0, 0x00ABD000 + i, 1, 0, -5.0f, -80));

    meshtastic_MeshPacket evicted = command_(0, NODENUM_BROADCAST, 0x00ABD000, ".path");
    TEST_ASSERT_TRUE(meshtasticPathCommand.handleCommand(&evicted));
    char body[256];
    bodyOf(capture->received.back(), body, sizeof(body));
    TEST_ASSERT_NOT_NULL(strstr(body, "Sin datos"));

    meshtastic_MeshPacket newest = command_(0, NODENUM_BROADCAST, 0x00ABD000 + MeshtasticPathCommand::MEMORY, ".path");
    TEST_ASSERT_TRUE(meshtasticPathCommand.handleCommand(&newest));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(0x00ABD000 + MeshtasticPathCommand::MEMORY), capture->received.back().decoded.reply_id);
}

// Without a router the answer has nowhere to go: the command is still swallowed, never transmitted.
void test_without_a_router_injects_nothing(void)
{
    router = nullptr;
    addNode(0x00001111, "Ana");
    meshtasticPathCommand.noteHeard(heardText(0x00001111, 0, 0x00ABD100, 1, 0, -5.0f, -80));

    meshtastic_MeshPacket cmd = command_(0, NODENUM_BROADCAST, 0x00ABD100, ".path");
    TEST_ASSERT_TRUE(meshtasticPathCommand.handleCommand(&cmd));
    TEST_ASSERT_TRUE(capture->received.empty());
}

// Names are reported whole, the " (MC)" suffix is dropped, and every bold span closes inside the message.
void test_report_keeps_whole_names(void)
{
    // Numbers no other case in this suite uses: the per-suite sandbox keeps earlier tests' nodes, and a repeated
    // last byte from a stale copy would make the relay ambiguous.
    addNode(0x0000B711, "Remitente Largo (MC)");
    addRelay(0x0000B755, "Repetidor Muy Largo (MC)");
    meshtasticPathCommand.noteHeard(heardText(0x0000B711, 0, 0x00ABCA00, 3, 0x55, -5.0f, -80));

    meshtastic_MeshPacket cmd = command_(0, NODENUM_BROADCAST, 0x00ABCA00, ".path");
    TEST_ASSERT_TRUE(meshtasticPathCommand.handleCommand(&cmd));

    char body[256];
    bodyOf(capture->received.back(), body, sizeof(body));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(body, "de **Remitente Largo**"), body);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(body, "**Repetidor Muy Largo**"), body);
    TEST_ASSERT_NULL(strstr(body, "(MC)"));
    TEST_ASSERT_NOT_NULL(strstr(body, "3️⃣"));    // the glyph is the hop count
    TEST_ASSERT_NULL(strstr(body, "3 saltos")); // and the text does not repeat it

    int markers = 0;
    for (const char *p = body; (p = strstr(p, "**")) != nullptr; p += 2)
        markers++;
    TEST_ASSERT_EQUAL_INT(0, markers % 2);
}

// Names carrying markdown delimiters or control bytes cannot break the report's formatting.
void test_report_sanitizes_names(void)
{
    addNode(0x00001111, "Bad*Name\n[with] `tricks`");
    meshtasticPathCommand.noteHeard(heardText(0x00001111, 0, 0x00ABC800, 0, 0, -5.0f, -80));

    meshtastic_MeshPacket cmd = command_(0, NODENUM_BROADCAST, 0x00ABC800, ".path");
    TEST_ASSERT_TRUE(meshtasticPathCommand.handleCommand(&cmd));

    char body[256];
    bodyOf(capture->received.back(), body, sizeof(body));
    TEST_ASSERT_NULL(strstr(body, "Bad*"));
    TEST_ASSERT_NULL(strstr(body, "[with]"));
    TEST_ASSERT_NOT_NULL(strstr(body, "BadName"));
}

// Even the longest names keep the report inside one payload, valid UTF-8 and NUL-terminated.
void test_report_fits_one_payload(void)
{
    addNode(0x00001111, "Un nombre larguísimo con acentos y símbolos 🛰️");
    addRelay(0x00002222, "Otro nombre igual de largo para el repetidor");

    meshtastic_Channel &ch = channels.getByIndex(0);
    strncpy(ch.settings.name, "Un canal con un nombre larguísimo", sizeof(ch.settings.name) - 1);

    meshtasticPathCommand.noteHeard(heardText(0x00001111, 0, 0x00ABC900, 7, 0x22, -20.0f, -120));

    meshtastic_MeshPacket cmd = command_(0, NODENUM_BROADCAST, 0x00ABC900, ".path");
    TEST_ASSERT_TRUE(meshtasticPathCommand.handleCommand(&cmd));

    const meshtastic_MeshPacket &report = capture->received.back();
    TEST_ASSERT_LESS_OR_EQUAL_UINT(sizeof(report.decoded.payload.bytes), report.decoded.payload.size);

    char body[sizeof(report.decoded.payload.bytes) + 1];
    bodyOf(report, body, sizeof(body));
    char sanitized[sizeof(body)];
    strcpy(sanitized, body);
    TEST_ASSERT_FALSE(sanitizeUtf8(sanitized, sizeof(sanitized))); // already valid: nothing to replace
}

void setUp(void)
{
    Time::setTestMillis(1000);
    previousChannel = channels.getByIndex(0);
    previousChannel1 = channels.getByIndex(1);
    previousNodeDB = nodeDB;
    previousRouter = router;

    nodeDB = new NodeDB();
    capture = new CaptureRouter();
    router = capture;
    meshtastic_Channel &ch = channels.getByIndex(0);
    ch = meshtastic_Channel();
    ch.has_settings = true;
    ch.role = meshtastic_Channel_Role_PRIMARY;
    strncpy(ch.settings.name, "LongFast", sizeof(ch.settings.name) - 1);
    meshtasticPathCommand.reset();
}

void tearDown(void)
{
    meshtasticPathCommand.reset();
    channels.getByIndex(0) = previousChannel;
    channels.getByIndex(1) = previousChannel1;
    router = previousRouter;
    delete capture; // hands cryptLock back, so the next test can build a router
    capture = nullptr;
    delete nodeDB;
    nodeDB = previousNodeDB;
    Time::useRealClock();
}

void setup()
{
    initializeTestEnvironment();
    initRegion();
    UNITY_BEGIN();
    RUN_TEST(test_answers_locally_with_route_info);
    RUN_TEST(test_injected_report_cannot_be_relayed);
    RUN_TEST(test_plain_path_is_not_intercepted);
    RUN_TEST(test_without_data_is_a_hint);
    RUN_TEST(test_ignores_other_text);
    RUN_TEST(test_unknown_relay_is_hex);
    RUN_TEST(test_unknown_hops_are_not_reported_as_direct);
    RUN_TEST(test_answers_a_direct_message);
    RUN_TEST(test_records_only_remote_meshtastic_texts);
    RUN_TEST(test_own_reports_are_not_heard);
    RUN_TEST(test_ring_keeps_only_the_newest_texts);
    RUN_TEST(test_without_a_router_injects_nothing);
    RUN_TEST(test_report_keeps_whole_names);
    RUN_TEST(test_report_sanitizes_names);
    RUN_TEST(test_report_fits_one_payload);
    exit(UNITY_END());
}

void loop() {}
