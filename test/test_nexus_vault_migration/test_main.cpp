// Unit tests for applyZeroHopRoleDefaults() and noteLoadedVault() in src/modules/NexusMeshtacoreRuntime.cpp.
//
// What is under test:
//   - noteLoadedVault() records the vault version and invokes applyVaultV68() when v.version >= 68.
//   - noteLoadedVault() restores custom ping_trigger when v.version >= 69, defaulting to /ping on older vaults.
//   - applyZeroHopRoleDefaults() selectively enables Zero-Hop role defaults (ZERO_HOP_EN, ZERO_HOP_AUTO_ROUTER,
//     ZERO_HOP_AUTO_CLIENT) for infrastructure roles (ROUTER, ROUTER_LATE, CLIENT_BASE), migrating older vaults
//     (v65-v67) to v69 in flash exactly once.
//
// Why that behavior is required:
//   - Infrastructure roles require Zero-Hop and auto masks enabled out of the box, but once a v68+ vault is
//     persisted, an operator's explicit choice to disable or tweak Zero-Hop must not be overridden on subsequent boots.
//   - Client nodes (non-infrastructure) must not be forced into Zero-Hop defaults, nor should they incur an automatic
//     flash write migration on boot.
//   - Nodes running without any vault (vaultVersionLoaded == 0) must apply defaults in RAM only, without writing flash.
//
// The regression guarded:
//   - If applyZeroHopRoleDefaults() ignores the v68 version check, an operator's intentional Zero-Hop changes are erased
//     on every reboot.
//   - If the migration does not synchronize vaultVersionLoaded in RAM, subsequent runtime checks believe the node runs an
//     older vault until a hard reboot.
//   - If client roles are not filtered, client nodes unnecessarily write to flash at boot and enable router masks.
//   - If noteLoadedVault() reads ping_trigger on a v68 image, uninitialized memory corrupts the active ping trigger.
#include "MeshTypes.h"
#include "TestUtil.h"
#include <unity.h>

#include "configuration.h"
#include "modules/NexusModule.h"

namespace
{

class TestNexusModule : public meshtastic::NexusModule
{
  public:
    int saveVaultCallCount = 0;
    bool saveVaultReturnValue = true;

    bool saveVault() override
    {
        saveVaultCallCount++;
        return saveVaultReturnValue;
    }
};

void test_noteLoadedVault_records_version_and_applies_v68()
{
    TestNexusModule module;
    meshtastic::NexusModule::NexusVault v = {};
    v.version = 68;
    v.lpcomp_wake_ref = 8;
    v.battery_chemistry = 2;
    v.ext_flags = meshtastic::NexusModule::EXT_FLAG_PUBLIC_SILENCE;

    module.noteLoadedVault(v);

    TEST_ASSERT_EQUAL_UINT32(68, module.vaultVersionLoaded);
    TEST_ASSERT_EQUAL_UINT8(8, module.lpcomp_wake_ref);
    TEST_ASSERT_EQUAL_UINT8(meshtastic::NexusModule::EXT_FLAG_PUBLIC_SILENCE, module.ext_flags);
}

void test_noteLoadedVault_records_version_without_applying_v68_fields_for_older_vault()
{
    TestNexusModule module;
    meshtastic::NexusModule::NexusVault v = {};
    v.version = 67;
    v.lpcomp_wake_ref = 8;
    v.battery_chemistry = 2;

    module.noteLoadedVault(v);

    TEST_ASSERT_EQUAL_UINT32(67, module.vaultVersionLoaded);
    // v68 fields must not be overwritten from a v67 image
    TEST_ASSERT_EQUAL_UINT8(0, module.lpcomp_wake_ref);
}

void test_noteLoadedVault_records_version_and_applies_v69()
{
    TestNexusModule module;
    meshtastic::NexusModule::NexusVault v = {};
    v.version = 69;
    strncpy(v.ping_trigger, "!ping", sizeof(v.ping_trigger) - 1);

    module.noteLoadedVault(v);

    TEST_ASSERT_EQUAL_UINT32(69, module.vaultVersionLoaded);
    TEST_ASSERT_EQUAL_STRING("!ping", module.ping_trigger);
}

void test_noteLoadedVault_defaults_ping_trigger_for_v68()
{
    TestNexusModule module;
    meshtastic::NexusModule::NexusVault v = {};
    v.version = 68;

    module.noteLoadedVault(v);

    TEST_ASSERT_EQUAL_UINT32(68, module.vaultVersionLoaded);
    TEST_ASSERT_EQUAL_STRING("/ping", module.ping_trigger);
}

// v71 splits the PPING header mode per layer. MeshCore ships with none of its own banner; Meshtastic keeps its Derpy
// branding unless a custom header is set.
void test_v71_vault_restores_each_layer_header_mode()
{
    TestNexusModule module;
    meshtastic::NexusModule::NexusVault v = {};
    v.version = 71;
    v.ping_header_modes = (uint8_t)(meshtastic::PING_HEADER_NONE | (meshtastic::PING_HEADER_CUSTOM << 2));
    strncpy(v.pping_header, "My Ping", sizeof(v.pping_header) - 1);

    module.noteLoadedVault(v);

    TEST_ASSERT_EQUAL_UINT8(meshtastic::PING_HEADER_NONE, module.mt_ping_header_mode);
    TEST_ASSERT_EQUAL_UINT8(meshtastic::PING_HEADER_CUSTOM, module.mc_ping_header_mode);
    // The vault flag older builds read stays a mirror of the Meshtastic mode, not of MeshCore's.
    TEST_ASSERT_FALSE(module.custom_pping_active);
    TEST_ASSERT_EQUAL_STRING("My Ping", module.pping_header);
}

// An older vault only carries the shared custom_pping_active flag: Meshtastic honours it, and MeshCore keeps a custom
// header that was in use instead of dropping it, falling back to no banner (its factory default) when none was set.
void test_pre_v71_vault_maps_the_shared_flag_onto_both_layers()
{
    TestNexusModule custom;
    meshtastic::NexusModule::NexusVault v = {};
    v.version = 70;
    v.custom_pping_active = 1;
    custom.noteLoadedVault(v);
    TEST_ASSERT_EQUAL_UINT8(meshtastic::PING_HEADER_CUSTOM, custom.mt_ping_header_mode);
    TEST_ASSERT_EQUAL_UINT8(meshtastic::PING_HEADER_CUSTOM, custom.mc_ping_header_mode);

    TestNexusModule plain;
    meshtastic::NexusModule::NexusVault w = {};
    w.version = 70;
    plain.noteLoadedVault(w);
    TEST_ASSERT_EQUAL_UINT8(meshtastic::PING_HEADER_DEFAULT, plain.mt_ping_header_mode);
    TEST_ASSERT_EQUAL_UINT8(meshtastic::PING_HEADER_NONE, plain.mc_ping_header_mode);
}

void test_migrates_v67_vault_on_router_role()
{
    TestNexusModule module;
    config.device.role = meshtastic_Config_DeviceConfig_Role_ROUTER;

    meshtastic::NexusModule::NexusVault v = {};
    v.version = 67;
    module.noteLoadedVault(v);

    module.zero_hop_config = 0;
    module.applyZeroHopRoleDefaults(module.vaultVersionLoaded >= 68);

    constexpr uint8_t EXPECTED_ZH = 0x01 | 0x08 | 0x10;
    TEST_ASSERT_EQUAL_UINT8(EXPECTED_ZH, module.zero_hop_config & EXPECTED_ZH);
    TEST_ASSERT_EQUAL_INT(1, module.saveVaultCallCount);
    // Vault version in RAM must be updated to 69 once migration succeeds
    TEST_ASSERT_EQUAL_UINT32(69, module.vaultVersionLoaded);
}

void test_v68_vault_on_router_preserves_operator_choice()
{
    TestNexusModule module;
    config.device.role = meshtastic_Config_DeviceConfig_Role_ROUTER;

    meshtastic::NexusModule::NexusVault v = {};
    v.version = 68;
    module.noteLoadedVault(v);

    // Operator explicitly turned off Zero-Hop
    module.zero_hop_config = 0;
    module.applyZeroHopRoleDefaults(module.vaultVersionLoaded >= 68);

    TEST_ASSERT_EQUAL_UINT8(0, module.zero_hop_config);
    TEST_ASSERT_EQUAL_INT(0, module.saveVaultCallCount);
    TEST_ASSERT_EQUAL_UINT32(68, module.vaultVersionLoaded);
}

void test_v67_vault_on_client_role_does_not_migrate()
{
    TestNexusModule module;
    config.device.role = meshtastic_Config_DeviceConfig_Role_CLIENT;

    meshtastic::NexusModule::NexusVault v = {};
    v.version = 67;
    module.noteLoadedVault(v);

    module.zero_hop_config = 0;
    module.applyZeroHopRoleDefaults(module.vaultVersionLoaded >= 68);

    TEST_ASSERT_EQUAL_UINT8(0, module.zero_hop_config);
    TEST_ASSERT_EQUAL_INT(0, module.saveVaultCallCount);
    TEST_ASSERT_EQUAL_UINT32(67, module.vaultVersionLoaded);
}

void test_no_vault_applies_defaults_in_ram_without_saving()
{
    TestNexusModule module;
    config.device.role = meshtastic_Config_DeviceConfig_Role_ROUTER;

    // No vault loaded
    module.vaultVersionLoaded = 0;
    module.zero_hop_config = 0;

    module.applyZeroHopRoleDefaults(module.vaultVersionLoaded >= 68);

    constexpr uint8_t EXPECTED_ZH = 0x01 | 0x08 | 0x10;
    TEST_ASSERT_EQUAL_UINT8(EXPECTED_ZH, module.zero_hop_config & EXPECTED_ZH);
    // Must NOT write to flash when there is no vault on disk
    TEST_ASSERT_EQUAL_INT(0, module.saveVaultCallCount);
    TEST_ASSERT_EQUAL_UINT32(0, module.vaultVersionLoaded);
}

// Unit test for matchTargetedCmd() prefix dispatch.
//
// What is under test:
//   - matchTargetedCmd() recognizes `/nexus` and `/mtc` as valid prefix roots (case-insensitively).
//   - matchTargetedCmd() rejects legacy `/jt`, `/jent`, and `/jentastic` prefix roots.
//
// Why that behavior is required:
//   - Nexus command CLI now standardizes on `/nexus` and `/mtc`, and all legacy Jentastic prefixes
//     must be ignored to prevent unintended execution or ambiguous alias routing.
//
// The regression guarded:
//   - If `/mtc` is missing or case-sensitivity breaks, nodes ignore targeted /mtc commands.
//   - If `/jt` roots remain active, obsolete prefix callers continue executing against deprecated roots.
void test_nexus_prefix_roots_mtc_and_nexus()
{
    TestNexusModule module;
    strncpy(owner.short_name, "TEST", sizeof(owner.short_name));

    size_t kind = 0;
    const char *cmd = module.matchTargetedCmd("/nexus-node TEST INFO", &kind);
    TEST_ASSERT_NOT_NULL(cmd);
    TEST_ASSERT_EQUAL_STRING("INFO", cmd);
    TEST_ASSERT_EQUAL_size_t(TestNexusModule::TARGET_NAME, kind);

    cmd = module.matchTargetedCmd("/mtc-node TEST INFO", &kind);
    TEST_ASSERT_NOT_NULL(cmd);
    TEST_ASSERT_EQUAL_STRING("INFO", cmd);
    TEST_ASSERT_EQUAL_size_t(TestNexusModule::TARGET_NAME, kind);

    cmd = module.matchTargetedCmd("/MTC-NODE TEST INFO", &kind);
    TEST_ASSERT_NOT_NULL(cmd);
    TEST_ASSERT_EQUAL_STRING("INFO", cmd);
    TEST_ASSERT_EQUAL_size_t(TestNexusModule::TARGET_NAME, kind);

    cmd = module.matchTargetedCmd("/mcore-node TEST INFO", &kind);
    TEST_ASSERT_NOT_NULL(cmd);
    TEST_ASSERT_EQUAL_STRING("INFO", cmd);
    TEST_ASSERT_EQUAL_size_t(TestNexusModule::TARGET_NAME, kind);

    cmd = module.matchTargetedCmd("/jt-node TEST INFO", &kind);
    TEST_ASSERT_NULL(cmd);

    cmd = module.matchTargetedCmd("/jent-node TEST INFO", &kind);
    TEST_ASSERT_NULL(cmd);

    cmd = module.matchTargetedCmd("/jentastic-node TEST INFO", &kind);
    TEST_ASSERT_NULL(cmd);
}

} // namespace

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_noteLoadedVault_records_version_and_applies_v68);
    RUN_TEST(test_noteLoadedVault_records_version_without_applying_v68_fields_for_older_vault);
    RUN_TEST(test_noteLoadedVault_records_version_and_applies_v69);
    RUN_TEST(test_noteLoadedVault_defaults_ping_trigger_for_v68);
    RUN_TEST(test_v71_vault_restores_each_layer_header_mode);
    RUN_TEST(test_pre_v71_vault_maps_the_shared_flag_onto_both_layers);
    RUN_TEST(test_migrates_v67_vault_on_router_role);
    RUN_TEST(test_v68_vault_on_router_preserves_operator_choice);
    RUN_TEST(test_v67_vault_on_client_role_does_not_migrate);
    RUN_TEST(test_no_vault_applies_defaults_in_ram_without_saving);
    RUN_TEST(test_nexus_prefix_roots_mtc_and_nexus);
    exit(UNITY_END());
}

void loop() {}
