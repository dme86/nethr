#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef _WIN32
#include <winsock2.h>
#endif

#include "globals.h"
#include "tools.h"
#include "varnum.h"
#include "packets.h"
#include "registries.h"
#include "worldgen.h"
#include "structures.h"
#include "serialize.h"
#include "procedures.h"

int client_states[MAX_PLAYERS * 2];

enum VillagerJob {
  VJ_FARMER = 0,
  VJ_LIBRARIAN = 1,
  VJ_TOOLSMITH = 2
};

static const char *villagerJobName (uint8_t job) {
  if (job == VJ_FARMER) return "Farmer";
  if (job == VJ_LIBRARIAN) return "Librarian";
  if (job == VJ_TOOLSMITH) return "Toolsmith";
  return "Villager";
}

static uint8_t isInNetherZone (short z) {
  return z >= NETHER_ZONE_OFFSET;
}

static const char *spawnBiomeName (uint8_t biome) {
  switch (biome) {
    case W_plains: return "plains";
    case W_mangrove_swamp: return "mangrove_swamp";
    case W_desert: return "desert";
    case W_snowy_plains: return "snowy_plains";
    case W_beach: return "beach";
    default: return "unknown";
  }
}

static uint8_t isSpawnColumnSafe (int x, int y, int z) {
  if (y < 1 || y > WORLDGEN_HEIGHT_CAP) return false;
  uint8_t below = getBlockAt(x, y - 1, z);
  uint8_t feet = getBlockAt(x, y, z);
  uint8_t head = getBlockAt(x, y + 1, z);
  if (isPassableBlock(below)) return false;
  if (feet != B_air || head != B_air) return false;
  uint8_t n = getBlockAt(x, y, z - 1);
  uint8_t s = getBlockAt(x, y, z + 1);
  uint8_t w = getBlockAt(x - 1, y, z);
  uint8_t e = getBlockAt(x + 1, y, z);
  if (n == B_water || s == B_water || w == B_water || e == B_water) return false;
  if (n == B_lava || s == B_lava || w == B_lava || e == B_lava) return false;
  return true;
}

// Validates that spawn is not only locally safe, but also in a playable land area.
static uint8_t isSpawnAreaPlayable (int x, int y, int z) {
  if (!isSpawnColumnSafe(x, y, z)) return false;

  uint8_t center_biome = getChunkBiome(div_floor(x, CHUNK_SIZE), div_floor(z, CHUNK_SIZE));
  if (center_biome == W_beach) return false;

  int land_cells = 0;
  int water_cells = 0;
  for (int dz = -4; dz <= 4; dz += 2) {
    for (int dx = -4; dx <= 4; dx += 2) {
      int sx = x + dx;
      int sz = z + dz;
      uint8_t h = getHeightAt(sx, sz);
      uint8_t top = getBlockAt(sx, h, sz);
      uint8_t above = getBlockAt(sx, h + 1, sz);
      if (!isPassableBlock(top) && above == B_air && h >= 63) {
        land_cells++;
      } else if (above == B_water || top == B_water) {
        water_cells++;
      }
    }
  }

  // Require enough nearby dry cells and reject clearly oceanic surroundings.
  if (land_cells < 8) return false;
  if (water_cells > 10) return false;
  return true;
}

static uint8_t templateVisibilityCompatEnabled () {
#ifdef CHUNK_TEMPLATE_VISIBILITY_COMPAT
  const char *enable_env = getenv("NETHR_ENABLE_TEMPLATE_CHUNKS");
  return (enable_env != NULL && enable_env[0] == '1');
#else
  return false;
#endif
}

void ensureWorldSpawn () {
  if (world_spawn_locked) {
    uint8_t biome = getChunkBiome(div_floor(world_spawn_x, CHUNK_SIZE), div_floor(world_spawn_z, CHUNK_SIZE));
    if (biome != W_beach && isSpawnAreaPlayable(world_spawn_x, world_spawn_y, world_spawn_z)) return;
    printf(
      "Persisted world spawn invalid (x=%d y=%u z=%d, biome=%s), regenerating...\n",
      world_spawn_x, world_spawn_y, world_spawn_z, spawnBiomeName(biome)
    );
    world_spawn_locked = false;
  }

  uint64_t spawn_pick = splitmix64((((uint64_t)world_seed_raw) << 32) ^ (uint64_t)rng_seed_raw ^ 0x9E3779B97F4A7C15ULL);
  int center_x = ((int)(spawn_pick & 0x3FF) - 512);
  int center_z = ((int)((spawn_pick >> 10) & 0x3FF) - 512);
  if (center_x > -64 && center_x < 64) center_x += (center_x < 0) ? -96 : 96;
  if (center_z > -64 && center_z < 64) center_z += (center_z < 0) ? -96 : 96;
  printf(
    "Spawn search center (seeded): x=%d z=%d raw_pick=0x%08X%08X\n",
    center_x, center_z, (unsigned int)(spawn_pick >> 32), (unsigned int)spawn_pick
  );

  int best_score = -2147483647;
  short best_x = (short)center_x;
  short best_z = (short)center_z;
  uint8_t best_y = getHeightAt(center_x, center_z) + 1;
  uint8_t best_biome = W_plains;
  uint8_t found_candidate = false;

  // Scan a bounded radius around origin and pick a gentle, traversable area.
  for (int radius = 0; radius <= 128; radius += 8) {
    for (int x = -radius; x <= radius; x += 4) {
      for (int z = -radius; z <= radius; z += 4) {
        if (radius > 0 && abs(x) != radius && abs(z) != radius) continue;

        int wx = center_x + x;
        int wz = center_z + z;
        uint8_t y = getHeightAt(wx, wz);
        if (y < 60 || y > 96) continue;

        if (!isSpawnAreaPlayable(wx, y + 1, wz)) continue;

        uint8_t h_n = getHeightAt(wx, wz - 1);
        uint8_t h_s = getHeightAt(wx, wz + 1);
        uint8_t h_w = getHeightAt(wx - 1, wz);
        uint8_t h_e = getHeightAt(wx + 1, wz);
        uint8_t h_min = h_n;
        uint8_t h_max = h_n;
        if (h_s < h_min) h_min = h_s;
        if (h_w < h_min) h_min = h_w;
        if (h_e < h_min) h_min = h_e;
        if (h_s > h_max) h_max = h_s;
        if (h_w > h_max) h_max = h_w;
        if (h_e > h_max) h_max = h_e;
        int slope = (int)h_max - (int)h_min;
        if (slope > 4) continue;

        uint8_t biome = getChunkBiome(div_floor(wx, CHUNK_SIZE), div_floor(wz, CHUNK_SIZE));
        if (biome == W_beach) continue;

        // Reject spots adjacent to water/lava at player feet level.
        uint8_t feet_n = getBlockAt(wx, y + 1, wz - 1);
        uint8_t feet_s = getBlockAt(wx, y + 1, wz + 1);
        uint8_t feet_w = getBlockAt(wx - 1, y + 1, wz);
        uint8_t feet_e = getBlockAt(wx + 1, y + 1, wz);
        if (feet_n == B_water || feet_s == B_water || feet_w == B_water || feet_e == B_water) continue;
        if (feet_n == B_lava || feet_s == B_lava || feet_w == B_lava || feet_e == B_lava) continue;

        int score = 200;
        if (biome == W_plains) score += 220;
        else if (biome == W_snowy_plains) score += 120;
        else if (biome == W_desert) score += 80;
        else if (biome == W_mangrove_swamp) score += 30;
        else if (biome == W_beach) score -= 160;
        score -= slope * 40;
        score -= abs((int)y - 70) * 2;
        score -= radius / 2;

        if (score > best_score) {
          best_score = score;
          best_x = (short)wx;
          best_z = (short)wz;
          best_y = (uint8_t)(y + 1);
          best_biome = biome;
          found_candidate = true;
        }
      }
    }
  }

  if (!found_candidate) {
    // Hard fallback: search a large area and force a land spawn.
    // Phase 1 prefers plains-like starts; phase 2 accepts any non-beach land.
    for (int phase = 0; phase < 2 && !found_candidate; phase++) {
      for (int radius = 16; radius <= 1536 && !found_candidate; radius += 16) {
        for (int x = -radius; x <= radius; x += 4) {
          for (int z = -radius; z <= radius; z += 4) {
            if (abs(x) != radius && abs(z) != radius) continue;
            int wx = center_x + x;
            int wz = center_z + z;
            uint8_t y = getHeightAt(wx, wz);
            if (y < 58 || y > 110) continue;
            if (!isSpawnAreaPlayable(wx, y + 1, wz)) continue;

            uint8_t biome = getChunkBiome(div_floor(wx, CHUNK_SIZE), div_floor(wz, CHUNK_SIZE));
            if (biome == W_beach) continue;
            if (phase == 0 && biome != W_plains && biome != W_snowy_plains) continue;

            best_x = (short)wx;
            best_z = (short)wz;
            best_y = (uint8_t)(y + 1);
            best_biome = biome;
            best_score = 0;
            found_candidate = true;
            break;
          }
        }
      }
    }
  }

  if (!found_candidate) {
    printf(
      "Spawn scan found no land candidate around seeded center; forcing origin fallback scan\n"
    );
    for (int radius = 0; radius <= 1024 && !found_candidate; radius += 16) {
      for (int x = -radius; x <= radius; x += 4) {
        for (int z = -radius; z <= radius; z += 4) {
          if (radius > 0 && abs(x) != radius && abs(z) != radius) continue;
          uint8_t y = getHeightAt(x, z);
          if (!isSpawnAreaPlayable(x, y + 1, z)) continue;
          uint8_t biome = getChunkBiome(div_floor(x, CHUNK_SIZE), div_floor(z, CHUNK_SIZE));
          if (biome == W_beach) continue;
          best_x = (short)x;
          best_z = (short)z;
          best_y = (uint8_t)(y + 1);
          best_biome = biome;
          best_score = -1;
          found_candidate = true;
          break;
        }
      }
    }
  }

  if (!found_candidate) {
    // Last-resort safety net: keep player above water and not inside blocks.
    best_x = 8;
    best_z = 8;
    best_y = getHeightAt(best_x, best_z) + 1;
    while (best_y < WORLDGEN_HEIGHT_CAP && !isSpawnColumnSafe(best_x, best_y, best_z)) best_y++;
    best_biome = getChunkBiome(div_floor(best_x, CHUNK_SIZE), div_floor(best_z, CHUNK_SIZE));
    best_score = -9999;
  }

  world_spawn_x = best_x;
  world_spawn_y = best_y;
  world_spawn_z = best_z;
  world_spawn_locked = true;
  saveWorldMeta();

  printf(
    "Selected world spawn: x=%d y=%u z=%d biome=%s score=%d\n",
    world_spawn_x, world_spawn_y, world_spawn_z, spawnBiomeName(best_biome), best_score
  );
}

#define BLOCK_CHANGE_BUCKETS 1024

static int16_t block_change_bucket_heads[BLOCK_CHANGE_BUCKETS];
static int16_t block_change_next[MAX_BLOCK_CHANGES];
static uint8_t block_change_index_initialized = false;
static uint8_t block_change_index_dirty = true;

static void initBlockChangeIndexStorage () {
  if (block_change_index_initialized) return;
  for (int i = 0; i < BLOCK_CHANGE_BUCKETS; i ++) {
    block_change_bucket_heads[i] = -1;
  }
  for (int i = 0; i < MAX_BLOCK_CHANGES; i ++) {
    block_change_next[i] = -1;
  }
  block_change_index_initialized = true;
}

static uint16_t getBlockChangeBucket (short chunk_x, short chunk_z) {
  uint32_t ux = (uint32_t)(uint16_t)chunk_x;
  uint32_t uz = (uint32_t)(uint16_t)chunk_z;
  uint32_t hash = ux * 73856093u ^ uz * 19349663u;
  return (uint16_t)(hash & (BLOCK_CHANGE_BUCKETS - 1));
}

static void rebuildBlockChangeIndex () {
  initBlockChangeIndexStorage();

  for (int i = 0; i < BLOCK_CHANGE_BUCKETS; i ++) {
    block_change_bucket_heads[i] = -1;
  }
  for (int i = 0; i < block_changes_count; i ++) {
    block_change_next[i] = -1;
  }

  for (int i = 0; i < block_changes_count; i ++) {
    uint8_t block = block_changes[i].block;
    if (block == 0xFF) continue;

    short chunk_x = div_floor(block_changes[i].x, 16);
    short chunk_z = div_floor(block_changes[i].z, 16);
    uint16_t bucket = getBlockChangeBucket(chunk_x, chunk_z);

    block_change_next[i] = block_change_bucket_heads[bucket];
    block_change_bucket_heads[bucket] = i;

    #ifdef ALLOW_CHESTS
      if (block == B_chest) i += 14;
    #endif
  }

  block_change_index_dirty = false;
}

static void ensureBlockChangeIndex () {
  if (!block_change_index_dirty) return;
  rebuildBlockChangeIndex();
}

void invalidateBlockChangeIndex () {
  block_change_index_dirty = true;
}

int firstBlockChangeInChunk (short chunk_x, short chunk_z) {
  ensureBlockChangeIndex();
  return block_change_bucket_heads[getBlockChangeBucket(chunk_x, chunk_z)];
}

int nextIndexedBlockChange (int index) {
  if (index < 0 || index >= block_changes_count) return -1;
  return block_change_next[index];
}

int findBlockChangeIndex (short x, uint8_t y, short z) {
  short chunk_x = div_floor(x, 16);
  short chunk_z = div_floor(z, 16);

  for (int i = firstBlockChangeInChunk(chunk_x, chunk_z); i != -1; i = nextIndexedBlockChange(i)) {
    if (block_changes[i].x == x && block_changes[i].y == y && block_changes[i].z == z) {
      return i;
    }
  }

  return -1;
}

void setClientState (int client_fd, int new_state) {
  // Look for a client state with a matching file descriptor
  for (int i = 0; i < MAX_PLAYERS * 2; i += 2) {
    if (client_states[i] != client_fd) continue;
    client_states[i + 1] = new_state;
    return;
  }
  // If the above failed, look for an unused client state slot
  for (int i = 0; i < MAX_PLAYERS * 2; i += 2) {
    if (client_states[i] != -1) continue;
    client_states[i] = client_fd;
    client_states[i + 1] = new_state;
    return;
  }
}

int getClientState (int client_fd) {
  for (int i = 0; i < MAX_PLAYERS * 2; i += 2) {
    if (client_states[i] != client_fd) continue;
    return client_states[i + 1];
  }
  return STATE_NONE;
}

int getClientIndex (int client_fd) {
  for (int i = 0; i < MAX_PLAYERS * 2; i += 2) {
    if (client_states[i] != client_fd) continue;
    return i;
  }
  return -1;
}

// Resets player runtime state to default spawn values.
void resetPlayerData (PlayerData *player) {
  player->health = 20;
  player->hunger = 20;
  player->saturation = 2500;
  player->x = world_spawn_x;
  player->z = world_spawn_z;
  player->y = world_spawn_y;
  player->flags |= 0x02;
  player->grounded_y = 0;
  for (int i = 0; i < 41; i ++) {
    player->inventory_items[i] = 0;
    player->inventory_count[i] = 0;
  }
  for (int i = 0; i < 9; i ++) {
    player->craft_items[i] = 0;
    player->craft_count[i] = 0;
  }
  player->flags &= ~0x80;
}

// Binds login identity to an existing or free player slot.
int reservePlayerData (int client_fd, uint8_t *uuid, char *name) {

  for (int i = 0; i < MAX_PLAYERS; i ++) {
    // Found existing player entry (UUID match)
    if (memcmp(player_data[i].uuid, uuid, 16) == 0) {
      // Set network file descriptor and username
      player_data[i].client_fd = client_fd;
      memcpy(player_data[i].name, name, 16);
      // Flag player as loading
      player_data[i].flags |= 0x20;
      player_data[i].flagval_16 = 0;
      // Reset their recently visited chunk list
      for (int j = 0; j < VISITED_HISTORY; j ++) {
        player_data[i].visited_x[j] = 32767;
        player_data[i].visited_z[j] = 32767;
      }
      return 0;
    }
    // Search for unallocated player slots
    uint8_t empty = true;
    for (uint8_t j = 0; j < 16; j ++) {
      if (player_data[i].uuid[j] != 0) {
        empty = false;
        break;
      }
    }
    // Found free space for a player, initialize default parameters
    if (empty) {
      if (player_data_count >= MAX_PLAYERS) return 1;
      player_data[i].client_fd = client_fd;
      player_data[i].flags |= 0x20;
      player_data[i].flagval_16 = 0;
      memcpy(player_data[i].uuid, uuid, 16);
      memcpy(player_data[i].name, name, 16);
      resetPlayerData(&player_data[i]);
      player_data_count ++;
      return 0;
    }
  }

  return 1;

}

int getPlayerData (int client_fd, PlayerData **output) {
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    if (player_data[i].client_fd == client_fd) {
      *output = &player_data[i];
      return 0;
    }
  }
  return 1;
}

// Returns player by exact name slice, or NULL when not found.
PlayerData *getPlayerByName (int start_offset, int end_offset, uint8_t *buffer) {
  if (start_offset < 0 || end_offset <= start_offset || end_offset > 256) return NULL;

  int target_len = end_offset - start_offset;
  FOR_EACH_VISIBLE_PLAYER(i) {
    int name_len = strlen(player_data[i].name);
    if (name_len != target_len) continue;
    if (memcmp(player_data[i].name, buffer + start_offset, target_len) == 0) {
      return &player_data[i];
    }
  }
  return NULL;
}


// Handles disconnect cleanup and leave broadcast.
void handlePlayerDisconnect (int client_fd) {
  // Search for a corresponding player in the player data array
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    if (player_data[i].client_fd != client_fd) continue;
    // Mark the player as being offline
    player_data[i].client_fd = -1;
    // Prepare leave message for broadcast
    uint8_t player_name_len = strlen(player_data[i].name);
    strcpy((char *)recv_buffer, player_data[i].name);
    strcpy((char *)recv_buffer + player_name_len, " left the game");
    // Broadcast this player's leave to all other connected clients
    FOR_EACH_VISIBLE_OTHER_PLAYER(j, client_fd) {
      // Send chat message
      sc_systemChat(player_data[j].client_fd, (char *)recv_buffer, 14 + player_name_len);
      // Remove leaving player's entity
      sc_removeEntity(player_data[j].client_fd, client_fd);
    }
    break;
  }
  // Find the client state entry and reset it
  for (int i = 0; i < MAX_PLAYERS * 2; i += 2) {
    if (client_states[i] == client_fd) {
      client_states[i] = -1;
      return;
    }
  }
}

// Finalizes join and announces player to connected clients.
void handlePlayerJoin (PlayerData* player) {

  // Prepare join message for broadcast
  uint8_t player_name_len = strlen(player->name);
  strcpy((char *)recv_buffer, player->name);
  strcpy((char *)recv_buffer + player_name_len, " joined the game");

  // Inform other clients (and the joining client) of the player's name and entity
  FOR_EACH_VISIBLE_PLAYER(i) {
    sc_systemChat(player_data[i].client_fd, (char *)recv_buffer, 16 + player_name_len);
    sc_playerInfoUpdateAddPlayer(player_data[i].client_fd, *player);
    if (player_data[i].client_fd != player->client_fd) {
      sc_spawnEntityPlayer(player_data[i].client_fd, *player);
    }
  }

  // Clear "client loading" flag and fallback timer
  player->flags &= ~0x20;
  player->flagval_16 = 0;

}

void disconnectClient (int *client_fd, int cause) {
  if (*client_fd == -1) return;
  int state = getClientState(*client_fd);
  int saved_errno = errno;
  client_count --;
  setClientState(*client_fd, STATE_NONE);
  handlePlayerDisconnect(*client_fd);

  const char *cause_text = "unknown";
  switch (cause) {
    case -2: cause_text = "send timeout/socket write failure"; break;
    case -1: cause_text = "recv timeout/socket read failure"; break;
    case 1: cause_text = "peek failed or peer closed connection"; break;
    case 2: cause_text = "invalid packet length varint"; break;
    case 3: cause_text = "invalid packet id varint"; break;
    case 4: cause_text = "post-handle recv indicates closed/error socket"; break;
    case 5: cause_text = "legacy ping probe rejected"; break;
    case 6: cause_text = "dev world dump complete"; break;
    case 7: cause_text = "dev world import complete"; break;
    case 8: cause_text = "status ping complete (intentional close)"; break;
  }

  #ifdef _WIN32
  int saved_wsa_errno = WSAGetLastError();
  closesocket(*client_fd);
  printf(
    "Disconnected client %d, cause: %d (%s), state: %d, wsa_before_close: %d, wsa_after_close: %d\n",
    *client_fd, cause, cause_text, state, saved_wsa_errno, WSAGetLastError()
  );
  #else
  close(*client_fd);
  printf(
    "Disconnected client %d, cause: %d (%s), state: %d, errno_before_close: %d (%s), errno_after_close: %d (%s)\n\n",
    *client_fd, cause, cause_text, state,
    saved_errno, strerror(saved_errno),
    errno, strerror(errno)
  );
  #endif
  *client_fd = -1;
}

uint8_t serverSlotToClientSlot (int window_id, uint8_t slot) {

  if (window_id == 0) { // Player inventory

    if (slot < 9) return slot + 36;
    if (slot >= 9 && slot <= 35) return slot;
    if (slot == 40) return 45;
    if (slot >= 36 && slot <= 39) return 44 - slot;
    if (slot >= 41 && slot <= 44) return slot - 40;

  } else if (window_id == 12) { // Crafting table

    if (slot >= 41 && slot <= 49) return slot - 40;
    return serverSlotToClientSlot(0, slot - 1);

  } else if (window_id == 14) { // Furnace

    if (slot >= 41 && slot <= 43) return slot - 41;
    return serverSlotToClientSlot(0, slot + 6);

  }

  return 255;
}

uint8_t clientSlotToServerSlot (int window_id, uint8_t slot) {

  if (window_id == 0) { // Player inventory

    if (slot >= 36 && slot <= 44) return slot - 36;
    if (slot >= 9 && slot <= 35) return slot;
    if (slot == 45) return 40;
    if (slot >= 5 && slot <= 8) return 44 - slot;

    // Map 2x2 crafting UI slots into craft buffer offsets.
    // This relies on adjacent craft/inventory buffers in PlayerData.
    if (slot == 1) return 41;
    if (slot == 2) return 42;
    if (slot == 3) return 44;
    if (slot == 4) return 45;

  } else if (window_id == 12) { // Crafting table

    // 3x3 crafting grid mapped into craft buffer.
    if (slot >= 1 && slot <= 9) return 40 + slot;
    // The rest of the slots are identical, just shifted by one
    if (slot >= 10 && slot <= 45) return clientSlotToServerSlot(0, slot - 1);

  } else if (window_id == 14) { // Furnace

    // Reuse craft buffer for temporary furnace slots.
    // This allows items to be restored on container close.
    if (slot <= 2) return 41 + slot;
    // The rest of the slots are identical, just shifted by 6
    if (slot >= 3 && slot <= 38) return clientSlotToServerSlot(0, slot + 6);

  }
  #ifdef ALLOW_CHESTS
  else if (window_id == 2) { // Chest

    // Temporarily map chest slots into craft buffer.
    // Slot mapping is normalized by container-specific handlers.
    if (slot <= 26) return 41 + slot;
    // The rest of the slots are identical, just shifted by 18
    if (slot >= 27 && slot <= 62) return clientSlotToServerSlot(0, slot - 18);

  }
  #endif

  return 255;
}

int givePlayerItem (PlayerData *player, uint16_t item, uint8_t count) {

  if (item == 0 || count == 0) return 0;

  uint8_t slot = 255;
  uint8_t stack_size = getItemStackSize(item);

  for (int i = 0; i < 41; i ++) {
    if (player->inventory_items[i] == item && player->inventory_count[i] <= stack_size - count) {
      slot = i;
      break;
    }
  }

  if (slot == 255) {
    for (int i = 0; i < 41; i ++) {
      if (player->inventory_count[i] == 0) {
        slot = i;
        break;
      }
    }
  }

  // Fail to assign item if slot is outside of main inventory
  if (slot >= 36) return 1;

  player->inventory_items[slot] = item;
  player->inventory_count[slot] += count;
  sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, slot), player->inventory_count[slot], item);

  return 0;

}

// Sends the full login/play spawn sequence for one player.
void spawnPlayer (PlayerData *player) {

  // Player spawn coordinates, initialized to placeholders
  float spawn_x = (float)world_spawn_x + 0.5f;
  float spawn_y = (float)world_spawn_y;
  float spawn_z = (float)world_spawn_z + 0.5f;
  float spawn_yaw = 0.0f, spawn_pitch = 0.0f;

  if (player->flags & 0x02) { // Is this a new player?
    // Use server-selected world spawn for first login.
    printf(
      "Spawn source: new-player world spawn (x=%d y=%u z=%d)\n",
      world_spawn_x, world_spawn_y, world_spawn_z
    );
    player->x = world_spawn_x;
    player->z = world_spawn_z;
    player->y = world_spawn_y;
    spawn_x = (float)world_spawn_x + 0.5f;
    spawn_y = (float)world_spawn_y;
    spawn_z = (float)world_spawn_z + 0.5f;
    player->flags &= ~0x02;
  } else { // Not a new player
    // Calculate spawn position from player data
    printf(
      "Spawn source: stored player position (x=%d y=%u z=%d)\n",
      player->x, player->y, player->z
    );
    if (!isSpawnAreaPlayable(player->x, player->y, player->z)) {
      printf(
        "Stored player position unsafe (x=%d y=%u z=%d), moving to world spawn (x=%d y=%u z=%d)\n",
        player->x, player->y, player->z,
        world_spawn_x, world_spawn_y, world_spawn_z
      );
      player->x = world_spawn_x;
      player->y = world_spawn_y;
      player->z = world_spawn_z;
    }
    spawn_x = (float)player->x + 0.5;
    spawn_y = player->y;
    spawn_z = (float)player->z + 0.5;
    spawn_yaw = player->yaw * 180 / 127;
    spawn_pitch = player->pitch * 90 / 127;
  }

  #ifdef CHUNK_TEMPLATE_VISIBILITY_COMPAT
    if (templateVisibilityCompatEnabled()) {
      // Template chunks were captured around y~112.
      // Use a stable spawn height only in explicit template mode.
      spawn_y = 112.0f;
      player->y = 112;
    }
  #endif

  // Teleport player to spawn coordinates (first pass)
  printf(
    "Spawn sequence: initial player_position (x=%.2f y=%.2f z=%.2f yaw=%.2f pitch=%.2f)\n",
    spawn_x, spawn_y, spawn_z, spawn_yaw, spawn_pitch
  );
  sc_synchronizePlayerPosition(player->client_fd, spawn_x, spawn_y, spawn_z, spawn_yaw, spawn_pitch);

  task_yield(); // Yield between packet bursts.

  // Clear crafting grid residue, unlock craft_items
  for (int i = 0; i < 9; i++) {
    player->craft_items[i] = 0;
    player->craft_count[i] = 0;
  }
  player->flags &= ~0x80;

  // Sync client inventory and hotbar
  for (uint8_t i = 0; i < 41; i ++) {
    sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, i), player->inventory_count[i], player->inventory_items[i]);
  }
  sc_setHeldItem(player->client_fd, player->hotbar);
  // Sync client health and hunger
  sc_setHealth(player->client_fd, player->health, player->hunger, player->saturation);
  // Sync client clock time
  sc_updateTime(player->client_fd, world_time);

  #ifdef ENABLE_PLAYER_FLIGHT
  if (GAMEMODE != 1 && GAMEMODE != 3) {
    // Grant flight in non-creative/spectator for testing builds.
    sc_playerAbilities(player->client_fd, 0x04);
  }
  #endif

  // Calculate player's chunk coordinates
  short _x = div_floor(player->x, 16), _z = div_floor(player->z, 16);

  // Indicate that we're about to send chunk data
  printf("Spawn sequence: set_default_spawn_position + game_event(wait_chunks) + set_chunk_cache_center\n");
  int default_spawn_y = world_spawn_y;
  #ifdef CHUNK_TEMPLATE_VISIBILITY_COMPAT
    if (templateVisibilityCompatEnabled()) default_spawn_y = 112;
  #endif
  sc_setDefaultSpawnPosition(
    player->client_fd, "minecraft:overworld",
    world_spawn_x, default_spawn_y, world_spawn_z,
    0.0f, 0.0f
  );
  sc_startWaitingForChunks(player->client_fd);
  sc_setCenterChunk(player->client_fd, _x, _z);

  task_yield(); // Yield between packet bursts.

  // Send spawn chunk first
  sc_chunkDataAndUpdateLight(player->client_fd, _x, _z);
  for (int i = -view_distance; i <= view_distance; i ++) {
    for (int j = -view_distance; j <= view_distance; j ++) {
      if (i == 0 && j == 0) continue;
      sc_chunkDataAndUpdateLight(player->client_fd, _x + i, _z + j);
    }
  }
  // Re-teleport player after all chunks have been sent
  sc_synchronizePlayerPosition(player->client_fd, spawn_x, spawn_y, spawn_z, spawn_yaw, spawn_pitch);

  task_yield(); // Yield between packet bursts.

}

// Broadcasts player posture/sprint metadata to other clients.
void broadcastPlayerMetadata (PlayerData *player) {
  uint8_t sneaking = (player->flags & 0x04) != 0;
  uint8_t sprinting = (player->flags & 0x08) != 0;

  uint8_t entity_bit_mask = 0;
  if (sneaking) entity_bit_mask |= 0x02;
  if (sprinting) entity_bit_mask |= 0x08;

  int pose = 0;
  if (sneaking) pose = 5;

  EntityData metadata[] = {
    {
      0,                   // Index (Entity Bit Mask)
      0,                   // Type (Byte)
      { entity_bit_mask }, // Value
    },
    {
      6,        // Index (Pose),
      21,       // Type (Pose),
      { pose }, // Value (Standing)
    }
  };

  FOR_EACH_VISIBLE_OTHER_PLAYER(i, player->client_fd) {
    PlayerData* other_player = &player_data[i];
    int client_fd = other_player->client_fd;

    sc_setEntityMetadata(client_fd, player->client_fd, metadata, 2);
  }
}

// Sends mob metadata to one client, or broadcasts when client_fd == -1.
void broadcastMobMetadata (int client_fd, int entity_id) {

  int mob_index = -entity_id - 2;
  if (mob_index < 0 || mob_index >= MAX_MOBS) return;
  MobData *mob = &mob_data[mob_index];

  EntityData metadata[1];
  size_t length;

  switch (mob->type) {
    case ENTITY_TYPE_SHEEP: // Sheep
      if (!((mob->data >> 5) & 1)) // Don't send metadata if sheep isn't sheared
        return;

      metadata[0] = (EntityData){
        17,                // Index (Sheep Bit Mask),
        0,                 // Type (Byte),
        { (uint8_t)0x10 }, // Value
      };
      length = 1;

      break;

    default: return;
  }

  if (client_fd == -1) {
    FOR_EACH_VISIBLE_PLAYER(i) {
      PlayerData* player = &player_data[i];
      client_fd = player->client_fd;

      sc_setEntityMetadata(client_fd, entity_id, metadata, length);
    }
  } else {
    sc_setEntityMetadata(client_fd, entity_id, metadata, length);
  }
}

uint8_t getBlockChange (short x, uint8_t y, short z) {
  int index = findBlockChangeIndex(x, y, z);
  if (index != -1) return block_changes[index].block;
  return 0xFF;
}

// Handles exhaustion of block_change capacity.
void failBlockChange (short x, uint8_t y, short z, uint8_t block) {

  // Get previous block at this location
  uint8_t before = getBlockAt(x, y, z);

  // Broadcast a new update to all players
  FOR_EACH_VISIBLE_PLAYER(i) {
    // Reset the block they tried to change
    sc_blockUpdate(player_data[i].client_fd, x, y, z, before);
    // Broadcast a chat message warning about the limit
    sc_systemChat(player_data[i].client_fd, "Block changes limit exceeded. Restore original terrain to continue.", 67);
  }

}

uint8_t makeBlockChange (short x, uint8_t y, short z, uint8_t block) {

  // Transmit block update to all in-game clients
  FOR_EACH_VISIBLE_PLAYER(i) {
    sc_blockUpdate(player_data[i].client_fd, x, y, z, block);
  }

  // Calculate terrain at these coordinates and compare it to the input block.
  // Since block changes get overlayed on top of terrain, we don't want to
  // Store blocks that don't differ from the base terrain.
  ChunkAnchor anchor = {
    x / CHUNK_SIZE,
    z / CHUNK_SIZE
  };
  if (x % CHUNK_SIZE < 0) anchor.x --;
  if (z % CHUNK_SIZE < 0) anchor.z --;
  anchor.hash = getChunkHash(anchor.x, anchor.z);
  anchor.biome = getChunkBiome(anchor.x, anchor.z);

  uint8_t is_base_block = block == getTerrainAt(x, y, z, anchor);

  // In the block_changes array, 0xFF indicates a missing/restored entry.
  // We track the position of the first such "gap" for when the operation
  // Isn't replacing an existing block change.
  int first_gap = block_changes_count;

  // Prioritize replacing entries with matching coordinates
  // This prevents having conflicting entries for one set of coordinates
  for (int i = 0; i < block_changes_count; i ++) {
    if (block_changes[i].block == 0xFF) {
      if (first_gap == block_changes_count) first_gap = i;
      continue;
    }
    if (
      block_changes[i].x == x &&
      block_changes[i].y == y &&
      block_changes[i].z == z
    ) {
      #ifdef ALLOW_CHESTS
      // When replacing chests, clear following 14 entries too (item data)
      if (block_changes[i].block == B_chest) {
        for (int j = 1; j < 15; j ++) block_changes[i + j].block = 0xFF;
      }
      #endif
      if (is_base_block) block_changes[i].block = 0xFF;
      else {
        #ifdef ALLOW_CHESTS
        // When placing chests, just unallocate the target block and fall
        // Through to the chest-specific routine below.
        if (block == B_chest) {
          block_changes[i].block = 0xFF;
          if (first_gap > i) first_gap = i;
          #ifndef DISK_SYNC_BLOCKS_ON_INTERVAL
          writeBlockChangesToDisk(i, i);
          #endif
          invalidateBlockChangeIndex();
          break;
        }
        #endif
        block_changes[i].block = block;
      }
      #ifndef DISK_SYNC_BLOCKS_ON_INTERVAL
      writeBlockChangesToDisk(i, i);
      #endif
      invalidateBlockChangeIndex();
      return 0;
    }
  }

  // Don't create a new entry if it contains the base terrain block
  if (is_base_block) return 0;

  #ifdef ALLOW_CHESTS
  if (block == B_chest) {
    // Chests require 15 entries total, so for maximum space-efficiency,
    // We have to find a continuous gap that's at least 15 slots wide.
    // By design, this loop also continues past the current search range,
    // Which naturally appends the chest to the end if a gap isn't found.
    int last_real_entry = first_gap - 1;
    for (int i = first_gap; i <= block_changes_count + 15; i ++) {
      if (i >= MAX_BLOCK_CHANGES) break; // No more space, trigger failBlockChange

      if (block_changes[i].block != 0xFF) {
        last_real_entry = i;
        continue;
      }
      if (i - last_real_entry != 15) continue;
      // A wide enough gap has been found, assign the chest
      block_changes[last_real_entry + 1].x = x;
      block_changes[last_real_entry + 1].y = y;
      block_changes[last_real_entry + 1].z = z;
      block_changes[last_real_entry + 1].block = block;
      // Zero out the following 14 entries for item data
      for (int i = 2; i <= 15; i ++) {
        block_changes[last_real_entry + i].x = 0;
        block_changes[last_real_entry + i].y = 0;
        block_changes[last_real_entry + i].z = 0;
        block_changes[last_real_entry + i].block = 0;
      }
      // Extend future search range if necessary
      if (i >= block_changes_count) {
        block_changes_count = i + 1;
      }
      // Write changes to disk (if applicable)
      #ifndef DISK_SYNC_BLOCKS_ON_INTERVAL
      writeBlockChangesToDisk(last_real_entry + 1, last_real_entry + 15);
      #endif
      invalidateBlockChangeIndex();
      return 0;
    }
    // If we're here, no changes were made
    failBlockChange(x, y, z, block);
    return 1;
  }
  #endif

  // Handles exhaustion of block_change capacity.
  if (first_gap == MAX_BLOCK_CHANGES) {
    failBlockChange(x, y, z, block);
    return 1;
  }

  // Fall back to storing the change at the first possible gap
  block_changes[first_gap].x = x;
  block_changes[first_gap].y = y;
  block_changes[first_gap].z = z;
  block_changes[first_gap].block = block;
  // Write change to disk (if applicable)
  #ifndef DISK_SYNC_BLOCKS_ON_INTERVAL
  writeBlockChangesToDisk(first_gap, first_gap);
  #endif
  // Extend future search range if we've appended to the end
  if (first_gap == block_changes_count) {
    block_changes_count ++;
  }

  invalidateBlockChangeIndex();
  return 0;
}

// Returns the result of mining a block, taking into account the block type and tools
// Probability numbers obtained with this formula: N = floor(P * (2 ^ 32))
uint16_t getMiningResult (uint16_t held_item, uint8_t block) {

  switch (block) {

    case B_oak_leaves:
      if (held_item == I_shears) return I_oak_leaves;
      uint32_t r = fast_rand();
      if (r < 21474836) return I_apple; // 0.5%
      if (r < 85899345) return I_stick; // 2%
      if (r < 214748364) return I_oak_sapling; // 5%
      return 0;
      break;

    case B_stone:
    case B_cobblestone:
    case B_stone_slab:
    case B_cobblestone_slab:
    case B_sandstone:
    case B_furnace:
    case B_coal_ore:
    case B_iron_ore:
    case B_iron_block:
    case B_gold_block:
    case B_diamond_block:
    case B_redstone_block:
    case B_coal_block:
      // Check if player is holding (any) pickaxe
      if (
        held_item != I_wooden_pickaxe &&
        held_item != I_stone_pickaxe &&
        held_item != I_iron_pickaxe &&
        held_item != I_golden_pickaxe &&
        held_item != I_diamond_pickaxe &&
        held_item != I_netherite_pickaxe
      ) return 0;
      break;

    case B_gold_ore:
    case B_redstone_ore:
    case B_diamond_ore:
      // Check if player is holding an iron (or better) pickaxe
      if (
        held_item != I_iron_pickaxe &&
        held_item != I_golden_pickaxe &&
        held_item != I_diamond_pickaxe &&
        held_item != I_netherite_pickaxe
      ) return 0;
      break;

    case B_snow:
      // Check if player is holding (any) shovel
      if (
        held_item != I_wooden_shovel &&
        held_item != I_stone_shovel &&
        held_item != I_iron_shovel &&
        held_item != I_golden_shovel &&
        held_item != I_diamond_shovel &&
        held_item != I_netherite_shovel
      ) return 0;
      break;

    default: break;
  }

  return B_to_I[block];

}

// Rolls a random number to determine whether the player's tool should break
void bumpToolDurability (PlayerData *player) {

  uint16_t held_item = player->inventory_items[player->hotbar];

  // In order to avoid storing durability data, items break randomly with
  // The probability weighted based on vanilla durability.
  uint32_t r = fast_rand();
  if (
    ((held_item == I_wooden_pickaxe || held_item == I_wooden_axe || held_item == I_wooden_shovel) && r < 72796055) ||
    ((held_item == I_stone_pickaxe || held_item == I_stone_axe || held_item == I_stone_shovel) && r < 32786009) ||
    ((held_item == I_iron_pickaxe || held_item == I_iron_axe || held_item == I_iron_shovel) && r < 17179869) ||
    ((held_item == I_golden_pickaxe || held_item == I_golden_axe || held_item == I_golden_shovel) && r < 134217728) ||
    ((held_item == I_diamond_pickaxe || held_item == I_diamond_axe || held_item == I_diamond_shovel) && r < 2751420) ||
    ((held_item == I_netherite_pickaxe || held_item == I_netherite_axe || held_item == I_netherite_shovel) && r < 2114705) ||
    (held_item == I_shears && r < 18046081)
  ) {
    player->inventory_items[player->hotbar] = 0;
    player->inventory_count[player->hotbar] = 0;
    sc_entityEvent(player->client_fd, player->client_fd, 47);
    sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, player->hotbar), 0, 0);
  }

}

// Checks whether the given block would be mined instantly with the held tool
uint8_t isInstantlyMined (PlayerData *player, uint8_t block) {

  uint16_t held_item = player->inventory_items[player->hotbar];

  if (
    block == B_snow ||
    block == B_snow_block
  ) return (
    held_item == I_stone_shovel ||
    held_item == I_iron_shovel ||
    held_item == I_diamond_shovel ||
    held_item == I_netherite_shovel ||
    held_item == I_golden_shovel
  );

  if (block == B_oak_leaves)
    return held_item == I_shears;

  return (
    block == B_dead_bush ||
    block == B_short_grass ||
    block == B_torch ||
    block == B_lily_pad ||
    block == B_oak_sapling
  );

}

// Checks whether the given block has to have something beneath it
uint8_t isColumnBlock (uint8_t block) {
  return (
    block == B_snow ||
    block == B_moss_carpet ||
    block == B_cactus ||
    block == B_short_grass ||
    block == B_dead_bush ||
    block == B_sand ||
    block == B_torch ||
    block == B_oak_sapling
  );
}

// Checks whether the given block is non-solid
uint8_t isPassableBlock (uint8_t block) {
  return (
    block == B_air ||
    (block >= B_water && block < B_water + 8) ||
    (block >= B_lava && block < B_lava + 4) ||
    block == B_snow ||
    block == B_moss_carpet ||
    block == B_short_grass ||
    block == B_dead_bush ||
    block == B_torch
  );
}
// Checks whether the given block is non-solid and spawnable
uint8_t isPassableSpawnBlock (uint8_t block) {
    if ((block >= B_water && block < B_water + 8) ||
        (block >= B_lava && block < B_lava + 4))
    {
        return 0;
    }
    return isPassableBlock(block);
}

// Checks whether the given block can be replaced by another block
uint8_t isReplaceableBlock (uint8_t block) {
  return (
    block == B_air ||
    (block >= B_water && block < B_water + 8) ||
    (block >= B_lava && block < B_lava + 4) ||
    block == B_short_grass ||
    block == B_snow
  );
}

uint8_t isReplaceableFluid (uint8_t block, uint8_t level, uint8_t fluid) {
  if (block >= fluid && block - fluid < 8) {
    return block - fluid > level;
  }
  return isReplaceableBlock(block);
}

// Checks whether the given item can be used in a composter
// Returns the probability (out of 2^32) to return bone meal
uint32_t isCompostItem (uint16_t item) {

  // Output values calculated using the following formula:
  // P = 2^32 / (7 / compost_chance)

  if ( // Compost chance: 30%
    item == I_oak_leaves ||
    item == I_short_grass ||
    item == I_wheat_seeds ||
    item == I_oak_sapling ||
    item == I_moss_carpet
  ) return 184070026;

  if ( // Compost chance: 50%
    item == I_cactus ||
    item == I_sugar_cane
  ) return 306783378;

  if ( // Compost chance: 65%
    item == I_apple ||
    item == I_lily_pad
  ) return 398818392;

  return 0;
}

// Returns the maximum stack size of an item
uint8_t getItemStackSize (uint16_t item) {

  if (
    // Pickaxes
    item == I_wooden_pickaxe ||
    item == I_stone_pickaxe ||
    item == I_iron_pickaxe ||
    item == I_golden_pickaxe ||
    item == I_diamond_pickaxe ||
    item == I_netherite_pickaxe ||
    // Axes
    item == I_wooden_axe ||
    item == I_stone_axe ||
    item == I_iron_axe ||
    item == I_golden_axe ||
    item == I_diamond_axe ||
    item == I_netherite_axe ||
    // Shovels
    item == I_wooden_shovel ||
    item == I_stone_shovel ||
    item == I_iron_shovel ||
    item == I_golden_shovel ||
    item == I_diamond_shovel ||
    item == I_netherite_shovel ||
    // Swords
    item == I_wooden_sword ||
    item == I_stone_sword ||
    item == I_iron_sword ||
    item == I_golden_sword ||
    item == I_diamond_sword ||
    item == I_netherite_sword ||
    // Hoes
    item == I_wooden_hoe ||
    item == I_stone_hoe ||
    item == I_iron_hoe ||
    item == I_golden_hoe ||
    item == I_diamond_hoe ||
    item == I_netherite_hoe ||
    // Shears
    item == I_shears
  ) return 1;

  if (
    item == I_snowball
  ) return 16;

  return 64;
}

// Returns defense points for the given piece of armor
// If the input item is not armor, returns 0
uint8_t getItemDefensePoints (uint16_t item) {

  switch (item) {
    case I_leather_helmet: return 1;
    case I_golden_helmet: return 2;
    case I_iron_helmet: return 2;
    case I_diamond_helmet: // Same as netherite
    case I_netherite_helmet: return 3;
    case I_leather_chestplate: return 3;
    case I_golden_chestplate: return 5;
    case I_iron_chestplate: return 6;
    case I_diamond_chestplate: // Same as netherite
    case I_netherite_chestplate: return 8;
    case I_leather_leggings: return 2;
    case I_golden_leggings: return 3;
    case I_iron_leggings: return 5;
    case I_diamond_leggings: // Same as netherite
    case I_netherite_leggings: return 6;
    case I_leather_boots: return 1;
    case I_golden_boots: return 1;
    case I_iron_boots: return 2;
    case I_diamond_boots: // Same as netherite
    case I_netherite_boots: return 3;
    default: break;
  }

  return 0;
}

// Calculates total defense points for the player's equipped armor
uint8_t getPlayerDefensePoints (PlayerData *player) {
  return (
    // Helmet
    getItemDefensePoints(player->inventory_items[39]) +
    // Chestplate
    getItemDefensePoints(player->inventory_items[38]) +
    // Leggings
    getItemDefensePoints(player->inventory_items[37]) +
    // Boots
    getItemDefensePoints(player->inventory_items[36])
  );
}

// Returns the designated server slot for the given piece of armor
// If input item is not armor, returns 255
uint8_t getArmorItemSlot (uint16_t item) {

    switch (item) {
    case I_leather_helmet:
    case I_golden_helmet:
    case I_iron_helmet:
    case I_diamond_helmet:
    case I_netherite_helmet:
      return 39;
    case I_leather_chestplate:
    case I_golden_chestplate:
    case I_iron_chestplate:
    case I_diamond_chestplate:
    case I_netherite_chestplate:
      return 38;
    case I_leather_leggings:
    case I_golden_leggings:
    case I_iron_leggings:
    case I_diamond_leggings:
    case I_netherite_leggings:
      return 37;
    case I_leather_boots:
    case I_golden_boots:
    case I_iron_boots:
    case I_diamond_boots:
    case I_netherite_boots:
      return 36;
    default: break;
  }

  return 255;
}

// Handles the player eating their currently held item
// Returns whether the operation was succesful (item was consumed)
// If `just_check` is set to true, the item doesn't get consumed
uint8_t handlePlayerEating (PlayerData *player, uint8_t just_check) {

  // Exit early if player is unable to eat
  if (player->hunger >= 20) return false;

  uint16_t *held_item = &player->inventory_items[player->hotbar];
  uint8_t *held_count = &player->inventory_count[player->hotbar];

  // Exit early if player isn't holding anything
  if (*held_item == 0 || *held_count == 0) return false;

  uint8_t food = 0;
  uint16_t saturation = 0;

  // The saturation ratio from vanilla to here is about 1:500
  switch (*held_item) {
    case I_chicken: food = 2; saturation = 600; break;
    case I_beef: food = 3; saturation = 900; break;
    case I_porkchop: food = 3; saturation = 300; break;
    case I_mutton: food = 2; saturation = 600; break;
    case I_cooked_chicken: food = 6; saturation = 3600; break;
    case I_cooked_beef: food = 8; saturation = 6400; break;
    case I_cooked_porkchop: food = 8; saturation = 6400; break;
    case I_cooked_mutton: food = 6; saturation = 4800; break;
    case I_rotten_flesh: food = 4; saturation = 0; break;
    case I_apple: food = 4; saturation = 1200; break;
    default: break;
  }

  // If just checking the item, return before making any changes
  if (just_check) return food != 0;

  // Apply saturation and food boost
  player->saturation += saturation;
  player->hunger += food;
  if (player->hunger > 20) player->hunger = 20;

  // Consume held item
  *held_count -= 1;
  if (*held_count == 0) *held_item = 0;

  // Update the client of these changes
  sc_entityEvent(player->client_fd, player->client_fd, 9);
  sc_setHealth(player->client_fd, player->health, player->hunger, player->saturation);
  sc_setContainerSlot(
    player->client_fd, 0,
    serverSlotToClientSlot(0, player->hotbar),
    *held_count, *held_item
  );

  return true;
}

void handleFluidMovement (short x, uint8_t y, short z, uint8_t fluid, uint8_t block) {

  // Get fluid level (0-7)
  // The terminology here is a bit different from vanilla:
  // A higher fluid "level" means the fluid has traveled farther
  uint8_t level = block - fluid;

  // Query blocks adjacent to this fluid stream
  uint8_t adjacent[4] = {
    getBlockAt(x + 1, y, z),
    getBlockAt(x - 1, y, z),
    getBlockAt(x, y, z + 1),
    getBlockAt(x, y, z - 1)
  };

  // Handle maintaining connections to a fluid source
  if (level != 0) {
    // Check if this fluid is connected to a block exactly one level lower
    uint8_t connected = false;
    for (int i = 0; i < 4; i ++) {
      if (adjacent[i] == block - 1) {
        connected = true;
        break;
      }
    }
    // If not connected, clear this block and recalculate surrounding flow
    if (!connected) {
      makeBlockChange(x, y, z, B_air);
      checkFluidUpdate(x + 1, y, z, adjacent[0]);
      checkFluidUpdate(x - 1, y, z, adjacent[1]);
      checkFluidUpdate(x, y, z + 1, adjacent[2]);
      checkFluidUpdate(x, y, z - 1, adjacent[3]);
      return;
    }
  }

  // Check if water should flow down, prioritize that over lateral flow
  uint8_t block_below = getBlockAt(x, y - 1, z);
  if (isReplaceableBlock(block_below)) {
    makeBlockChange(x, y - 1, z, fluid);
    return handleFluidMovement(x, y - 1, z, fluid, fluid);
  }

  // Stop flowing laterally at the maximum level
  if (level == 3 && fluid == B_lava) return;
  if (level == 7) return;

  // Handle lateral water flow, increasing level by 1
  if (isReplaceableFluid(adjacent[0], level, fluid)) {
    makeBlockChange(x + 1, y, z, block + 1);
    handleFluidMovement(x + 1, y, z, fluid, block + 1);
  }
  if (isReplaceableFluid(adjacent[1], level, fluid)) {
    makeBlockChange(x - 1, y, z, block + 1);
    handleFluidMovement(x - 1, y, z, fluid, block + 1);
  }
  if (isReplaceableFluid(adjacent[2], level, fluid)) {
    makeBlockChange(x, y, z + 1, block + 1);
    handleFluidMovement(x, y, z + 1, fluid, block + 1);
  }
  if (isReplaceableFluid(adjacent[3], level, fluid)) {
    makeBlockChange(x, y, z - 1, block + 1);
    handleFluidMovement(x, y, z - 1, fluid, block + 1);
  }

}

void checkFluidUpdate (short x, uint8_t y, short z, uint8_t block) {

  uint8_t fluid;
  if (block >= B_water && block < B_water + 8) fluid = B_water;
  else if (block >= B_lava && block < B_lava + 4) fluid = B_lava;
  else return;

  handleFluidMovement(x, y, z, fluid, block);

}

#ifdef ENABLE_PICKUP_ANIMATION
// Plays the item pickup animation with the given item at the given coordinates
void playPickupAnimation (PlayerData *player, uint16_t item, double x, double y, double z) {

  // Spawn a new item entity at the input coordinates
  // ID -1 is safe, as elsewhere it's reserved as a placeholder
  // The player's name is used as the UUID as it's cheap and unique enough
  sc_spawnEntity(player->client_fd, -1, (uint8_t *)player->name, 69, x + 0.5, y + 0.5, z + 0.5, 0, 0);

  // Write a Set Entity Metadata packet for the item
  // There's no packets.c entry for this, as it's not cheaply generalizable
  writeVarInt(player->client_fd, 12 + sizeVarInt(item));
  writeByte(player->client_fd, 0x5C);
  writeVarInt(player->client_fd, -1);

  // Describe slot data array entry
  writeByte(player->client_fd, 8);
  writeByte(player->client_fd, 7);
  // Send slot data
  writeByte(player->client_fd, 1);
  writeVarInt(player->client_fd, item);
  writeByte(player->client_fd, 0);
  writeByte(player->client_fd, 0);
  // Terminate entity metadata array
  writeByte(player->client_fd, 0xFF);

  // Send the Pickup Item packet targeting this entity
  sc_pickupItem(player->client_fd, -1, player->client_fd, 1);

  // Remove the item entity from the client right away
  sc_removeEntity(player->client_fd, -1);

}
#endif

void handlePlayerAction (PlayerData *player, int action, short x, short y, short z) {

  // Re-sync slot when player drops an item
  if (action == 3 || action == 4) {
    sc_setContainerSlot(
      player->client_fd, 0,
      serverSlotToClientSlot(0, player->hotbar),
      player->inventory_count[player->hotbar],
      player->inventory_items[player->hotbar]
    );
    return;
  }

  // "Finish eating" action, called any time eating stops
  if (action == 5) {
    // Reset eating timer and clear eating flag
    player->flagval_16 = 0;
    player->flags &= ~0x10;
  }

  // Ignore further actions not pertaining to mining blocks
  if (action != 0 && action != 2) return;

  // In creative, only the "start mining" action is sent
  // No additional verification is performed, the block is simply removed
  if (action == 0 && GAMEMODE == 1) {
    makeBlockChange(x, y, z, 0);
    return;
  }

  uint8_t block = getBlockAt(x, y, z);

  // If this is a "start mining" packet, the block must be instamine
  if (action == 0 && !isInstantlyMined(player, block)) return;

  // Don't continue if the block change failed
  if (makeBlockChange(x, y, z, 0)) return;

  uint16_t held_item = player->inventory_items[player->hotbar];
  uint16_t item = getMiningResult(held_item, block);
  bumpToolDurability(player);

  if (item) {
    #ifdef ENABLE_PICKUP_ANIMATION
    playPickupAnimation(player, item, x, y, z);
    #endif
    givePlayerItem(player, item, 1);
  }

  // Update nearby fluids
  uint8_t block_above = getBlockAt(x, y + 1, z);
  #ifdef DO_FLUID_FLOW
    checkFluidUpdate(x, y + 1, z, block_above);
    checkFluidUpdate(x - 1, y, z, getBlockAt(x - 1, y, z));
    checkFluidUpdate(x + 1, y, z, getBlockAt(x + 1, y, z));
    checkFluidUpdate(x, y, z - 1, getBlockAt(x, y, z - 1));
    checkFluidUpdate(x, y, z + 1, getBlockAt(x, y, z + 1));
  #endif

  // Check if any blocks above this should break, and if so,
  // Iterate upward over all blocks in the column and break them
  uint8_t y_offset = 1;
  while (isColumnBlock(block_above)) {
    // Destroy the next block
    makeBlockChange(x, y + y_offset, z, 0);
    // Check for item drops *without a tool*
    uint16_t item = getMiningResult(0, block_above);
    if (item) givePlayerItem(player, item, 1);
    // Select the next block in the column
    y_offset ++;
    block_above = getBlockAt(x, y + y_offset, z);
  }
}

void handlePlayerUseItem (PlayerData *player, short x, short y, short z, uint8_t face) {

  // Get targeted block (if coordinates are provided)
  uint8_t target = face == 255 ? 0 : getBlockAt(x, y, z);
  // Get held item properties
  uint8_t *count = &player->inventory_count[player->hotbar];
  uint16_t *item = &player->inventory_items[player->hotbar];

  // Check interaction with containers when not sneaking
  if (!(player->flags & 0x04) && face != 255) {
    if (target == B_crafting_table) {
      sc_openScreen(player->client_fd, 12, "Crafting", 8);
      return;
    } else if (target == B_furnace) {
      sc_openScreen(player->client_fd, 14, "Furnace", 7);
      return;
    } else if (target == B_composter) {
      // Check if the player is holding anything
      if (*count == 0) return;
      // Check if the item is a valid compost item
      uint32_t compost_chance = isCompostItem(*item);
      if (compost_chance != 0) {
        // Take away composted item
        if ((*count -= 1) == 0) *item = 0;
        sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, player->hotbar), *count, *item);
        // Test compost chance and give bone meal on success
        if (fast_rand() < compost_chance) {
          givePlayerItem(player, I_bone_meal, 1);
        }
        return;
      }
    }
    #ifdef ALLOW_CHESTS
    else if (target == B_chest) {
      // Get a pointer to the entry following this chest in block_changes
      uint8_t *storage_ptr = NULL;
      int chest_index = findBlockChangeIndex(x, y, z);
      if (chest_index != -1 && block_changes[chest_index].block == B_chest) {
        storage_ptr = (uint8_t *)(&block_changes[chest_index + 1]);
      }
      if (storage_ptr == NULL) return;
      // Terrible memory hack!!
      // Copy the pointer into the player's crafting table item array.
      // This allows us to save some memory by repurposing a feature that
      // Is mutually exclusive with chests, though it is otherwise a
      // Terrible idea for obvious reasons.
      memcpy(player->craft_items, &storage_ptr, sizeof(storage_ptr));
      // Flag craft_items as locked due to holding a pointer
      player->flags |= 0x80;
      // Show the player the chest UI
      sc_openScreen(player->client_fd, 2, "Chest", 5);
      // Load the slots of the chest from the block_changes array.
      // This is a similarly dubious memcpy hack, but at least we're not
      // Mixing data types? Kind of?
      for (int i = 0; i < 27; i ++) {
        uint16_t item;
        uint8_t count;
        memcpy(&item, storage_ptr + i * 3, 2);
        memcpy(&count, storage_ptr + i * 3 + 2, 1);
        sc_setContainerSlot(player->client_fd, 2, i, count, item);
      }
      return;
    }
    #endif
  }

  // If the selected slot doesn't hold any items, exit
  if (*count == 0) return;

  // Check special item handling
  if (*item == I_bone_meal) {
    uint8_t target_below = getBlockAt(x, y - 1, z);
    if (target == B_oak_sapling) {
      // Consume the bone meal (yes, even before checks)
      // Wasting bone meal on misplanted saplings is vanilla behavior
      if ((*count -= 1) == 0) *item = 0;
      sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, player->hotbar), *count, *item);
      if ( // Saplings can only grow when placed on these blocks
        target_below == B_dirt ||
        target_below == B_grass_block ||
        target_below == B_snowy_grass_block ||
        target_below == B_mud
      ) {
        // Bone meal has a 25% chance of growing a tree from a sapling
        if ((fast_rand() & 3) == 0) placeTreeStructure(x, y, z);
      }
    }
  } else if (handlePlayerEating(player, true)) {
    // Reset eating timer and set eating flag
    player->flagval_16 = 0;
    player->flags |= 0x10;
  } else if (getItemDefensePoints(*item) != 0) {
    // For some reason, this action is sent twice when looking at a block
    // Ignore the variant that has coordinates
    if (face != 255) return;
    // Swap to held piece of armor
    uint8_t slot = getArmorItemSlot(*item);
    uint16_t prev_item = player->inventory_items[slot];
    player->inventory_items[slot] = *item;
    player->inventory_count[slot] = 1;
    player->inventory_items[player->hotbar] = prev_item;
    player->inventory_count[player->hotbar] = 1;
    // Update client inventory
    sc_setContainerSlot(player->client_fd, -2, serverSlotToClientSlot(0, slot), 1, *item);
    sc_setContainerSlot(player->client_fd, -2, serverSlotToClientSlot(0, player->hotbar), 1, prev_item);
    return;
  }

  // Don't proceed with block placement if no coordinates were provided
  if (face == 255) return;

  // If the selected item doesn't correspond to a block, exit
  uint8_t block = I_to_B(*item);
  if (block == 0) return;

  switch (face) {
    case 0: y -= 1; break;
    case 1: y += 1; break;
    case 2: z -= 1; break;
    case 3: z += 1; break;
    case 4: x -= 1; break;
    case 5: x += 1; break;
    default: break;
  }

  // Check if the block's placement conditions are met
  if (
    !( // Is player in the way?
      !isPassableBlock(block) &&
      x == player->x &&
      (y == player->y || y == player->y + 1) &&
      z == player->z
    ) &&
    isReplaceableBlock(getBlockAt(x, y, z)) &&
    (!isColumnBlock(block) || getBlockAt(x, y - 1, z) != B_air)
  ) {
    // Apply server-side block change
    if (makeBlockChange(x, y, z, block)) return;
    // Decrease item amount in selected slot
    *count -= 1;
    // Clear item id in slot if amount is zero
    if (*count == 0) *item = 0;
    // Calculate fluid flow
    #ifdef DO_FLUID_FLOW
      checkFluidUpdate(x, y + 1, z, getBlockAt(x, y + 1, z));
      checkFluidUpdate(x - 1, y, z, getBlockAt(x - 1, y, z));
      checkFluidUpdate(x + 1, y, z, getBlockAt(x + 1, y, z));
      checkFluidUpdate(x, y, z - 1, getBlockAt(x, y, z - 1));
      checkFluidUpdate(x, y, z + 1, getBlockAt(x, y, z + 1));
    #endif
  }

  // Sync hotbar contents to player
  sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, player->hotbar), *count, *item);

}

void spawnMob (uint8_t type, short x, uint8_t y, short z, uint8_t health) {

  for (int i = 0; i < MAX_MOBS; i ++) {
    // Look for type 0 (unallocated)
    if (mob_data[i].type != 0) continue;

    // Assign it the input parameters
    mob_data[i].type = type;
    mob_data[i].x = x;
    mob_data[i].y = y;
    mob_data[i].z = z;
    mob_data[i].data = health & 31;
    villager_job[i] = 0;
    villager_level[i] = 0;
    villager_xp[i] = 0;

    if (type == ENTITY_TYPE_VILLAGER) {
      villager_job[i] = fast_rand() % 3;
    }

    // Forge a UUID from a random number and the mob's index
    uint8_t uuid[16];
    uint32_t r = fast_rand();
    memcpy(uuid, &r, 4);
    memcpy(uuid + 4, &i, 4);

    // Broadcast entity creation to all players
    for (int j = 0; j < MAX_PLAYERS; j ++) {
      if (player_data[j].client_fd == -1) continue;
      if (player_data[j].flags & 0x20) continue;
      sc_spawnEntity(
        player_data[j].client_fd,
        -2 - i, // Use negative IDs to avoid conflicts with player IDs
        uuid, // Use the UUID generated above
        type, (double)x + 0.5f, y, (double)z + 0.5f,
        // Face opposite of the player, as if looking at them when spawning
        (player_data[j].yaw + 127) & 255, 0
      );
    }

    // Freshly spawned mobs currently don't need metadata updates.
    // If this changes, uncomment this line.
    // BroadcastMobMetadata(-1, i);

    break;
  }

}

int getMobCountByType (uint8_t type) {
  int count = 0;
  for (int i = 0; i < MAX_MOBS; i ++) {
    if (mob_data[i].type == type && (mob_data[i].data & 31) > 0) count ++;
  }
  return count;
}

void movePlayerToNetherZone (PlayerData *player, uint8_t to_nether) {
  if (player == NULL) return;

  uint8_t currently_nether = isInNetherZone(player->z);
  if (to_nether == currently_nether) return;

  if (to_nether) {
    player->x /= 8;
    player->z = player->z / 8 + NETHER_ZONE_OFFSET;
  } else {
    player->x *= 8;
    player->z = (player->z - NETHER_ZONE_OFFSET) * 8;
  }

  player->y = getHeightAt(player->x, player->z) + 1;
  player->grounded_y = player->y;

  for (int i = 0; i < VISITED_HISTORY; i ++) {
    player->visited_x[i] = 32767;
    player->visited_z[i] = 32767;
  }

  if (to_nether) sc_systemChat(player->client_fd, "Entered the nether zone", 23);
  else sc_systemChat(player->client_fd, "Returned to overworld", 21);

  spawnPlayer(player);
}

void interactEntity (int entity_id, int interactor_id) {

  PlayerData *player;
  if (getPlayerData(interactor_id, &player)) return;

  int mob_index = -entity_id - 2;
  if (mob_index < 0 || mob_index >= MAX_MOBS) return;
  MobData *mob = &mob_data[mob_index];

  switch (mob->type) {
    case ENTITY_TYPE_SHEEP: // Sheep
      if (player->inventory_items[player->hotbar] != I_shears)
        return;

      if ((mob->data >> 5) & 1) // Check if sheep has already been sheared
        return;

      mob->data |= 1 << 5; // Set sheared to true

      bumpToolDurability(player);

      #ifdef ENABLE_PICKUP_ANIMATION
      playPickupAnimation(player, I_white_wool, mob->x, mob->y, mob->z);
      #endif

      uint8_t item_count = 1 + (fast_rand() & 1); // 1-2
      givePlayerItem(player, I_white_wool, item_count);

      for (int i = 0; i < MAX_PLAYERS; i ++) {
        PlayerData* player = &player_data[i];
        int client_fd = player->client_fd;

        if (client_fd == -1) continue;
        if (player->flags & 0x20) continue;

        sc_entityAnimation(client_fd, interactor_id, 0);
      }

      broadcastMobMetadata(-1, entity_id);

      break;

    case ENTITY_TYPE_VILLAGER: {
      uint8_t job = villager_job[mob_index] % 3;
      uint8_t level = villager_level[mob_index];
      uint8_t held_slot = player->hotbar;
      uint16_t *held_item = &player->inventory_items[held_slot];
      uint8_t *held_count = &player->inventory_count[held_slot];

      uint16_t cost_item = 0, out_item = 0;
      uint8_t cost_count = 0, out_count = 0;

      if (job == VJ_FARMER) {
        if (*held_item == I_wheat && *held_count >= 18) {
          cost_item = I_wheat; cost_count = 18;
          out_item = I_emerald; out_count = 1;
        } else if (*held_item == I_emerald && *held_count >= 1) {
          cost_item = I_emerald; cost_count = 1;
          out_item = I_bread; out_count = 3;
        }
      } else if (job == VJ_LIBRARIAN) {
        if (*held_item == I_paper && *held_count >= 24) {
          cost_item = I_paper; cost_count = 24;
          out_item = I_emerald; out_count = 1;
        } else if (level >= 1 && *held_item == I_emerald && *held_count >= 5) {
          cost_item = I_emerald; cost_count = 5;
          out_item = I_bookshelf; out_count = 1;
        }
      } else if (job == VJ_TOOLSMITH) {
        if (*held_item == I_iron_ingot && *held_count >= 1) {
          cost_item = I_iron_ingot; cost_count = 1;
          out_item = I_emerald; out_count = 1;
        } else if (level >= 1 && *held_item == I_emerald && *held_count >= 6) {
          cost_item = I_emerald; cost_count = 6;
          out_item = I_iron_pickaxe; out_count = 1;
        }
      }

      if (cost_item == 0) {
        char msg[96];
        snprintf(msg, sizeof(msg), "§e%s§7 (lvl %u): hold a trade item and right-click", villagerJobName(job), level + 1);
        sc_systemChat(player->client_fd, msg, strlen(msg));
        return;
      }

      if (givePlayerItem(player, out_item, out_count)) {
        sc_systemChat(player->client_fd, "Inventory full", 14);
        return;
      }

      *held_count -= cost_count;
      if (*held_count == 0) *held_item = 0;
      sc_setContainerSlot(
        player->client_fd,
        0,
        serverSlotToClientSlot(0, held_slot),
        *held_count,
        *held_item
      );

      if (villager_xp[mob_index] < 255) villager_xp[mob_index] ++;
      if (villager_level[mob_index] == 0 && villager_xp[mob_index] >= 4) villager_level[mob_index] = 1;
      else if (villager_level[mob_index] == 1 && villager_xp[mob_index] >= 10) villager_level[mob_index] = 2;

      char msg[80];
      snprintf(msg, sizeof(msg), "Traded with %s (lvl %u)", villagerJobName(job), villager_level[mob_index] + 1);
      sc_systemChat(player->client_fd, msg, strlen(msg));
      break;
    }
  }
}

void hurtEntity (int entity_id, int attacker_id, uint8_t damage_type, uint8_t damage) {

  if (attacker_id > 0) { // Attacker is a player

    PlayerData *player;
    if (getPlayerData(attacker_id, &player)) return;

    // Check if attack cooldown flag is set
    if (player->flags & 0x01) return;

    // Scale damage based on held item
    uint16_t held_item = player->inventory_items[player->hotbar];
    if (held_item == I_wooden_sword) damage *= 4;
    else if (held_item == I_golden_sword) damage *= 4;
    else if (held_item == I_stone_sword) damage *= 5;
    else if (held_item == I_iron_sword) damage *= 6;
    else if (held_item == I_diamond_sword) damage *= 7;
    else if (held_item == I_netherite_sword) damage *= 8;

    // Enable attack cooldown
    player->flags |= 0x01;
    player->flagval_8 = 0;

  }

  // Whether this attack caused the target entity to die
  uint8_t entity_died = false;
  // Whether to emit a mob hurt entity_event for this hit.
  uint8_t mob_hurt_event = false;
  int mob_sound_hurt = -1;
  int mob_sound_death = -1;
  int mob_sound_source = 6; // neutral

  if (entity_id > 0) { // The attacked entity is a player

    PlayerData *player;
    if (getPlayerData(entity_id, &player)) return;

    // Don't continue if the player is already dead
    if (player->health == 0) return;

    // Calculate damage reduction from player's armor
    uint8_t defense = getPlayerDefensePoints(player);
    // This uses the old (pre-1.9) protection calculation. Factors are
    // Scaled up 256 times to avoid floating point math. Due to lost
    // Precision, the 4% reduction factor drops to ~3.9%, although the
    // The resulting effective damage is then also rounded down.
    uint8_t effective_damage = damage * (256 - defense * 10) / 256;

    // Process health change on the server
    if (player->health <= effective_damage) {

      player->health = 0;
      entity_died = true;

      // Prepare death message in recv_buffer
      uint8_t player_name_len = strlen(player->name);
      strcpy((char *)recv_buffer, player->name);

      if (damage_type == D_fall && damage > 8) {
        // Killed by a greater than 5 block fall
        strcpy((char *)recv_buffer + player_name_len, " fell from a high place");
        recv_buffer[player_name_len + 23] = '\0';
      } else if (damage_type == D_fall) {
        // Killed by a less than 5 block fall
        strcpy((char *)recv_buffer + player_name_len, " hit the ground too hard");
        recv_buffer[player_name_len + 24] = '\0';
      } else if (damage_type == D_lava) {
        // Killed by being in lava
        strcpy((char *)recv_buffer + player_name_len, " tried to swim in lava");
        recv_buffer[player_name_len + 22] = '\0';
      } else if (attacker_id < -1) {
        // Killed by a mob
        strcpy((char *)recv_buffer + player_name_len, " was slain by a mob");
        recv_buffer[player_name_len + 19] = '\0';
      } else if (attacker_id > 0) {
        // Killed by a player
        PlayerData *attacker;
        if (getPlayerData(attacker_id, &attacker)) return;
        strcpy((char *)recv_buffer + player_name_len, " was slain by ");
        strcpy((char *)recv_buffer + player_name_len + 14, attacker->name);
        recv_buffer[player_name_len + 14 + strlen(attacker->name)] = '\0';
      } else if (damage_type == D_cactus) {
        // Killed by being near a cactus
        strcpy((char *)recv_buffer + player_name_len, " was pricked to death");
        recv_buffer[player_name_len + 21] = '\0';
      } else {
        // Unknown death reason
        strcpy((char *)recv_buffer + player_name_len, " died");
        recv_buffer[player_name_len + 5] = '\0';
      }

    } else player->health -= effective_damage;

    // Update health on the client
    sc_setHealth(entity_id, player->health, player->hunger, player->saturation);

  } else { // The attacked entity is a mob

    int mob_index = -entity_id - 2;
    if (mob_index < 0 || mob_index >= MAX_MOBS) return;
    MobData *mob = &mob_data[mob_index];

    uint8_t mob_health = mob->data & 31;

    // Don't continue if the mob is already dead
    if (mob_health == 0) return;

    // Set the mob's panic timer
    mob->data |= (3 << 6);
    // Emit hurt event for every valid mob hit; this drives hurt animation/sound.
    mob_hurt_event = true;
    switch (mob->type) {
      case ENTITY_TYPE_CHICKEN:
        mob_sound_hurt = 333;
        mob_sound_death = 331;
        mob_sound_source = 6;
        break;
      case ENTITY_TYPE_COW:
        mob_sound_hurt = 424;
        mob_sound_death = 423;
        mob_sound_source = 6;
        break;
      case ENTITY_TYPE_PIG:
        mob_sound_hurt = 1216;
        mob_sound_death = 1215;
        mob_sound_source = 6;
        break;
      case ENTITY_TYPE_SHEEP:
        mob_sound_hurt = 1379;
        mob_sound_death = 1378;
        mob_sound_source = 6;
        break;
      case ENTITY_TYPE_ZOMBIE:
        mob_sound_hurt = 1807;
        mob_sound_death = 1800;
        mob_sound_source = 5; // hostile
        break;
      default:
        break;
    }

    // Process health change on the server
    if (mob_health <= damage) {

      mob->data -= mob_health;
      mob->y = 0;
      entity_died = true;

      // Handle mob drops
      if (attacker_id > 0) {
        PlayerData *player;
        if (getPlayerData(attacker_id, &player)) return;
        switch (mob->type) {
          case ENTITY_TYPE_CHICKEN:
            givePlayerItem(player, I_chicken, 1);
            if ((fast_rand() & 1) == 0) givePlayerItem(player, I_feather, 1 + (fast_rand() & 1));
            break;
          case ENTITY_TYPE_COW:
            givePlayerItem(player, I_beef, 1 + (fast_rand() % 3));
            if ((fast_rand() & 1) == 0) givePlayerItem(player, I_leather, 1 + (fast_rand() & 1));
            break;
          case ENTITY_TYPE_PIG:
            givePlayerItem(player, I_porkchop, 1 + (fast_rand() % 3));
            break;
          case ENTITY_TYPE_SHEEP:
            givePlayerItem(player, I_mutton, 1 + (fast_rand() & 1));
            // Unsheared sheep drop one wool on death.
            if (((mob->data >> 5) & 1) == 0) givePlayerItem(player, I_white_wool, 1);
            break;
          case ENTITY_TYPE_ZOMBIE:
            if ((fast_rand() & 1) == 0) givePlayerItem(player, I_rotten_flesh, 1 + (fast_rand() & 1));
            break;
          default: break;
        }
      }

    } else mob->data -= damage;

  }

  // Broadcast damage event to all players
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    int client_fd = player_data[i].client_fd;
    if (client_fd == -1) continue;
    if (mob_hurt_event) {
      sc_entityEvent(client_fd, entity_id, 2);
      if (!entity_died && mob_sound_hurt != -1) {
        sc_soundEntity(client_fd, mob_sound_hurt, mob_sound_source, entity_id, 1.0f, 1.0f, fast_rand());
      }
    }
    sc_damageEvent(client_fd, entity_id, damage_type);
    // Below this, handle death events
    if (!entity_died) continue;
    sc_entityEvent(client_fd, entity_id, 3);
    if (entity_id < 0 && mob_sound_death != -1) {
      sc_soundEntity(client_fd, mob_sound_death, mob_sound_source, entity_id, 1.0f, 1.0f, fast_rand());
    }
    if (entity_id >= 0) {
      // If a player died, broadcast their death message
      sc_systemChat(client_fd, (char *)recv_buffer, strlen((char *)recv_buffer));
    }
  }

}

// Simulates events scheduled for regular intervals
// Takes the time since the last tick in microseconds as the only arguemnt
void handleServerTick (int64_t time_since_last_tick) {
  static uint8_t attack_cooldown_ticks = 0;
  static uint16_t eating_ticks = 0;
  static uint8_t tick_thresholds_ready = false;
  if (!tick_thresholds_ready) {
    attack_cooldown_ticks = (uint8_t)(0.6f * TICKS_PER_SECOND);
    eating_ticks = (uint16_t)(1.6f * TICKS_PER_SECOND);
    tick_thresholds_ready = true;
  }

  // Update world time
  world_time = (world_time + time_since_last_tick / 50000) % 24000;
  // Increment server tick counter
  server_ticks ++;
  uint8_t is_second_tick = (server_ticks % (uint32_t)TICKS_PER_SECOND) == 0;

  // Update player events
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    PlayerData *player = &player_data[i];
    if (player->client_fd == -1) continue; // Skip offline players
    if (player->flags & 0x20) { // Check "client loading" flag
      // If 3 seconds (60 vanilla ticks) have passed, assume player has loaded
      player->flagval_16 ++;
      if (player->flagval_16 > (uint16_t)(3 * TICKS_PER_SECOND)) {
        handlePlayerJoin(player);
      } else continue;
    }
    // Reset player attack cooldown
    if (player->flags & 0x01) {
      if (player->flagval_8 >= attack_cooldown_ticks) {
        player->flags &= ~0x01;
        player->flagval_8 = 0;
      } else player->flagval_8 ++;
    }
    // Handle eating animation
    if (player->flags & 0x10) {
      if (player->flagval_16 >= eating_ticks) {
        handlePlayerEating(&player_data[i], false);
        player->flags &= ~0x10;
        player->flagval_16 = 0;
      } else player->flagval_16 ++;
    }
    // Reset movement update cooldown if not broadcasting every update
    // Effectively ties player movement updates to the tickrate
    #ifndef BROADCAST_ALL_MOVEMENT
      player->flags &= ~0x40;
    #endif
    // Below this, process events that happen once per second
    if (!is_second_tick) continue;
    // Send Keep Alive and Update Time packets
    sc_keepAlive(player->client_fd);
    sc_updateTime(player->client_fd, world_time);
    // Tick damage from lava
    uint8_t block = getBlockAt(player->x, player->y, player->z);
    if (block >= B_lava && block < B_lava + 4) {
      hurtEntity(player->client_fd, -1, D_lava, 8);
    }
    #ifdef ENABLE_CACTUS_DAMAGE
    // Tick damage from a cactus block if one is under/inside or around the player.
    if (block == B_cactus ||
      getBlockAt(player->x + 1, player->y, player->z) == B_cactus ||
      getBlockAt(player->x - 1, player->y, player->z) == B_cactus ||
      getBlockAt(player->x, player->y, player->z + 1) == B_cactus ||
      getBlockAt(player->x, player->y, player->z - 1) == B_cactus
    ) hurtEntity(player->client_fd, -1, D_cactus, 4);
    #endif
    // Heal from saturation if player is able and has enough food
    if (player->health >= 20 || player->health == 0) continue;
    if (player->hunger < 18) continue;
    if (player->saturation >= 600) {
      player->saturation -= 600;
      player->health ++;
    } else {
      player->hunger --;
      player->health ++;
    }
    sc_setHealth(player->client_fd, player->health, player->hunger, player->saturation);
  }

  // Perform regular checks for if it's time to write to disk
  writeDataToDiskOnInterval();

  /**
   * If the RNG seed ever hits 0, it'll never generate anything
   * else. This is because the fast_rand function uses a simple
   * XORshift. This isn't a common concern, so we only check for
   * this periodically. If it does become zero, we reset it to
   * the world seed as a good-enough fallback.
   */
  if (rng_seed == 0) rng_seed = world_seed;

  // Tick mob behavior
  for (int i = 0; i < MAX_MOBS; i ++) {
    if (mob_data[i].type == 0) continue;
    int entity_id = -2 - i;

    // Handle deallocation on mob death
    if ((mob_data[i].data & 31) == 0) {
      if (mob_data[i].y < (unsigned int)TICKS_PER_SECOND) {
        mob_data[i].y ++;
        continue;
      }
      mob_data[i].type = 0;
      villager_job[i] = 0;
      villager_level[i] = 0;
      villager_xp[i] = 0;
      for (int j = 0; j < MAX_PLAYERS; j ++) {
        if (player_data[j].client_fd == -1) continue;
        // Spawn death smoke particles
        sc_entityEvent(player_data[j].client_fd, entity_id, 60);
        // Remove the entity from the client
        sc_removeEntity(player_data[j].client_fd, entity_id);
      }
      continue;
    }

    uint8_t passive = (
      mob_data[i].type == ENTITY_TYPE_CHICKEN || // Chicken
      mob_data[i].type == ENTITY_TYPE_COW || // Cow
      mob_data[i].type == ENTITY_TYPE_PIG || // Pig
      mob_data[i].type == ENTITY_TYPE_SHEEP || // Sheep
      mob_data[i].type == ENTITY_TYPE_VILLAGER // Villager
    );
    // Mob "panic" timer, set to 3 after being hit
    // Currently has no effect on hostile mobs
    uint8_t panic = (mob_data[i].data >> 6) & 3;

    // Burn hostile mobs if above ground during sunlight
    if (!passive && (world_time < 13000 || world_time > 23460) && mob_data[i].y > 48) {
      hurtEntity(entity_id, -1, D_on_fire, 2);
    }

    uint32_t r = fast_rand();

    if (passive) {
      if (panic) {
        // If panicking, move randomly at up to 4 times per second
        if (TICKS_PER_SECOND >= 4) {
          uint32_t ticks_per_panic = (uint32_t)(TICKS_PER_SECOND / 4);
          if (server_ticks % ticks_per_panic != 0) continue;
        }
        // Reset panic state after timer runs out
        // Each panic timer tick takes one second
        if (server_ticks % (uint32_t)TICKS_PER_SECOND == 0) {
          mob_data[i].data -= (1 << 6);
        }
      } else {
        // When not panicking, move idly once per 4 seconds on average
        if (r % (4 * (unsigned int)TICKS_PER_SECOND) != 0) continue;
      }
    } else {
      // Update hostile mobs once per second
      if (server_ticks % (uint32_t)TICKS_PER_SECOND != 0) continue;
    }

    // Find the player closest to this mob
    PlayerData* closest_player = &player_data[0];
    uint32_t closest_dist = 2147483647;
    for (int j = 0; j < MAX_PLAYERS; j ++) {
      if (player_data[j].client_fd == -1) continue;
      uint16_t curr_dist = (
        abs(mob_data[i].x - player_data[j].x) +
        abs(mob_data[i].z - player_data[j].z)
      );
      if (curr_dist < closest_dist) {
        closest_dist = curr_dist;
        closest_player = &player_data[j];
      }
    }

    // Despawn mobs past a certain distance from nearest player
    if (closest_dist > MOB_DESPAWN_DISTANCE) {
      mob_data[i].type = 0;
      villager_job[i] = 0;
      villager_level[i] = 0;
      villager_xp[i] = 0;
      continue;
    }

    short old_x = mob_data[i].x, old_z = mob_data[i].z;
    uint8_t old_y = mob_data[i].y;

    short new_x = old_x, new_z = old_z;
    uint8_t new_y = old_y, yaw = 0;

    if (passive) { // Passive mob movement handling

      // Move by one block on the X or Z axis
      // Yaw is set to face in the direction of motion
      if ((r >> 2) & 1) {
        if ((r >> 1) & 1) { new_x += 1; yaw = 192; }
        else { new_x -= 1; yaw = 64; }
      } else {
        if ((r >> 1) & 1) { new_z += 1; yaw = 0; }
        else { new_z -= 1; yaw = 128; }
      }

    } else { // Hostile mob movement handling

      // If we're already next to the player, hurt them and skip movement
      if (closest_dist < 3 && abs(old_y - closest_player->y) < 2) {
        hurtEntity(closest_player->client_fd, entity_id, D_generic, 6);
        continue;
      }

      // Move towards the closest player on 8 axis
      // The condition nesting ensures a correct yaw at 45 degree turns
      if (closest_player->x < old_x) {
        new_x -= 1; yaw = 64;
        if (closest_player->z < old_z) { new_z -= 1; yaw += 32; }
        else if (closest_player->z > old_z) { new_z += 1; yaw -= 32; }
      }
      else if (closest_player->x > old_x) {
        new_x += 1; yaw = 192;
        if (closest_player->z < old_z) { new_z -= 1; yaw -= 32; }
        else if (closest_player->z > old_z) { new_z += 1; yaw += 32; }
      } else {
        if (closest_player->z < old_z) { new_z -= 1; yaw = 128; }
        else if (closest_player->z > old_z) { new_z += 1; yaw = 0; }
      }

    }

    // Holds the block that the mob is moving into
    uint8_t block = getBlockAt(new_x, new_y, new_z);
    // Holds the block above the target block, i.e. the "head" block
    uint8_t block_above = getBlockAt(new_x, new_y + 1, new_z);

    // Validate movement on X axis
    if (new_x != old_x && (
      !isPassableBlock(getBlockAt(new_x, new_y + 1, old_z)) ||
      (
        !isPassableBlock(getBlockAt(new_x, new_y, old_z)) &&
        !isPassableBlock(getBlockAt(new_x, new_y + 2, old_z))
      )
    )) {
      new_x = old_x;
      block = getBlockAt(old_x, new_y, new_z);
      block_above = getBlockAt(old_x, new_y + 1, new_z);
    }
    // Validate movement on Z axis
    if (new_z != old_z && (
      !isPassableBlock(getBlockAt(old_x, new_y + 1, new_z)) ||
      (
        !isPassableBlock(getBlockAt(old_x, new_y, new_z)) &&
        !isPassableBlock(getBlockAt(old_x, new_y + 2, new_z))
      )
    )) {
      new_z = old_z;
      block = getBlockAt(new_x, new_y, old_z);
      block_above = getBlockAt(new_x, new_y + 1, old_z);
    }
    // Validate diagonal movement
    if (new_x != old_x && new_z != old_z && (
      !isPassableBlock(block_above) ||
      (
        !isPassableBlock(block) &&
        !isPassableBlock(getBlockAt(new_x, new_y + 2, new_z))
      )
    )) {
      // We know that movement along just one axis is fine thanks to the
      // Checks above, pick one based on proximity.
      int dist_x = abs(old_x - closest_player->x);
      int dist_z = abs(old_z - closest_player->z);
      if (dist_x < dist_z) new_z = old_z;
      else new_x = old_x;
      block = getBlockAt(new_x, new_y, new_z);
    }

    // Check if we're supposed to climb/drop one block
    // The checks above already ensure that there's enough space to climb
    if (!isPassableBlock(block)) new_y += 1;
    else if (isPassableBlock(getBlockAt(new_x, new_y - 1, new_z))) new_y -= 1;

    // Exit early if all movement was cancelled
    if (new_x == mob_data[i].x && new_z == old_z && new_y == old_y) continue;

    // Prevent collisions with other mobs
    uint8_t colliding = false;
    for (int j = 0; j < MAX_MOBS; j ++) {
      if (j == i) continue;
      if (mob_data[j].type == 0) continue;
      if (
        mob_data[j].x == new_x &&
        mob_data[j].z == new_z &&
        abs((int)mob_data[j].y - (int)new_y) < 2
      ) {
        colliding = true;
        break;
      }
    }
    if (colliding) continue;

    if ( // Hurt mobs that stumble into lava
      (block >= B_lava && block < B_lava + 4) ||
      (block_above >= B_lava && block_above < B_lava + 4)
    ) hurtEntity(entity_id, -1, D_lava, 8);

    // Store new mob position
    mob_data[i].x = new_x;
    mob_data[i].y = new_y;
    mob_data[i].z = new_z;

    // Vary the yaw angle to look just a little less robotic
    yaw += ((r >> 7) & 31) - 16;

    // Broadcast relevant entity movement packets
    for (int j = 0; j < MAX_PLAYERS; j ++) {
      if (player_data[j].client_fd == -1) continue;
      sc_moveEntityPosRot (
        player_data[j].client_fd, entity_id,
        (double)old_x + 0.5, (double)old_y, (double)old_z + 0.5,
        (double)new_x + 0.5, (double)new_y, (double)new_z + 0.5,
        yaw, 0
      );
      sc_setHeadRotation(player_data[j].client_fd, entity_id, yaw);
    }

  }

}

#ifdef ALLOW_CHESTS
// Broadcasts a chest slot update to all clients who have that chest open,
// Except for the client who initiated the update.
void broadcastChestUpdate (int origin_fd, uint8_t *storage_ptr, uint16_t item, uint8_t count, uint8_t slot) {

  for (int i = 0; i < MAX_PLAYERS; i ++) {
    if (player_data[i].client_fd == -1) continue;
    if (player_data[i].flags & 0x20) continue;
    // Filter for players that have this chest open
    if (memcmp(player_data[i].craft_items, &storage_ptr, sizeof(storage_ptr)) != 0) continue;
    // Send slot update packet
    sc_setContainerSlot(player_data[i].client_fd, 2, slot, count, item);
  }

  #ifndef DISK_SYNC_BLOCKS_ON_INTERVAL
  writeChestChangesToDisk(storage_ptr, slot);
  #endif

}
#endif

ssize_t writeEntityData (int client_fd, EntityData *data) {
  writeByte(client_fd, data->index);
  writeVarInt(client_fd, data->type);

  switch (data->type) {
    case 0: // Byte
      return writeByte(client_fd, data->value.byte);
    case 21: // Pose
      writeVarInt(client_fd, data->value.pose);
      return 0;

    default: return -1;
  }
}

// Returns the networked size of an EntityData entry
int sizeEntityData (EntityData *data) {
  int value_size;

  switch (data->type) {
    case 0: // Byte
      value_size = 1;
      break;
    case 21: // Pose
      value_size = sizeVarInt(data->value.pose);
      break;

    default: return -1;
  }

  return 1 + sizeVarInt(data->type) + value_size;
}

// Returns the networked size of an array of EntityData entries
int sizeEntityMetadata (EntityData *metadata, size_t length) {
  int total_size = 0;
  for (size_t i = 0; i < length; i ++) {
    int size = sizeEntityData(&metadata[i]);
    if (size == -1) return -1;
    total_size += size;
  }
  return total_size;
}
