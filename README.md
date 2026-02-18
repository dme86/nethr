# nethr
Lightweight Minecraft server focused on constrained systems.

`nethr` is designed for environments where memory and CPU budget are limited (for example ESP32-class targets). Project priorities are:

1. Memory usage
2. Runtime performance
3. Feature breadth

Vanilla protocol compatibility is implemented pragmatically, not as a strict goal.

- Minecraft version: `1.21.11`
- Protocol version: `774`

> [!WARNING]
> The vanilla client is the supported baseline. Modded clients (for example Fabric-based stacks) may behave unpredictably.

## Quick Start
For x86_64 hosts, run a prebuilt release binary or build from source using the steps below.

The release artifact is a Cosmopolitan binary and is intended to run on Linux, Windows, and many macOS setups.

## Build From Source
Registry data must be generated from an official Minecraft server JAR before compiling.

### Toolchain (asdf)
- Required tools are pinned in `.tool-versions`:
  - `nodejs` (registry processing)
  - `java` (Notchian data generation, Java 21+)
  - `gcc` (native build + lint)
- Bootstrap everything with:
  - `make asdf-install`
- Verify setup:
  - `make asdf-check`

### Makefile workflow (recommended)
- `make all` downloads `server.jar` if missing, generates registries, and builds.
- `make build` builds using existing generated registries.
- Build-time overrides are supported via `EXTRA_CPPFLAGS`:
  - Example: `make build EXTRA_CPPFLAGS="-DMAX_PLAYERS=32 -DMAX_MOBS=24 -DVIEW_DISTANCE=3"`
- `make lint` runs compile-time lint checks for critical C issues.
- `make doctor` runs toolchain + lint checks.
- `make clean` removes build outputs and generated registry artifacts.
- `make world-reset` deletes `world.bin` for a fresh world/player state.
- `make world-regen` resets `world.bin` + `world.meta` and writes fresh seeds (`SEED=`/`RNG_SEED=` optional).
- `make template-refresh` captures chunk templates from a running Notchian server (default `127.0.0.1:25566`).

Generated artifacts (`include/registries.h`, `src/registries.c`) and the local `notchian/` workspace are intentionally not tracked in git.

### Platform notes
- Linux: use the asdf workflow above, then run `make all`.
- ESP targets: use PlatformIO with ESP-IDF (not Arduino), then apply project-specific configuration.

## Configuration
Primary configuration lives in:
- `include/globals.h` (networking, limits, gameplay toggles)
- `src/globals.c` (runtime defaults such as MOTD/time)

Common tuning options:
- Movement broadcast load: disable `BROADCAST_ALL_MOVEMENT` and/or `SCALE_MOVEMENT_UPDATES_TO_PLAYER_COUNT` if network overhead is high.
- Stability toggles: disable `ALLOW_CHESTS` or `DO_FLUID_FLOW` if needed on weaker hardware.
- Chunk revisit behavior: increase `VISITED_HISTORY` to reduce repeated regeneration under constrained conditions.
- World density can be tuned at build time, e.g.:
  - `make build EXTRA_CPPFLAGS="-DWORLDGEN_PLAINS_GRASS_CHANCE=96 -DWORLDGEN_PLAINS_FLOWER_CHANCE=28 -DWORLDGEN_TREE_EDGE_MARGIN=0"`
  - `make build EXTRA_CPPFLAGS="-DMAX_PLAYERS=32 -DMAX_MOBS=24 -DPASSIVE_SPAWN_CHANCE=4"`

Runtime chunk pipeline:
- Default: use procedural chunk generation (better biome continuity, less repetition).
- Optional template mode: use Notchian-captured templates for compatibility testing.
  - `NETHR_ENABLE_TEMPLATE_CHUNKS=1 make run`

## Admin System Chat Pipe (Linux)
On Linux builds, nethr creates:

- `/tmp/nethr-admin.pipe`

Each newline-terminated line written to this FIFO is broadcast to all online players as a red system chat message prefixed with `[SYSTEM]`.

Example:

```sh
printf 'Backup erfolgreich: 1.4G\n' > /tmp/nethr-admin.pipe
```

The FIFO is stream-based (not a regular text file): once consumed by the server, data is not retained.

## Persistence (Optional)
On PC builds, world/player state is persisted to `world.bin` by default.
World seed + selected spawn are persisted to `world.meta`.

For ESP-class deployments:
- Configure LittleFS and enable `SYNC_WORLD_TO_DISK` in `include/globals.h`.
- Consider `DISK_SYNC_BLOCKS_ON_INTERVAL` to reduce blocking writes.
- Adjust `MAX_BLOCK_CHANGES` based on partition size.

Development-only fallback:
- `DEV_ENABLE_BEEF_DUMPS` enables raw world dump/upload over TCP.
- This mode has no authentication and should not be exposed to untrusted networks.

## Reference
- Java protocol documentation: https://minecraft.wiki/w/Java_Edition_protocol/Packets

## Worldgen Status
- Biomes now use deterministic multi-octave 2D noise in minichunk space.
- Current overworld biome set:
  - `minecraft:plains`
  - `minecraft:mangrove_swamp`
  - `minecraft:desert`
  - `minecraft:snowy_plains`
  - `minecraft:beach` (coastline band)
- Spawn area near origin is biased to plains for more playable starts.

## Test Commands
1. Reset world quickly:
   - `make world-regen`
   - `make run`
2. Reset world with a fixed, reproducible seed:
   - `make world-regen SEED=123456789`
   - `make run`
3. Refresh templates (if Notchian probe server is running on `127.0.0.1:25566`):
   - `REFRESH_TEMPLATES=1 make world-regen`
4. Validate procedural chunk path:
   - `make world-regen`
   - `make run`
   - Check for `Chunk encoder v8` in server logs.
