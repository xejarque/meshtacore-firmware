// SPDX-License-Identifier: AGPL-3.0-only
//
// /mtc VOLT: up to four voltage threshold rules that run a command, hibernate or wake the node when a battery or INA3221
// channel crosses a level. Ported from JenTastic 2.8.105 (NexusModule::runVoltageAutomations and the VOLT command) and split into
// small functions. It lives here, not in NexusModule.cpp / NexusCommands.cpp, so a merge of the jentastic upstream does not
// collide with it. In the JenTastic vault the rules are v68; in this fork's vault they are v70.

#include "Power.h"
#include "configuration.h"
#include "main.h"
#include "modules/NexusCommandParse.h"
#include "modules/NexusModule.h"
#include "sleep.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>

using namespace meshtastic;

namespace
{
constexpr uint8_t VOLT_RULE_COUNT = 4;
constexpr uint8_t VOLT_DEBOUNCE_SAMPLES = 3;
constexpr uint16_t VOLT_REARM_HYSTERESIS_MV = 50;
constexpr uint16_t VOLT_AUTO_WAKE_MARGIN_MV = 100;
constexpr uint32_t HIBERNATE_SNIFF_INTERVAL_MS = 60000;
constexpr uint16_t VOLT_MIN_MV = 500;
constexpr uint16_t VOLT_MAX_MV = 30000;
constexpr int INA3221_MV_PER_BUS_LSB = 8;

#if defined(ARCH_ESP32)
// Survives deep sleep so a hibernating node knows it is hibernating when the timer wakes it.
RTC_DATA_ATTR bool rtcNexusHibernating = false;
#else
bool rtcNexusHibernating = false;
#endif

// nRF52 System OFF has no timer: without a battery LPCOMP wake, the 60 s sniff only happens if the sleep is a timed one.
void sleepUntilNextHibernateSniff()
{
#ifdef ARCH_NRF52
    if (!nrf52ArmsBatteryLpcompWake())
        nrf52RequestTimedSleep();
#endif
    doDeepSleep(HIBERNATE_SNIFF_INTERVAL_MS, true, true);
}

bool isRuleActive(const NexusModule::NexusVoltRule &rule)
{
    return rule.action != NexusModule::NEXUS_VOLT_ACT_NONE && rule.mv > 0;
}

const char *actionName(uint8_t action)
{
    switch (action) {
    case NexusModule::NEXUS_VOLT_ACT_CMD:
        return "CMD";
    case NexusModule::NEXUS_VOLT_ACT_HIBERNATE:
        return "HIB";
    case NexusModule::NEXUS_VOLT_ACT_WAKE:
        return "WAKE";
    default:
        return "UNK";
    }
}

const char *directionName(uint8_t direction)
{
    return direction == NexusModule::NEXUS_VOLT_DIR_HIGH ? "HIGH" : "LOW";
}

const char *channelName(uint8_t channel)
{
    return channel == 1 ? "CH2" : channel == 2 ? "CH3" : "BAT";
}

bool matchesThreshold(const NexusModule::NexusVoltRule &rule, uint16_t currentMv)
{
    return rule.direction == NexusModule::NEXUS_VOLT_DIR_HIGH ? currentMv >= rule.mv : currentMv <= rule.mv;
}

bool hasRearmed(const NexusModule::NexusVoltRule &rule, uint16_t currentMv)
{
    if (rule.direction == NexusModule::NEXUS_VOLT_DIR_HIGH)
        return currentMv + VOLT_REARM_HYSTERESIS_MV <= rule.mv;
    return currentMv >= rule.mv + VOLT_REARM_HYSTERESIS_MV;
}

// Accepts "3.7" (volts), "4" (volts, up to 30) or "3700" (millivolts). Returns 0 when it is not a voltage.
uint16_t parseMillivolts(const char *token)
{
    if (strchr(token, '.') != nullptr) {
        float volts = 0;
        if (!parseFloat(token, volts, 0.001f, 30.0f))
            return 0;
        return (uint16_t)(volts * 1000.0f + 0.5f);
    }
    uint32_t value = 0;
    if (!parseUInt(token, value, 30000) || value == 0)
        return 0;
    return value <= 30 ? (uint16_t)(value * 1000) : (uint16_t)value;
}

bool parseDirection(const char *token, uint8_t *direction)
{
    if (strcasecmp(token, "HIGH") == 0 || strcasecmp(token, "HI") == 0 || strcasecmp(token, "RISE") == 0 ||
        strcasecmp(token, "UP") == 0 || strcmp(token, ">=") == 0) {
        *direction = NexusModule::NEXUS_VOLT_DIR_HIGH;
        return true;
    }
    if (strcasecmp(token, "LOW") == 0 || strcasecmp(token, "LO") == 0 || strcasecmp(token, "FALL") == 0 ||
        strcasecmp(token, "DOWN") == 0 || strcmp(token, "<=") == 0) {
        *direction = NexusModule::NEXUS_VOLT_DIR_LOW;
        return true;
    }
    return false;
}

bool parseAction(const char *token, uint8_t *action)
{
    if (strcasecmp(token, "CMD") == 0 || strcasecmp(token, "COMMAND") == 0) {
        *action = NexusModule::NEXUS_VOLT_ACT_CMD;
        return true;
    }
    if (strcasecmp(token, "HIB") == 0 || strcasecmp(token, "HIBERNATE") == 0 || strcasecmp(token, "SLEEP") == 0) {
        *action = NexusModule::NEXUS_VOLT_ACT_HIBERNATE;
        return true;
    }
    if (strcasecmp(token, "WAKE") == 0) {
        *action = NexusModule::NEXUS_VOLT_ACT_WAKE;
        return true;
    }
    return false;
}

// Copies the next space-delimited word into `out` and returns the pointer after it and the spaces that follow.
// A word that does not fit yields an empty `out`, so callers reject it instead of parsing the leftover as the next token.
const char *takeWord(const char *p, char *out, size_t outSize)
{
    size_t length = 0;
    while (*p && *p != ' ' && length < outSize - 1)
        out[length++] = *p++;
    out[length] = '\0';
    if (*p && *p != ' ') {
        out[0] = '\0';
        while (*p && *p != ' ')
            p++;
    }
    while (*p == ' ')
        p++;
    return p;
}

// Consumes an optional leading CH1 / CH2 / CH3 / BAT and returns the matching channel index (0 when absent).
const char *takeChannel(const char *p, uint8_t *channel)
{
    static const struct {
        const char *name;
        uint8_t channel;
    } CHANNELS[] = {{"CH1", 0}, {"CH2", 1}, {"CH3", 2}, {"BAT", 0}};
    *channel = 0;
    for (const auto &entry : CHANNELS) {
        if (strncasecmp(p, entry.name, 3) == 0 && (p[3] == ' ' || p[3] == '\0')) {
            *channel = entry.channel;
            p += 3;
            while (*p == ' ')
                p++;
            break;
        }
    }
    return p;
}
} // namespace

void NexusModule::seedHibernatingFromRtc()
{
    is_hibernating = rtcNexusHibernating;
}

void NexusModule::setHibernating(bool hib)
{
    is_hibernating = hib;
#if defined(ARCH_ESP32)
    rtcNexusHibernating = hib;
#endif
}

uint16_t NexusModule::getVoltageReadingMv(uint8_t channel)
{
#if HAS_TELEMETRY && !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR && __has_include(<INA3221.h>)
    const bool inaPresent = ina3221Sensor.hasSensor();
#else
    const bool inaPresent = false;
#endif
    if (channel == 0) {
        uint16_t millivolts = powerStatus ? powerStatus->getBatteryVoltageMv() : 0;
#if HAS_TELEMETRY && !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR && __has_include(<INA3221.h>)
        if (millivolts == 0 && inaPresent) {
            const int16_t raw = ina3221Sensor.getRawBusVoltage(0);
            if (raw > 0)
                millivolts = (uint16_t)(raw * INA3221_MV_PER_BUS_LSB);
        }
#endif
        return millivolts;
    }
#if HAS_TELEMETRY && !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR && __has_include(<INA3221.h>)
    if (inaPresent && channel <= 2) {
        const int16_t raw = ina3221Sensor.getRawBusVoltage(channel);
        if (raw > 0)
            return (uint16_t)(raw * INA3221_MV_PER_BUS_LSB);
    }
#endif
    return 0;
}

// Runs the rule's command as if it came from this node.
void NexusModule::runRuleCommand(const char *command)
{
    if (command[0] == '\0')
        return;
    meshtastic_MeshPacket self = meshtastic_MeshPacket_init_default;
    self.from = myNodeInfo.my_node_num;
    self.to = myNodeInfo.my_node_num;
    handleNexusCommand(self, command, false, 0, true);
}

void NexusModule::runVoltageAutomations()
{
    bool anyRuleActive = false;
    for (const auto &rule : volt_rules)
        anyRuleActive |= isRuleActive(rule);
    if (!anyRuleActive && !is_hibernating)
        return;

    for (uint8_t i = 0; i < VOLT_RULE_COUNT; i++) {
        NexusVoltRule &rule = volt_rules[i];
        if (!isRuleActive(rule))
            continue;
        const uint16_t currentMv = getVoltageReadingMv(rule.channel);
        if (currentMv == 0)
            continue;

        if (!matchesThreshold(rule, currentMv)) {
            volt_debounce_count[i] = 0;
            if (hasRearmed(rule, currentMv))
                volt_triggered[i] = false;
            continue;
        }
        if (volt_debounce_count[i] < VOLT_DEBOUNCE_SAMPLES)
            volt_debounce_count[i]++;
        if (volt_debounce_count[i] >= VOLT_DEBOUNCE_SAMPLES && !volt_triggered[i]) {
            volt_triggered[i] = true;
            LOG_INFO("MTC: Volt slot %d triggered (%u mV, dir %u, act %u, ch %u)", i + 1, (unsigned)currentMv,
                     (unsigned)rule.direction, (unsigned)rule.action, (unsigned)rule.channel);
            applyRuleAction(rule);
        }
    }

    if (is_hibernating)
        continueHibernation();
}

void NexusModule::applyRuleAction(const NexusVoltRule &rule)
{
    switch (rule.action) {
    case NEXUS_VOLT_ACT_CMD:
        runRuleCommand(rule.command);
        break;
    case NEXUS_VOLT_ACT_HIBERNATE:
        setHibernating(true);
        runRuleCommand(rule.command);
        LOG_INFO("MTC: Entering voltage-induced hibernation (sniff interval 60s)...");
        // skipSaveNodeDb: saveToDisk() has no dirty check, so saving on every ~75 s cycle would wear the flash, and nothing new
        // was heard since the node went into hibernation.
        sleepUntilNextHibernateSniff();
        break;
    case NEXUS_VOLT_ACT_WAKE:
        setHibernating(false);
        runRuleCommand(rule.command);
        LOG_INFO("MTC: Woke from hibernation by voltage rule!");
        break;
    }
}

// While hibernating the node wakes every minute, looks at the voltage, and goes back to sleep unless it recovered.
void NexusModule::continueHibernation()
{
    bool hasWakeRule = false;
    uint16_t hibernateMv = 0;
    uint8_t hibernateChannel = 0;
    for (const auto &rule : volt_rules) {
        if (rule.action == NEXUS_VOLT_ACT_WAKE && rule.mv > 0)
            hasWakeRule = true;
        if (rule.action == NEXUS_VOLT_ACT_HIBERNATE && rule.mv > hibernateMv) {
            hibernateMv = rule.mv;
            hibernateChannel = rule.channel;
        }
    }
    if (!hasWakeRule) {
        const uint16_t currentMv = getVoltageReadingMv(hibernateChannel);
        if (currentMv >= hibernateMv + VOLT_AUTO_WAKE_MARGIN_MV) {
            setHibernating(false);
            LOG_INFO("MTC: Auto-woke from hibernation (ch %u cur %u mV >= %u mV)", (unsigned)hibernateChannel,
                     (unsigned)currentMv, (unsigned)(hibernateMv + VOLT_AUTO_WAKE_MARGIN_MV));
            return;
        }
    }
    LOG_INFO("MTC: Hibernating sniff complete, returning to deep sleep (60s)...");
    sleepUntilNextHibernateSniff();
}

void NexusModule::listVoltRules(const meshtastic_MeshPacket &mp, uint32_t replyId)
{
    startPaging(mp.to == myNodeInfo.my_node_num ? mp.from : 0xFFFFFFFF, mp.channel, replyId, "VOLT Rules");
    char line[96];
    const uint16_t batteryMv = getVoltageReadingMv(0);
    snprintf(line, sizeof(line), "MESHTACORE VOLT (Bat: %u.%02uV, Hib: %s):\n", batteryMv / 1000, (batteryMv % 1000) / 10,
             is_hibernating ? "YES" : "NO");
    appendPaging(line);
    for (uint8_t i = 0; i < VOLT_RULE_COUNT; i++) {
        const NexusVoltRule &rule = volt_rules[i];
        if (!isRuleActive(rule)) {
            snprintf(line, sizeof(line), "[%d] OFF\n", i + 1);
        } else {
            const uint16_t reading = getVoltageReadingMv(rule.channel);
            snprintf(line, sizeof(line), "[%d] %u.%02uV %s %s [%s] (%s, cur %u.%02uV)%s%.20s\n", i + 1, rule.mv / 1000,
                     (rule.mv % 1000) / 10, directionName(rule.direction), actionName(rule.action), channelName(rule.channel),
                     volt_triggered[i] ? "TRIGGERED" : "ARMED", reading / 1000, (reading % 1000) / 10,
                     rule.command[0] ? " : " : "", rule.command);
        }
        appendPaging(line);
    }
    snprintf(replyMsg, sizeof(replyMsg), "MESHTACORE: Paging VOLT rules...");
}

void NexusModule::showVoltRule(uint8_t slot)
{
    const NexusVoltRule &rule = volt_rules[slot - 1];
    if (!isRuleActive(rule)) {
        snprintf(replyMsg, sizeof(replyMsg), "MESHTACORE VOLT [%d]: OFF", slot);
        return;
    }
    snprintf(replyMsg, sizeof(replyMsg), "MESHTACORE VOLT [%d]: %u.%02uV %s %s [%s] %s%s", slot, rule.mv / 1000,
             (rule.mv % 1000) / 10, directionName(rule.direction), actionName(rule.action), channelName(rule.channel),
             volt_triggered[slot - 1] ? "[TRIG] " : "[ARMED] ", rule.command);
}

void NexusModule::clearVoltRule(uint8_t slot)
{
    memset(&volt_rules[slot - 1], 0, sizeof(NexusVoltRule));
    volt_debounce_count[slot - 1] = 0;
    volt_triggered[slot - 1] = false;
    snprintf(replyMsg, sizeof(replyMsg), "MESHTACORE: Volt slot [%d] cleared (RAM). /mtc SAVE to persist.", slot);
}

// The rule about to go into `slot`, seen against the reading now and the other slots' HIB and WAKE thresholds.
VoltRuleProposal NexusModule::proposeVoltRule(uint8_t slot, uint16_t mv, uint8_t direction, uint8_t action, uint8_t channel)
{
    VoltRuleProposal p;
    p.hibernate = action == NEXUS_VOLT_ACT_HIBERNATE;
    p.wake = action == NEXUS_VOLT_ACT_WAKE;
    p.fallingEdge = direction == NEXUS_VOLT_DIR_LOW;
    p.mv = mv;
    p.currentMv = getVoltageReadingMv(channel);
    for (uint8_t i = 0; i < VOLT_RULE_COUNT; i++) {
        const NexusVoltRule &other = volt_rules[i];
        if (i == slot - 1 || !isRuleActive(other))
            continue;
        if (other.action == NEXUS_VOLT_ACT_HIBERNATE && other.mv > p.highestOtherHibMv)
            p.highestOtherHibMv = other.mv;
        if (other.action == NEXUS_VOLT_ACT_WAKE && (p.lowestOtherWakeMv == 0 || other.mv < p.lowestOtherWakeMv))
            p.lowestOtherWakeMv = other.mv;
    }
    return p;
}

// <voltage> <HIGH|LOW> <CMD|HIB|WAKE> [CH1..3|BAT] [command...]
void NexusModule::configureVoltRule(uint8_t slot, const char *args)
{
    char voltageToken[16] = {0};
    char directionToken[16] = {0};
    char actionToken[16] = {0};
    args = takeWord(args, voltageToken, sizeof(voltageToken));
    args = takeWord(args, directionToken, sizeof(directionToken));
    args = takeWord(args, actionToken, sizeof(actionToken));
    if (voltageToken[0] == '\0' || directionToken[0] == '\0' || actionToken[0] == '\0') {
        snprintf(replyMsg, sizeof(replyMsg),
                 "MESHTACORE VOLT: Usage: VOLT <1..4> <V|mV> <HIGH|LOW> <CMD|HIB|WAKE> [CH1..3] [cmd...]");
        return;
    }
    const uint16_t targetMv = parseMillivolts(voltageToken);
    if (targetMv < VOLT_MIN_MV || targetMv > VOLT_MAX_MV) {
        snprintf(replyMsg, sizeof(replyMsg), "MESHTACORE VOLT: Invalid voltage '%s' (range 500..30000 mV)", voltageToken);
        return;
    }
    uint8_t direction = 0;
    if (!parseDirection(directionToken, &direction)) {
        snprintf(replyMsg, sizeof(replyMsg), "MESHTACORE VOLT: Invalid direction '%s' (HIGH or LOW)", directionToken);
        return;
    }
    uint8_t action = 0;
    if (!parseAction(actionToken, &action)) {
        snprintf(replyMsg, sizeof(replyMsg), "MESHTACORE VOLT: Invalid action '%s' (CMD, HIB, or WAKE)", actionToken);
        return;
    }
    uint8_t channel = 0;
    args = takeChannel(args, &channel);

    if (const char *problem = voltRuleStrandsNode(proposeVoltRule(slot, targetMv, direction, action, channel))) {
        snprintf(replyMsg, sizeof(replyMsg), "MESHTACORE VOLT: [%d] refused, %s", slot, problem);
        return;
    }

    NexusVoltRule &rule = volt_rules[slot - 1];
    memset(&rule, 0, sizeof(rule));
    rule.mv = targetMv;
    rule.direction = direction;
    rule.action = action;
    rule.channel = channel;
    strncpy(rule.command, args, sizeof(rule.command) - 1);
    volt_debounce_count[slot - 1] = 0;
    volt_triggered[slot - 1] = false;

    snprintf(replyMsg, sizeof(replyMsg),
             "MESHTACORE VOLT: [%d] %u.%02uV %s %s [%s]%s%.16s configured (RAM). Run /mtc SAVE to persist.", slot,
             targetMv / 1000, (targetMv % 1000) / 10, directionName(direction), actionName(action), channelName(channel),
             rule.command[0] ? " : " : "", rule.command);
}

static bool isClearToken(const char *text)
{
    return strcasecmp(text, "OFF") == 0 || strcasecmp(text, "CLEAR") == 0 || strcasecmp(text, "NONE") == 0 ||
           strcmp(text, "0") == 0;
}

void NexusModule::handleVoltCommand(const meshtastic_MeshPacket &mp, uint32_t replyId, const char *cmd)
{
    const char *args = strncasecmp(cmd, "VOLTAGE", 7) == 0 ? cmd + 7 : cmd + 4;
    while (*args == ' ')
        args++;

    if (*args == '\0' || strcasecmp(args, "LIST") == 0 || strcasecmp(args, "STATUS") == 0) {
        listVoltRules(mp, replyId);
        return;
    }
    if (strcasecmp(args, "WAKE") == 0) {
        setHibernating(false);
        snprintf(replyMsg, sizeof(replyMsg), "MESHTACORE: Hibernation cleared in RAM.");
        return;
    }
    // The slot is one digit followed by a space or the end: "VOLT 40" is not slot 4 plus a clear token.
    if (*args < '1' || *args > '4' || (args[1] != '\0' && args[1] != ' ')) {
        snprintf(replyMsg, sizeof(replyMsg), "MESHTACORE VOLT: Invalid slot '%s'. Use 1..4.", args);
        return;
    }
    const uint8_t slot = *args - '0';
    args++;
    while (*args == ' ')
        args++;

    if (*args == '\0')
        showVoltRule(slot);
    else if (isClearToken(args))
        clearVoltRule(slot);
    else
        configureVoltRule(slot, args);
}
