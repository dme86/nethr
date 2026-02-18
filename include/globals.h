#ifndef H_GLOBALS
#define H_GLOBALS

#include <stdint.h>
#include <unistd.h>

#ifdef ESP_PLATFORM
  #define WIFI_SSID "your-ssid"
  #define WIFI_PASS "your-password"
  void task_yield ();
#else
  #define task_yield();
#endif

#define true 1
#define false 0

// TCP port, Minecraft's default is 25565
#ifndef PORT
  #define PORT 25565
#endif

// Player records kept in memory (not concurrent sessions).
// Previously seen players still occupy slots while offline.
#ifndef MAX_PLAYERS
  #define MAX_PLAYERS 16
#endif

// How many mobs to allocate memory for
#ifndef MAX_MOBS
  #define MAX_MOBS (MAX_PLAYERS / 2)
#endif

// Global cap for villager-like trading NPCs
#ifndef MAX_VILLAGERS
  #define MAX_VILLAGERS 12
#endif

// Manhattan distance at which mobs despawn
#ifndef MOB_DESPAWN_DISTANCE
  #define MOB_DESPAWN_DISTANCE 256
#endif

// Server game mode: 0 - survival; 1 - creative; 2 - adventure; 3 - spectator
#ifndef GAMEMODE
  #define GAMEMODE 0
#endif

// Max render distance, determines how many chunks to send
#ifndef VIEW_DISTANCE
  #define VIEW_DISTANCE 2
#endif

// Tick interval in microseconds (default 1s).
#ifndef TIME_BETWEEN_TICKS
  #define TIME_BETWEEN_TICKS 1000000
#endif

// Average passive spawn chance for newly discovered chunks (1 / N)
#ifndef PASSIVE_SPAWN_CHANCE
  #define PASSIVE_SPAWN_CHANCE 6
#endif

// World-space offset of the lightweight "Nether zone".
// This avoids full multi-dimension state while still allowing nether gameplay.
#ifndef NETHER_ZONE_OFFSET
  #define NETHER_ZONE_OFFSET 16384
#endif

// Protocol entity type IDs from 1.21.11 minecraft:entity_type registry.
// Keep these centralized to avoid client-side model mismatches when Mojang
// shifts registry ordering between protocol versions.
#define ENTITY_TYPE_CHICKEN 26
#define ENTITY_TYPE_COW 30
#define ENTITY_TYPE_PIG 100
#define ENTITY_TYPE_SHEEP 111
#define ENTITY_TYPE_VILLAGER 139
#define ENTITY_TYPE_ZOMBIE 150

// Calculated from TIME_BETWEEN_TICKS
#define TICKS_PER_SECOND ((float)1000000 / TIME_BETWEEN_TICKS)

// Initial terrain/biome seed, hashed at startup.
#ifndef INITIAL_WORLD_SEED
  #define INITIAL_WORLD_SEED 0xA103DE6C
#endif

// Initial gameplay RNG seed, hashed at startup.
#ifndef INITIAL_RNG_SEED
  #define INITIAL_RNG_SEED 0xE2B9419
#endif

// Size of each interpolated terrain area; prefer powers of two.
#ifndef CHUNK_SIZE
  #define CHUNK_SIZE 8
#endif

// Baseline terrain elevation.
#ifndef TERRAIN_BASE_HEIGHT
  #define TERRAIN_BASE_HEIGHT 60
#endif

// Cave generation Y level
#ifndef CAVE_BASE_DEPTH
  #define CAVE_BASE_DEPTH 24
#endif

// Biome span in multiples of CHUNK_SIZE; prefer powers of two.
#define BIOME_SIZE (CHUNK_SIZE * 8)

// Calculated from BIOME_SIZE
#define BIOME_RADIUS (BIOME_SIZE / 2)

// Per-player recently visited chunk history.
// Chunks in this window are not re-sent on movement updates.
// Must be at least 1.
#ifndef VISITED_HISTORY
  #define VISITED_HISTORY 4
#endif

// Maximum persisted player block changes.
#ifndef MAX_BLOCK_CHANGES
  #define MAX_BLOCK_CHANGES 20000
#endif

// World decoration tuning (overridable via -D... build flags).
// Values are probabilities in [0..255] for per-column decorators.
#ifndef WORLDGEN_TREE_EDGE_MARGIN
  #define WORLDGEN_TREE_EDGE_MARGIN 1
#endif
#ifndef WORLDGEN_PLAINS_GRASS_CHANCE
  #define WORLDGEN_PLAINS_GRASS_CHANCE 72
#endif
#ifndef WORLDGEN_PLAINS_FLOWER_CHANCE
  #define WORLDGEN_PLAINS_FLOWER_CHANCE 18
#endif
#ifndef WORLDGEN_PLAINS_PUMPKIN_CHANCE
  #define WORLDGEN_PLAINS_PUMPKIN_CHANCE 2
#endif
#ifndef WORLDGEN_DESERT_DEAD_BUSH_CHANCE
  #define WORLDGEN_DESERT_DEAD_BUSH_CHANCE 20
#endif
#ifndef WORLDGEN_SNOWY_GRASS_CHANCE
  #define WORLDGEN_SNOWY_GRASS_CHANCE 28
#endif
#ifndef WORLDGEN_SWAMP_GRASS_CHANCE
  #define WORLDGEN_SWAMP_GRASS_CHANCE 40
#endif

// Terrain shape tuning (overridable via -D... build flags).
// Values define amplitude/rarity for coherent macro terrain.
#ifndef WORLDGEN_HEIGHT_CAP
  #define WORLDGEN_HEIGHT_CAP 160
#endif
#ifndef WORLDGEN_ROLLING_AMPLITUDE
  #define WORLDGEN_ROLLING_AMPLITUDE 8
#endif
#ifndef WORLDGEN_HILL_AMPLITUDE
  #define WORLDGEN_HILL_AMPLITUDE 7
#endif
#ifndef WORLDGEN_VALLEY_DEPTH
  #define WORLDGEN_VALLEY_DEPTH 12
#endif
#ifndef WORLDGEN_MOUNTAIN_AMPLITUDE
  #define WORLDGEN_MOUNTAIN_AMPLITUDE 34
#endif
#ifndef WORLDGEN_MOUNTAIN_THRESHOLD
  #define WORLDGEN_MOUNTAIN_THRESHOLD 74
#endif
#ifndef WORLDGEN_CONTINENT_SCALE
  #define WORLDGEN_CONTINENT_SCALE 64
#endif
#ifndef WORLDGEN_EROSION_SCALE
  #define WORLDGEN_EROSION_SCALE 64
#endif
#ifndef WORLDGEN_RIDGE_SCALE
  #define WORLDGEN_RIDGE_SCALE 16
#endif
#ifndef WORLDGEN_MOUNTAIN_CONTINENT_MIN
  #define WORLDGEN_MOUNTAIN_CONTINENT_MIN 60
#endif
#ifndef WORLDGEN_MOUNTAIN_EROSION_MAX
  #define WORLDGEN_MOUNTAIN_EROSION_MAX 50
#endif
#ifndef WORLDGEN_VALLEY_CONTINENT_MAX
  #define WORLDGEN_VALLEY_CONTINENT_MAX 48
#endif
#ifndef WORLDGEN_VALLEY_EROSION_MIN
  #define WORLDGEN_VALLEY_EROSION_MIN 58
#endif

// Enables synchronous world persistence to disk/flash.
// Runtime state stays in memory; disk is read on startup and written on updates.
// Disabled by default on ESP because flash writes are expensive.
#ifndef ESP_PLATFORM
  #define SYNC_WORLD_TO_DISK
#endif

// Minimum interval for periodic disk flushes (microseconds).
// Applies to player data by default; block changes can opt in below.
#define DISK_SYNC_INTERVAL 15000000

// Flush block changes on interval instead of per-change writes.
// #define DISK_SYNC_BLOCKS_ON_INTERVAL

// Socket progress timeout in microseconds.
#define NETWORK_TIMEOUT_TIME 15000000

// Size of the receive buffer for incoming string data
#define MAX_RECV_BUF_LEN 256

// Sends server brand string to clients (debug screen / F3).
// Brand text is defined in src/globals.c.
#define SEND_BRAND

// Debug mode: send only the initial play login packet after configuration.
// Useful to isolate login decode issues before spawn/chunk packets.
// #define DEBUG_LOGIN_ONLY

// Verbose registry/tag dump in configuration phase.
// #define DEBUG_REGISTRY_VERBOSE

// Temporary compatibility mode while using the Notchian chunk template.
// Spawns players high above terrain and forces daytime visibility.
#define CHUNK_TEMPLATE_VISIBILITY_COMPAT

// Rebroadcast all movement packets immediately, independent of tick rate.
// Improves smoothness on low tick rates at higher network cost.
#define BROADCAST_ALL_MOVEMENT

// Scale movement rebroadcast cadence by active player count.
// Reduces bandwidth but can make movement appear jittery.
#define SCALE_MOVEMENT_UPDATES_TO_PLAYER_COUNT

// Simulate fluid flow near block updates.
#define DO_FLUID_FLOW

// Enable chest interaction and persistence.
// Each chest consumes 15 block-change slots and adds bookkeeping overhead.
#define ALLOW_CHESTS

// Enable flight for all players.
// #define ENABLE_PLAYER_FLIGHT

// Enable item pickup animation on block break.
// Items are still inserted directly into inventory.
// Temporarily disabled in current 1.21.11 compatibility mode because
// add_entity encoding is not finalized yet.
// #define ENABLE_PICKUP_ANIMATION

// If defined, players are able to receive damage from nearby cacti.
#define ENABLE_CACTUS_DAMAGE

// Log unrecognized packet IDs.
// #define DEV_LOG_UNKNOWN_PACKETS

// Log packet parse length mismatches.
#define DEV_LOG_LENGTH_DISCREPANCY

// Log chunk generation timings.
// #define DEV_LOG_CHUNK_GENERATION

// Enable unauthenticated raw world dump/import commands (0xBEEF / 0xFEED).
// #define DEV_ENABLE_BEEF_DUMPS

#define STATE_NONE 0
#define STATE_STATUS 1
#define STATE_LOGIN 2
#define STATE_TRANSFER 3
#define STATE_CONFIGURATION 4
#define STATE_PLAY 5

extern ssize_t recv_count;
extern uint8_t recv_buffer[MAX_RECV_BUF_LEN];

extern uint32_t world_seed;
extern uint32_t rng_seed;
extern uint32_t world_seed_raw;
extern uint32_t rng_seed_raw;

extern short world_spawn_x;
extern uint8_t world_spawn_y;
extern short world_spawn_z;
extern uint8_t world_spawn_locked;

extern uint16_t world_time;
extern uint32_t server_ticks;

extern char motd[];
extern uint8_t motd_len;

#ifdef SEND_BRAND
  extern char brand[];
  extern uint8_t brand_len;
#endif

extern uint16_t client_count;

typedef struct {
  short x;
  short z;
  uint8_t y;
  uint8_t block;
} BlockChange;

#pragma pack(push, 1)

typedef struct {
  uint8_t uuid[16];
  char name[16];
  int client_fd;
  short x;
  uint8_t y;
  short z;
  short visited_x[VISITED_HISTORY];
  short visited_z[VISITED_HISTORY];
  #ifdef SCALE_MOVEMENT_UPDATES_TO_PLAYER_COUNT
    uint16_t packets_since_update;
  #endif
  int8_t yaw;
  int8_t pitch;
  uint8_t grounded_y;
  uint8_t health;
  uint8_t hunger;
  uint16_t saturation;
  uint8_t hotbar;
  uint16_t inventory_items[41];
  uint16_t craft_items[9];
  uint8_t inventory_count[41];
  uint8_t craft_count[9];
  // Multi-purpose 16-bit field; meaning depends on flags.
  // With no special flags, stores cursor item ID.
  uint16_t flagval_16;
  // Multi-purpose 8-bit field; meaning depends on flags.
  // With no special flags, stores cursor item count.
  uint8_t flagval_8;
  // 0x01: attack cooldown (uses flagval_8 timer)
  // 0x02: not spawned yet
  // 0x04: sneaking
  // 0x08: sprinting
  // 0x10: eating (uses flagval_16 timer)
  // 0x20: client loading (uses flagval_16 fallback timer)
  // 0x40: movement update cooldown
  // 0x80: craft_items lock (pointer storage)
  uint8_t flags;
} PlayerData;

typedef struct {
  uint8_t type;
  short x;
  // When health is zero, y stores despawn timer.
  uint8_t y;
  short z;
  // Bits 0-4: health
  // Bit 5: sheep sheared flag (unused for other mobs)
  // Bits 6-7: panic timer
  uint8_t data;
} MobData;

#pragma pack(pop)

union EntityDataValue {
  uint8_t byte;
  int pose;
};

typedef struct {
  uint8_t index;
  // 0 = Byte, 21 = Pose
  int type;
  union EntityDataValue value;
} EntityData;

extern BlockChange block_changes[MAX_BLOCK_CHANGES];
extern int block_changes_count;

extern PlayerData player_data[MAX_PLAYERS];
extern int player_data_count;

extern MobData mob_data[MAX_MOBS];
extern uint8_t villager_job[MAX_MOBS];
extern uint8_t villager_level[MAX_MOBS];
extern uint8_t villager_xp[MAX_MOBS];

#endif
