# Command table (phase 2, not implemented)

Phase 1 replaced the `/nexus`, `/mcore`, `/nl` and `/ml` roots with `/mt`, `/mc`, `/mtc` and the local `/mtl`, `/mcl`,
`/mtcl`. The roots live in `cmdRoots()` and `applyCommandScope()` (`src/modules/NexusCommandParse.h`). `/mc` prepends
`MCORE`, `/mt` refuses the `MESHCORE`/`MCORE` family, `/mtc` runs everything. That is a minimal filter: no command carries
metadata saying which radio it belongs to.

This note records what a full redesign would look like, in case it is worth doing one day.

## Idea

A central table replaces the ~256 `strcasecmp` branches of `handleNexusCommand` (`NexusCommands.cpp`) and the duplicated
help text:

```cpp
struct CmdEntry {
    const char *name;
    const char *alias;   // may be null
    CmdScope scope;      // SCOPE_MT, SCOPE_MC or SCOPE_BOTH
    const char *help;
    void (NexusModule::*handler)(const meshtastic_MeshPacket &, uint32_t replyId, const char *args);
};
```

- `/mt` and `/mc` would filter by the real scope of each command instead of only refusing `MESHCORE`.
- The `CmdScope` enum and the root table from phase 1 are reused unchanged.
- `/help` would be generated from the table, so a command and its help can no longer drift apart.

## Size

Estimates, not measured:

- The help text is ~8.7 KB of `appendPaging` strings plus ~60 KB of `MESHTACORE…` reply strings today. Generating help from the
  table is where flash could be saved (roughly 3-8 KB).
- The table itself costs a pointer, a scope and a handler per command. With hundreds of tiny commands it can come out equal or
  larger than the `if/else` chain. Measure before committing to it.
- The sure benefit is maintainability, not size.

## Plan

1. Branch `refactor/command-table` from `main`.
2. Migrate by family: MeshCore first (~16 branches in `NexusMeshtacoreCommands.cpp`), then the rest.
3. After each family, build the ThinkNode M1 slim env (it sits at ~96 % flash) and compare the binary size. Stop if it does
   not go down.
4. Keep the native test suite green: it has 13 known failures, so compare against that baseline rather than against zero.

## Out of scope even here

Flash file names (`/nexus.bin`, `/nexus_rescue.bin`) and the `nexus_*` protobuf fields stay as they are: renaming them would
orphan data already stored on devices.
