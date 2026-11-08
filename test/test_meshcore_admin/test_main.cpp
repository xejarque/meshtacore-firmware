// Unit tests for src/mesh/MeshCoreAdmin.{h,cpp} and MeshCoreAdminFile.h - saved repeater/room passwords and the login
// + CLI exchange Nexus drives.
//
// Why it matters: a wrong login frame is silently ignored by the repeater, and a wrong PATH parsing means the command
// is never sent. Both look like an unreachable node from the operator's side. The file format also carries plaintext
// passwords across reboots, so a corrupt image must load nothing rather than half of itself.
//
// Regressions guarded: a login frame whose path_len/pubkey/HMAC/password layout differs from MeshCore's ANON_REQ; a
// login response read as a different payload type (PATH is 0x08, RESPONSE is the extra type 0x01); a CLI reply with
// the text type in the wrong bits; a command timeout that never retries (or retries forever); a persisted image that
// loses names, types or passwords; a target resolved by a prefix that matches more than one node.
#define MESHTASTIC_ENABLE_MESHCORE_SNIFFER 1
#define MESHTASTIC_ENABLE_MESHCORE_RELAY 0
#define MESHTASTIC_ENABLE_MESHCORE_COMPANION 1
#define MESHTASTIC_ENABLE_MESHCORE_ADMIN 1
#define MESHTASTIC_MESHCORE_PATH_COMMAND 0
#include "mesh/MeshCoreAdmin.cpp"
#include "mesh/MeshCoreCompanion.cpp"
#include "mesh/MeshCoreRepeater.cpp"

#include "Arduino.h"
#include "TestUtil.h"
#include "TypeConversions.h"
#include "UptimeClock.h"
#include "gps/RTC.h"
#include "modules/NexusModule.h"
#include <ctime>
#include <unity.h>

static const uint8_t kSeedA[32] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16,
                                   17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};
static const uint8_t kSeedB[32] = {32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17,
                                   16, 15, 14, 13, 12, 11, 10, 9,  8,  7,  6,  5,  4,  3,  2,  1};

class CaptureRouter : public Router
{
  public:
    ~CaptureRouter()
    {
        iface.release();
        delete cryptLock;
        cryptLock = nullptr;
    }

    void setRadio(RadioInterface *r)
    {
        iface.release();
        iface.reset(r);
    }

    void enqueueReceivedMessage(meshtastic_MeshPacket *p) override { packetPool.release(p); }
};

class CaptureRadio : public RadioInterface
{
  public:
    ErrorCode send(meshtastic_MeshPacket *p) override
    {
        packetPool.release(p);
        return ERRNO_OK;
    }
    uint32_t getPacketTime(uint32_t totalPacketLen, bool received = false) override
    {
        (void)totalPacketLen;
        (void)received;
        return 100;
    }
    bool queueMeshCoreFrame(const uint8_t *bytes, size_t len) override
    {
        memcpy(frame, bytes, len);
        frameLen = len;
        return true;
    }

    uint8_t frame[MeshCoreRepeater::MAX_FRAME_LEN];
    size_t frameLen = 0;
};

// Our identity (kSeedA) with one admin target (kSeedB). The target is installed directly in the MeshCore node store:
// the admin path never needs a NodeDB row, only the key/name/type the contact list already holds.
struct AdminSetup {
    struct RegionReady {
        RegionReady() { initRegion(); }
    } regionReady;

    Router *previousRouter = router;
    CaptureRouter capture;
    CaptureRadio radio;
    uint8_t pubA[32] = {};
    uint8_t pubB[32] = {};
    AdminSetup(uint8_t type = MeshCoreNode::REPEATER, const char *name = "Repeater")
    {
        router = &capture;
        capture.setRadio(&radio);
        Ed25519::derivePublicKey(pubA, kSeedA);
        Ed25519::derivePublicKey(pubB, kSeedB);
        meshCoreCompanion.setIdentity(kSeedA, pubA);

        MeshCoreNode *n = meshCoreCompanion.nodes.upsert(pubB);
        n->type = type;
        n->setName(name);
    }
    ~AdminSetup()
    {
        meshCoreAdmin.abort();
        router = previousRouter;
    }
};

static void clearAdminState()
{
    uint8_t empty[MeshCoreAdminFile::IMAGE_MAX];
    const size_t len = MeshCoreAdminFile::encode(nullptr, 0, empty, sizeof(empty));
    meshCoreAdmin.importFile(empty, len);
    meshCoreAdmin.abort();
    Time::setTestMillis(1000);
}

static void setTestEpoch()
{
    resetRTCStateForTests();
    struct timeval tv = {(time_t)(time(nullptr) + SEC_PER_DAY), 0}; // beyond BUILD_EPOCH on any host
    TEST_ASSERT_EQUAL_INT(RTCSetResultSuccess, perhapsSetRTC(RTCQualityFromNet, &tv));
}

static size_t makePathResponse(const uint8_t secret[32], uint8_t destHash, uint8_t srcHash, const uint8_t *extra, size_t extraLen,
                               uint8_t *out, size_t cap)
{
    uint8_t plain[2 + 96] = {};
    plain[0] = 0x00; // path_len: no hops
    plain[1] = 0x01; // extra type: RESPONSE
    TEST_ASSERT_TRUE(extraLen <= sizeof(plain) - 2);
    memcpy(&plain[2], extra, extraLen);
    return sealDatagram(secret, 0x08, destHash, srcHash, plain, 2 + extraLen, out, cap);
}

// Decrypt an ANON_REQ the way MeshCore's onRecvPacket does: dest hash, sender pubkey, MAC, then AES blocks.
static bool openAnonLogin(const uint8_t *frame, size_t len, const uint8_t secret[32], uint8_t *plain, size_t *plainLen)
{
    MeshCoreRepeater::FrameInfo info;
    if (!MeshCoreRepeater::parse(frame, len, &info) || info.payload_type != 0x07 || info.payload_len < 1 + 32 + 2)
        return false;
    const uint8_t *payload = frame + info.header_len;
    const size_t cipherLen = info.payload_len - (1 + 32 + 2);
    if (cipherLen % 16 != 0 || cipherLen > MeshCoreCompanion::MAX_CIPHER_LEN)
        return false;

    uint8_t mac[2];
    SHA256 hmac;
    hmac.resetHMAC(secret, 32);
    hmac.update(&payload[35], cipherLen);
    hmac.finalizeHMAC(secret, 32, mac, sizeof(mac));
    if (memcmp(mac, &payload[33], 2) != 0)
        return false;

    *plainLen = aesDecrypt(secret, &payload[35], cipherLen, plain);
    return *plainLen != 0;
}

static void fillCredential(MeshCoreAdminFile::Credential &c, uint8_t first, uint8_t type, const char *name, const char *pass)
{
    memset(&c, 0, sizeof(c));
    c.pub[0] = first;
    c.pub[31] = (uint8_t)(first ^ 0xFF);
    c.type = type;
    snprintf(c.name, sizeof(c.name), "%s", name);
    c.passLen = strlen(pass);
    memcpy(c.pass, pass, c.passLen);
}

void setUp(void)
{
    initializeTestEnvironment();
    clearAdminState();
    setTestEpoch();
}

void tearDown(void)
{
    meshCoreAdmin.abort();
    Time::useRealClock();
    resetRTCStateForTests();
}

static void test_file_round_trip_keeps_key_name_type_and_password()
{
    MeshCoreAdminFile::Credential in[2];
    fillCredential(in[0], 0x34, MeshCoreNode::REPEATER, "RPT-1", "s3cret");
    fillCredential(in[1], 0x56, MeshCoreNode::ROOM, "ROOM-1", "guest");

    uint8_t image[MeshCoreAdminFile::IMAGE_MAX] = {};
    const size_t len = MeshCoreAdminFile::encode(in, 2, image, sizeof(image));
    TEST_ASSERT_EQUAL_UINT(5 + 2 * 82 + 4, len);

    MeshCoreAdminFile::Credential out[2];
    bool valid = false;
    TEST_ASSERT_EQUAL_UINT(2, MeshCoreAdminFile::decode(image, len, out, 2, &valid));
    TEST_ASSERT_TRUE(valid);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(in[0].pub, out[0].pub, 32);
    TEST_ASSERT_EQUAL_UINT8(MeshCoreNode::REPEATER, out[0].type);
    TEST_ASSERT_EQUAL_STRING("RPT-1", out[0].name);
    TEST_ASSERT_EQUAL_STRING("s3cret", out[0].pass);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(in[1].pub, out[1].pub, 32);
    TEST_ASSERT_EQUAL_UINT8(MeshCoreNode::ROOM, out[1].type);
    TEST_ASSERT_EQUAL_STRING("guest", out[1].pass);
}

static void test_empty_image_is_valid_and_clears_the_table()
{
    MeshCoreAdminFile::Credential one;
    fillCredential(one, 0x11, MeshCoreNode::REPEATER, "RPT", "pw");
    uint8_t image[MeshCoreAdminFile::IMAGE_MAX] = {};
    MeshCoreAdminFile::encode(&one, 1, image, sizeof(image));
    meshCoreAdmin.importFile(image, 5 + 82 + 4);
    TEST_ASSERT_EQUAL_UINT(1, meshCoreAdmin.credentialCount());

    uint8_t empty[MeshCoreAdminFile::IMAGE_MAX] = {};
    const size_t len = MeshCoreAdminFile::encode(nullptr, 0, empty, sizeof(empty));
    TEST_ASSERT_EQUAL_UINT(5 + 4, len);
    TEST_ASSERT_EQUAL_UINT(0, meshCoreAdmin.importFile(empty, len));
    TEST_ASSERT_EQUAL_UINT(0, meshCoreAdmin.credentialCount());

    bool valid = false;
    TEST_ASSERT_EQUAL_UINT(0, MeshCoreAdminFile::decode(empty, len, &one, 1, &valid));
    TEST_ASSERT_TRUE(valid);
}

static void test_corrupt_or_short_image_loads_nothing()
{
    MeshCoreAdminFile::Credential in;
    fillCredential(in, 0x22, MeshCoreNode::REPEATER, "RPT", "pw");
    uint8_t image[MeshCoreAdminFile::IMAGE_MAX] = {};
    const size_t len = MeshCoreAdminFile::encode(&in, 1, image, sizeof(image));

    uint8_t bad[MeshCoreAdminFile::IMAGE_MAX];
    memcpy(bad, image, len);
    bad[len - 1] ^= 0x01;

    MeshCoreAdminFile::Credential out;
    bool valid = true;
    TEST_ASSERT_EQUAL_UINT(0, MeshCoreAdminFile::decode(bad, len, &out, 1, &valid));
    TEST_ASSERT_FALSE(valid);

    valid = true;
    TEST_ASSERT_EQUAL_UINT(0, MeshCoreAdminFile::decode(image, len - 1, &out, 1, &valid));
    TEST_ASSERT_FALSE(valid);

    uint8_t wrongType[MeshCoreAdminFile::IMAGE_MAX];
    memcpy(wrongType, image, len);
    wrongType[5 + 32] = MeshCoreNode::CHAT;
    TEST_ASSERT_EQUAL_UINT(0, MeshCoreAdminFile::decode(wrongType, len, &out, 1, &valid));
    TEST_ASSERT_FALSE(valid);

    TEST_ASSERT_EQUAL_UINT(0, MeshCoreAdminFile::encode(&in, 1, image, MeshCoreAdminFile::IMAGE_MAX - 1));
}

static void test_password_set_by_name_prefix_and_verb_first()
{
    uint8_t pub[32] = {};
    pub[0] = 0x34;
    pub[1] = 0xFA;
    MeshCoreNode *n = meshCoreCompanion.nodes.upsert(pub);
    n->type = MeshCoreNode::REPEATER;
    n->setName("RPT");

    char reply[meshtastic::NEXUS_MAX_REPLY] = {};
    bool wait = false;
    TEST_ASSERT_TRUE(meshCoreAdmin.handleCommand("RPT PASS s3cret", reply, sizeof(reply), wait));
    TEST_ASSERT_EQUAL_UINT(1, meshCoreAdmin.credentialCount());
    TEST_ASSERT_EQUAL_STRING("s3cret", meshCoreAdmin.credentialAt(0)->pass);

    TEST_ASSERT_TRUE(meshCoreAdmin.handleCommand("PASS 34fa other", reply, sizeof(reply), wait));
    TEST_ASSERT_FALSE(wait);
    TEST_ASSERT_EQUAL_UINT(1, meshCoreAdmin.credentialCount());
    TEST_ASSERT_EQUAL_STRING("other", meshCoreAdmin.credentialAt(0)->pass);

    TEST_ASSERT_TRUE(meshCoreAdmin.handleCommand("ffff PASS x", reply, sizeof(reply), wait));
    TEST_ASSERT_EQUAL_UINT(1, meshCoreAdmin.credentialCount());
    TEST_ASSERT_NOT_NULL(strstr(reply, "unknown"));
}

static void test_password_to_an_ambiguous_name_is_refused()
{
    uint8_t pubA[32] = {};
    pubA[0] = 0x10;
    uint8_t pubB[32] = {};
    pubB[0] = 0x20;
    MeshCoreNode *a = meshCoreCompanion.nodes.upsert(pubA);
    a->type = MeshCoreNode::REPEATER;
    a->setName("DUP");
    MeshCoreNode *b = meshCoreCompanion.nodes.upsert(pubB);
    b->type = MeshCoreNode::ROOM;
    b->setName("DUP");

    char reply[meshtastic::NEXUS_MAX_REPLY] = {};
    bool wait = false;
    TEST_ASSERT_TRUE(meshCoreAdmin.handleCommand("DUP PASS x", reply, sizeof(reply), wait));
    TEST_ASSERT_EQUAL_UINT(0, meshCoreAdmin.credentialCount());
    TEST_ASSERT_NOT_NULL(strstr(reply, "ambiguous"));
}

static void test_repeater_login_and_cli_round_trip()
{
    AdminSetup s(MeshCoreNode::REPEATER, "RPT-LOGIN");
    uint8_t secret[32];
    TEST_ASSERT_TRUE(MeshCoreCompanion::sharedSecret(kSeedA, s.pubB, secret));

    char reply[meshtastic::NEXUS_MAX_REPLY] = {};
    bool wait = false;
    TEST_ASSERT_TRUE(meshCoreAdmin.handleCommand("RPT-LOGIN PASS s3cret", reply, sizeof(reply), wait));
    s.radio.frameLen = 0;
    TEST_ASSERT_TRUE(meshCoreAdmin.handleCommand("RPT-LOGIN CMD tx 22", reply, sizeof(reply), wait));
    TEST_ASSERT_TRUE(wait);
    TEST_ASSERT_EQUAL(MeshCoreAdmin::LOGIN_SENT, meshCoreAdmin.phase());
    TEST_ASSERT_GREATER_THAN(0, s.radio.frameLen);

    MeshCoreRepeater::FrameInfo loginInfo;
    TEST_ASSERT_TRUE(MeshCoreRepeater::parse(s.radio.frame, s.radio.frameLen, &loginInfo));
    TEST_ASSERT_EQUAL_HEX8(0x07, loginInfo.payload_type);
    TEST_ASSERT_EQUAL_HEX8(0x01, loginInfo.route_type);
    TEST_ASSERT_EQUAL_HEX8(s.pubB[0], s.radio.frame[loginInfo.header_len]);

    uint8_t plain[32] = {};
    size_t plainLen = 0;
    TEST_ASSERT_TRUE(openAnonLogin(s.radio.frame, s.radio.frameLen, secret, plain, &plainLen));
    TEST_ASSERT_TRUE(plainLen >= 4 + strlen("s3cret"));
    TEST_ASSERT_EQUAL_STRING("s3cret", (const char *)&plain[4]);

    uint8_t extra[13] = {};
    extra[4] = 0; // RESP_SERVER_LOGIN_OK
    extra[6] = 1; // admin
    extra[7] = 3; // PERM_ACL_ADMIN
    uint8_t response[128];
    const size_t responseLen = makePathResponse(secret, s.pubA[0], s.pubB[0], extra, sizeof(extra), response, sizeof(response));
    TEST_ASSERT_GREATER_THAN(0, responseLen);
    MeshCoreRepeater::FrameInfo responseInfo;
    TEST_ASSERT_TRUE(MeshCoreRepeater::parse(response, responseLen, &responseInfo));
    TEST_ASSERT_TRUE(meshCoreAdmin.onPathResponse(response, responseLen, responseInfo));
    TEST_ASSERT_EQUAL(MeshCoreAdmin::COMMAND_SENT, meshCoreAdmin.phase());
    TEST_ASSERT_GREATER_THAN(0, s.radio.frameLen); // the CLI frame replaced the login frame

    char text[MeshCoreCompanion::MAX_TEXT_LEN + 1];
    uint32_t timestamp = 0;
    uint8_t flags = 0;
    MeshCoreRepeater::FrameInfo cmdInfo;
    TEST_ASSERT_TRUE(MeshCoreRepeater::parse(s.radio.frame, s.radio.frameLen, &cmdInfo));
    TEST_ASSERT_TRUE(MeshCoreCompanion::decodeDirectTextType(secret, 1, s.radio.frame, s.radio.frameLen, cmdInfo, text,
                                                             sizeof(text), &timestamp, &flags));
    TEST_ASSERT_EQUAL_STRING("tx 22", text);
    TEST_ASSERT_EQUAL_HEX8(MeshCoreCompanion::TXT_FLAGS_CLI, flags & 0xFC);

    uint8_t cli[128];
    const size_t cliLen = MeshCoreCompanion::encodeDirectText(secret, s.pubA[0], s.pubB[0], 99, MeshCoreCompanion::TXT_FLAGS_CLI,
                                                              "ok", cli, sizeof(cli));
    MeshCoreRepeater::FrameInfo cliInfo;
    TEST_ASSERT_TRUE(MeshCoreRepeater::parse(cli, cliLen, &cliInfo));
    TEST_ASSERT_TRUE(meshCoreAdmin.onCliText(cli, cliLen, cliInfo));
    char event[MeshCoreAdmin::EVENT_MAX] = {};
    TEST_ASSERT_TRUE(meshCoreAdmin.pollEvent(event, sizeof(event)));
    TEST_ASSERT_EQUAL_STRING("RPT-LOGIN: ok", event);
    TEST_ASSERT_EQUAL(MeshCoreAdmin::IDLE, meshCoreAdmin.phase());
    TEST_ASSERT_FALSE(meshCoreAdmin.pollEvent(event, sizeof(event)));
}

static void test_room_login_carries_sync_since()
{
    AdminSetup s(MeshCoreNode::ROOM, "ROOM-LOGIN");
    char reply[meshtastic::NEXUS_MAX_REPLY] = {};
    bool wait = false;
    TEST_ASSERT_TRUE(meshCoreAdmin.handleCommand("ROOM-LOGIN PASS pw", reply, sizeof(reply), wait));
    TEST_ASSERT_TRUE(meshCoreAdmin.handleCommand("ROOM-LOGIN CMD status", reply, sizeof(reply), wait));

    uint8_t secret[32], plain[32];
    size_t plainLen = 0;
    TEST_ASSERT_TRUE(MeshCoreCompanion::sharedSecret(kSeedA, s.pubB, secret));
    TEST_ASSERT_TRUE(openAnonLogin(s.radio.frame, s.radio.frameLen, secret, plain, &plainLen));
    TEST_ASSERT_TRUE(plainLen >= 8 + 2);
    TEST_ASSERT_EQUAL_HEX8(0xFF, plain[4]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, plain[5]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, plain[6]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, plain[7]);
    TEST_ASSERT_EQUAL_STRING("pw", (const char *)&plain[8]);
}

static void test_non_admin_login_reports_and_sends_no_command()
{
    AdminSetup s(MeshCoreNode::REPEATER, "RPT-NOADMIN");
    uint8_t secret[32];
    TEST_ASSERT_TRUE(MeshCoreCompanion::sharedSecret(kSeedA, s.pubB, secret));

    char reply[meshtastic::NEXUS_MAX_REPLY] = {};
    bool wait = false;
    TEST_ASSERT_TRUE(meshCoreAdmin.handleCommand("RPT-NOADMIN PASS pw", reply, sizeof(reply), wait));
    TEST_ASSERT_TRUE(meshCoreAdmin.handleCommand("RPT-NOADMIN CMD info", reply, sizeof(reply), wait));
    const size_t loginFrameLen = s.radio.frameLen;

    uint8_t extra[13] = {};
    extra[6] = 0; // not admin
    extra[7] = 0;
    uint8_t response[128];
    const size_t responseLen = makePathResponse(secret, s.pubA[0], s.pubB[0], extra, sizeof(extra), response, sizeof(response));
    MeshCoreRepeater::FrameInfo info;
    TEST_ASSERT_TRUE(MeshCoreRepeater::parse(response, responseLen, &info));
    TEST_ASSERT_TRUE(meshCoreAdmin.onPathResponse(response, responseLen, info));
    TEST_ASSERT_EQUAL(MeshCoreAdmin::IDLE, meshCoreAdmin.phase());
    TEST_ASSERT_EQUAL(loginFrameLen, s.radio.frameLen);

    char event[MeshCoreAdmin::EVENT_MAX] = {};
    TEST_ASSERT_TRUE(meshCoreAdmin.pollEvent(event, sizeof(event)));
    TEST_ASSERT_NOT_NULL(strstr(event, "not admin"));
}

static void test_oversized_login_response_is_rejected()
{
    AdminSetup s(MeshCoreNode::REPEATER, "RPT-BIGRESP");
    uint8_t secret[32];
    TEST_ASSERT_TRUE(MeshCoreCompanion::sharedSecret(kSeedA, s.pubB, secret));

    char reply[meshtastic::NEXUS_MAX_REPLY] = {};
    bool wait = false;
    TEST_ASSERT_TRUE(meshCoreAdmin.handleCommand("RPT-BIGRESP PASS pw", reply, sizeof(reply), wait));
    TEST_ASSERT_TRUE(meshCoreAdmin.handleCommand("RPT-BIGRESP CMD info", reply, sizeof(reply), wait));
    TEST_ASSERT_EQUAL(MeshCoreAdmin::LOGIN_SENT, meshCoreAdmin.phase());

    uint8_t extra[64] = {};
    extra[4] = 0;
    extra[6] = 1;
    extra[7] = 3;
    uint8_t response[128];
    const size_t responseLen = makePathResponse(secret, s.pubA[0], s.pubB[0], extra, sizeof(extra), response, sizeof(response));
    MeshCoreRepeater::FrameInfo info;
    TEST_ASSERT_TRUE(MeshCoreRepeater::parse(response, responseLen, &info));
    TEST_ASSERT_FALSE(meshCoreAdmin.onPathResponse(response, responseLen, info));
    TEST_ASSERT_EQUAL(MeshCoreAdmin::LOGIN_SENT, meshCoreAdmin.phase());

    char event[MeshCoreAdmin::EVENT_MAX] = {};
    TEST_ASSERT_FALSE(meshCoreAdmin.pollEvent(event, sizeof(event)));
}

static void test_login_timeout_reports_once()
{
    AdminSetup s(MeshCoreNode::REPEATER, "RPT-TIMEOUT");
    char reply[meshtastic::NEXUS_MAX_REPLY] = {};
    bool wait = false;
    TEST_ASSERT_TRUE(meshCoreAdmin.handleCommand("RPT-TIMEOUT PASS pw", reply, sizeof(reply), wait));
    TEST_ASSERT_TRUE(meshCoreAdmin.handleCommand("RPT-TIMEOUT CMD info", reply, sizeof(reply), wait));

    Time::advanceTestMillis(MeshCoreAdmin::LOGIN_TIMEOUT_MS + 1);
    meshCoreAdmin.service();
    char event[MeshCoreAdmin::EVENT_MAX] = {};
    TEST_ASSERT_TRUE(meshCoreAdmin.pollEvent(event, sizeof(event)));
    TEST_ASSERT_NOT_NULL(strstr(event, "no login response"));
    TEST_ASSERT_EQUAL(MeshCoreAdmin::IDLE, meshCoreAdmin.phase());
    TEST_ASSERT_FALSE(meshCoreAdmin.pollEvent(event, sizeof(event)));
}

static void test_command_timeout_relogs_and_retries_once()
{
    AdminSetup s(MeshCoreNode::REPEATER, "RPT-RETRY");
    uint8_t secret[32];
    TEST_ASSERT_TRUE(MeshCoreCompanion::sharedSecret(kSeedA, s.pubB, secret));

    char reply[meshtastic::NEXUS_MAX_REPLY] = {};
    bool wait = false;
    TEST_ASSERT_TRUE(meshCoreAdmin.handleCommand("RPT-RETRY PASS pw", reply, sizeof(reply), wait));
    TEST_ASSERT_TRUE(meshCoreAdmin.handleCommand("RPT-RETRY CMD info", reply, sizeof(reply), wait));

    uint8_t extra[13] = {};
    extra[6] = 1;
    extra[7] = 3;
    uint8_t response[128];
    const size_t responseLen = makePathResponse(secret, s.pubA[0], s.pubB[0], extra, sizeof(extra), response, sizeof(response));
    MeshCoreRepeater::FrameInfo info;
    TEST_ASSERT_TRUE(MeshCoreRepeater::parse(response, responseLen, &info));
    TEST_ASSERT_TRUE(meshCoreAdmin.onPathResponse(response, responseLen, info));
    TEST_ASSERT_EQUAL(MeshCoreAdmin::COMMAND_SENT, meshCoreAdmin.phase());

    Time::advanceTestMillis(MeshCoreAdmin::COMMAND_TIMEOUT_MS + 1);
    meshCoreAdmin.service();
    TEST_ASSERT_EQUAL(MeshCoreAdmin::LOGIN_SENT, meshCoreAdmin.phase());
    MeshCoreRepeater::FrameInfo retryInfo;
    TEST_ASSERT_TRUE(MeshCoreRepeater::parse(s.radio.frame, s.radio.frameLen, &retryInfo));
    TEST_ASSERT_EQUAL_HEX8(0x07, retryInfo.payload_type);

    // Answer the retry login: the command must be sent again.
    const size_t retryResponseLen =
        makePathResponse(secret, s.pubA[0], s.pubB[0], extra, sizeof(extra), response, sizeof(response));
    TEST_ASSERT_TRUE(MeshCoreRepeater::parse(response, retryResponseLen, &info));
    TEST_ASSERT_TRUE(meshCoreAdmin.onPathResponse(response, retryResponseLen, info));
    TEST_ASSERT_EQUAL(MeshCoreAdmin::COMMAND_SENT, meshCoreAdmin.phase());

    Time::advanceTestMillis(MeshCoreAdmin::COMMAND_TIMEOUT_MS + 1);
    meshCoreAdmin.service();
    TEST_ASSERT_EQUAL(MeshCoreAdmin::IDLE, meshCoreAdmin.phase());
    char event[MeshCoreAdmin::EVENT_MAX] = {};
    TEST_ASSERT_TRUE(meshCoreAdmin.pollEvent(event, sizeof(event)));
    TEST_ASSERT_NOT_NULL(strstr(event, "no command reply"));
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_file_round_trip_keeps_key_name_type_and_password);
    RUN_TEST(test_empty_image_is_valid_and_clears_the_table);
    RUN_TEST(test_corrupt_or_short_image_loads_nothing);
    RUN_TEST(test_password_set_by_name_prefix_and_verb_first);
    RUN_TEST(test_password_to_an_ambiguous_name_is_refused);
    RUN_TEST(test_repeater_login_and_cli_round_trip);
    RUN_TEST(test_room_login_carries_sync_since);
    RUN_TEST(test_non_admin_login_reports_and_sends_no_command);
    RUN_TEST(test_oversized_login_response_is_rejected);
    RUN_TEST(test_login_timeout_reports_once);
    RUN_TEST(test_command_timeout_relogs_and_retries_once);
    exit(UNITY_END());
}

void loop() {}
