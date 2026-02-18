#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdlib.h>

#ifdef ESP_PLATFORM
  #include "lwip/sockets.h"
  #include "lwip/netdb.h"
  #include "esp_task_wdt.h"
#else
  #ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
  #else
    #include <arpa/inet.h>
  #endif
  #include <unistd.h>
#endif

#include "globals.h"
#include "tools.h"
#include "varnum.h"
#include "registries.h"
#include "worldgen.h"
#include "crafting.h"
#include "procedures.h"
#include "packets.h"

static void writeOverworldContext (int client_fd) {
  const char *dimension = "minecraft:overworld";
  int dimension_len = (int)strlen(dimension);
  // CommonPlayerSpawnInfo.dimensionType.
  // Notchian 1.21.11 encodes overworld as varint 0 in this context.
  writeVarInt(client_fd, 0);
  // CommonPlayerSpawnInfo.dimension (ResourceKey<Level>)
  writeVarInt(client_fd, dimension_len);
  send_all(client_fd, dimension, dimension_len);
  // CommonPlayerSpawnInfo.seed
  writeUint64(client_fd, 0x0123456789ABCDEF);
  // CommonPlayerSpawnInfo.gameType
  writeByte(client_fd, GAMEMODE);
  // CommonPlayerSpawnInfo.previousGameType (-1 means none)
  writeByte(client_fd, 0xFF);
  // CommonPlayerSpawnInfo.isDebug / isFlat
  writeByte(client_fd, 0);
  writeByte(client_fd, 0);
  // CommonPlayerSpawnInfo.lastDeathLocation (Optional<GlobalPos>) - absent
  writeByte(client_fd, 0);
  // CommonPlayerSpawnInfo.portalCooldown
  writeVarInt(client_fd, 0);
  // CommonPlayerSpawnInfo.seaLevel
  writeVarInt(client_fd, 63);
}

static uint8_t sky_light_full[2048];
static uint8_t sky_light_dark[2048];
static uint8_t sky_light_buffers_initialized = false;
static int8_t template_chunks_enabled_cached = -1;

static void initSkyLightBuffers () {
  if (sky_light_buffers_initialized) return;
  for (int i = 0; i < 2048; i ++) {
    sky_light_full[i] = 0xFF;
    sky_light_dark[i] = 0x00;
  }
  sky_light_buffers_initialized = true;
}

static uint8_t templateChunksEnabled () {
  if (template_chunks_enabled_cached != -1) return (uint8_t)template_chunks_enabled_cached;
  const char *env = getenv("NETHR_DISABLE_TEMPLATE_CHUNKS");
  if (env != NULL && env[0] == '1') template_chunks_enabled_cached = false;
  else template_chunks_enabled_cached = true;
  if (!template_chunks_enabled_cached) {
    printf("Template chunks disabled by env NETHR_DISABLE_TEMPLATE_CHUNKS=1; using procedural encoder\n\n");
  }
  return (uint8_t)template_chunks_enabled_cached;
}

#define CHUNK_TEMPLATE_POOL_MAX 64
#define CHUNK_TEMPLATE_ASSIGN_CAPACITY 16384
#define CHUNK_TEMPLATE_SPAWN_SAFE_RADIUS 3

// Template pool for compatibility mode: we replay known-good Notchian
// level_chunk_with_light packets and patch only chunk x/z coordinates.
static uint8_t *chunk_template_0x2c_pool[CHUNK_TEMPLATE_POOL_MAX];
static size_t chunk_template_0x2c_pool_len[CHUNK_TEMPLATE_POOL_MAX];
static int32_t chunk_template_0x2c_src_x[CHUNK_TEMPLATE_POOL_MAX];
static int32_t chunk_template_0x2c_src_z[CHUNK_TEMPLATE_POOL_MAX];
static int chunk_template_0x2c_pool_count = 0;
static uint8_t chunk_template_0x2c_pool_loaded = false;
static int32_t chunk_template_grid_min_x = 0;
static int32_t chunk_template_grid_max_x = 0;
static int32_t chunk_template_grid_min_z = 0;
static int32_t chunk_template_grid_max_z = 0;
static int chunk_template_grid_width = 0;
static int chunk_template_grid_height = 0;
static uint8_t chunk_template_grid_complete = false;
static int16_t chunk_template_grid_lookup[CHUNK_TEMPLATE_POOL_MAX];
static int chunk_template_spawn_anchor_index = -1;
static int chunk_template_spawn_anchor_gx = 0;
static int chunk_template_spawn_anchor_gz = 0;
static int32_t readInt32BE (const uint8_t *buf);

static int loadChunkTemplateFile (const char *path) {
  if (chunk_template_0x2c_pool_count >= CHUNK_TEMPLATE_POOL_MAX) return 0;
  FILE *fp = fopen(path, "rb");
  if (fp == NULL) return 0;

  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return 0;
  }
  long size = ftell(fp);
  if (size <= 0 || size > 1 << 20) {
    fclose(fp);
    return 0;
  }
  rewind(fp);

  uint8_t *buf = malloc((size_t)size);
  if (buf == NULL) {
    fclose(fp);
    return 0;
  }
  size_t read_n = fread(buf, 1, (size_t)size, fp);
  fclose(fp);
  if (read_n != (size_t)size || buf[0] != 0x2C || size < 9) {
    free(buf);
    return 0;
  }

  int idx = chunk_template_0x2c_pool_count;
  chunk_template_0x2c_pool[idx] = buf;
  chunk_template_0x2c_pool_len[idx] = (size_t)size;
  chunk_template_0x2c_src_x[idx] = readInt32BE(buf + 1);
  chunk_template_0x2c_src_z[idx] = readInt32BE(buf + 5);
  chunk_template_0x2c_pool_count ++;
  return 1;
}

// Runtime assignment cache:
// world chunk (x,z) -> chosen template index.
// We keep this stable so revisits never "flip" terrain variants.
typedef struct {
  int32_t x;
  int32_t z;
  int16_t template_index;
  uint8_t used;
} ChunkTemplateAssignment;

static ChunkTemplateAssignment chunk_template_assignments[CHUNK_TEMPLATE_ASSIGN_CAPACITY];

static void writeInt32BE (uint8_t *buf, int32_t v) {
  uint32_t u = (uint32_t)v;
  buf[0] = (uint8_t)((u >> 24) & 0xFF);
  buf[1] = (uint8_t)((u >> 16) & 0xFF);
  buf[2] = (uint8_t)((u >> 8) & 0xFF);
  buf[3] = (uint8_t)(u & 0xFF);
}

static int32_t readInt32BE (const uint8_t *buf) {
  uint32_t u =
    ((uint32_t)buf[0] << 24) |
    ((uint32_t)buf[1] << 16) |
    ((uint32_t)buf[2] << 8) |
    (uint32_t)buf[3];
  return (int32_t)u;
}

static uint32_t hashChunkCoord (int32_t x, int32_t z) {
  uint32_t ux = (uint32_t)x;
  uint32_t uz = (uint32_t)z;
  return ux * 73856093u ^ uz * 19349663u;
}

static int findChunkTemplateAssignmentSlot (int32_t x, int32_t z, uint8_t create) {
  uint32_t h = hashChunkCoord(x, z);
  int first_free = -1;
  for (int i = 0; i < CHUNK_TEMPLATE_ASSIGN_CAPACITY; i ++) {
    int slot = (int)((h + (uint32_t)i) % CHUNK_TEMPLATE_ASSIGN_CAPACITY);
    ChunkTemplateAssignment *entry = &chunk_template_assignments[slot];
    if (!entry->used) {
      if (!create) return -1;
      if (first_free == -1) first_free = slot;
      break;
    }
    if (entry->x == x && entry->z == z) return slot;
  }
  if (!create || first_free == -1) return -1;
  chunk_template_assignments[first_free].used = true;
  chunk_template_assignments[first_free].x = x;
  chunk_template_assignments[first_free].z = z;
  chunk_template_assignments[first_free].template_index = -1;
  return first_free;
}

static int getChunkTemplateAssignment (int32_t x, int32_t z) {
  int slot = findChunkTemplateAssignmentSlot(x, z, false);
  if (slot == -1) return -1;
  return chunk_template_assignments[slot].template_index;
}

static void setChunkTemplateAssignment (int32_t x, int32_t z, int template_index) {
  int slot = findChunkTemplateAssignmentSlot(x, z, true);
  if (slot == -1) return;
  chunk_template_assignments[slot].template_index = (int16_t)template_index;
}

static void templateGridXY (int template_index, int *gx, int *gz) {
  // Convert absolute source chunk coords into local grid coords.
  *gx = chunk_template_0x2c_src_x[template_index] - chunk_template_grid_min_x;
  *gz = chunk_template_0x2c_src_z[template_index] - chunk_template_grid_min_z;
}

static int clampInt (int value, int min_value, int max_value) {
  if (value < min_value) return min_value;
  if (value > max_value) return max_value;
  return value;
}

static int gridLookupIndex (int gx, int gz) {
  if (!chunk_template_grid_complete) return -1;
  if (gx < 0 || gz < 0) return -1;
  if (gx >= chunk_template_grid_width || gz >= chunk_template_grid_height) return -1;
  int index = gz * chunk_template_grid_width + gx;
  if (index < 0 || index >= CHUNK_TEMPLATE_POOL_MAX) return -1;
  return index;
}

static int templateIndexAtGrid (int gx, int gz) {
  int lookup = gridLookupIndex(gx, gz);
  if (lookup == -1) return -1;
  int idx = chunk_template_grid_lookup[lookup];
  if (idx < 0 || idx >= chunk_template_0x2c_pool_count) return -1;
  return idx;
}

static uint8_t isSpawnSafeAreaChunk (int32_t world_x, int32_t world_z) {
  // Keep the central spawn region coherent and easy to traverse.
  return
    world_x >= -CHUNK_TEMPLATE_SPAWN_SAFE_RADIUS &&
    world_x <= CHUNK_TEMPLATE_SPAWN_SAFE_RADIUS &&
    world_z >= -CHUNK_TEMPLATE_SPAWN_SAFE_RADIUS &&
    world_z <= CHUNK_TEMPLATE_SPAWN_SAFE_RADIUS;
}

static int selectTemplateForSpawnArea (int32_t world_x, int32_t world_z) {
  if (!chunk_template_grid_complete) return -1;
  // Use a contiguous source window around a spawn anchor that prefers flatter
  // chunks (small packet size heuristic). Clamp instead of wrap to avoid
  // cyclic patterns directly around spawn.
  int want_gx = chunk_template_spawn_anchor_gx + (int)world_x;
  int want_gz = chunk_template_spawn_anchor_gz + (int)world_z;
  want_gx = clampInt(want_gx, 0, chunk_template_grid_width - 1);
  want_gz = clampInt(want_gz, 0, chunk_template_grid_height - 1);
  return templateIndexAtGrid(want_gx, want_gz);
}

static int selectTemplateByNeighbors (int32_t world_x, int32_t world_z) {
  if (chunk_template_0x2c_pool_count <= 0) return -1;

  // If we cannot reason on a coherent source grid, fallback to deterministic hash.
  if (!chunk_template_grid_complete) {
    uint32_t h = hashChunkCoord(world_x, world_z);
    return (int)(h % (uint32_t)chunk_template_0x2c_pool_count);
  }

  if (isSpawnSafeAreaChunk(world_x, world_z)) {
    int spawn_idx = selectTemplateForSpawnArea(world_x, world_z);
    if (spawn_idx != -1) return spawn_idx;
  }

  // Neighbor constraints:
  // left/right/up/down already-assigned chunks provide "ideal" next source cell.
  int left = getChunkTemplateAssignment(world_x - 1, world_z);
  int right = getChunkTemplateAssignment(world_x + 1, world_z);
  int up = getChunkTemplateAssignment(world_x, world_z - 1);
  int down = getChunkTemplateAssignment(world_x, world_z + 1);

  int best_index = -1;
  int best_score = 0x7FFFFFFF;
  uint32_t jitter = hashChunkCoord(world_x, world_z);

  for (int i = 0; i < chunk_template_0x2c_pool_count; i ++) {
    int gx, gz;
    templateGridXY(i, &gx, &gz);
    int score = 0;
    int constraints = 0;

    if (left != -1) {
      int lgx, lgz;
      templateGridXY(left, &lgx, &lgz);
      int want_x = lgx + 1;
      int want_z = lgz;
      int dx = gx - want_x;
      int dz = gz - want_z;
      score += dx * dx + dz * dz;
      constraints ++;
    }
    if (right != -1) {
      int rgx, rgz;
      templateGridXY(right, &rgx, &rgz);
      int want_x = rgx - 1;
      int want_z = rgz;
      int dx = gx - want_x;
      int dz = gz - want_z;
      score += dx * dx + dz * dz;
      constraints ++;
    }
    if (up != -1) {
      int ugx, ugz;
      templateGridXY(up, &ugx, &ugz);
      int want_x = ugx;
      int want_z = ugz + 1;
      int dx = gx - want_x;
      int dz = gz - want_z;
      score += dx * dx + dz * dz;
      constraints ++;
    }
    if (down != -1) {
      int dgx, dgz;
      templateGridXY(down, &dgx, &dgz);
      int want_x = dgx;
      int want_z = dgz - 1;
      int dx = gx - want_x;
      int dz = gz - want_z;
      score += dx * dx + dz * dz;
      constraints ++;
    }

    if (constraints == 0) {
      // No neighbors yet (frontier start): stable pseudo-random seed.
      score = (int)((hashChunkCoord(world_x, world_z) + (uint32_t)i * 2654435761u) & 0x7FFF);
    } else {
      // Tiny deterministic jitter breaks ties while preserving continuity bias.
      score += (int)((jitter ^ (uint32_t)i * 1103515245u) & 7u);
    }

    if (score < best_score) {
      best_score = score;
      best_index = i;
    }
  }

  if (best_index == -1) {
    uint32_t h = hashChunkCoord(world_x, world_z);
    best_index = (int)(h % (uint32_t)chunk_template_0x2c_pool_count);
  }
  return best_index;
}

static void tryLoadChunkTemplate0x2cPool () {
  if (!templateChunksEnabled()) return;
  if (chunk_template_0x2c_pool_loaded) return;
  chunk_template_0x2c_pool_loaded = true;

  // Load chunk_template_00.bin, chunk_template_01.bin, ... .
  // We do not stop at first gap, so a missing file does not disable the pool.
  int files_found = 0;
  for (int i = 0; i < CHUNK_TEMPLATE_POOL_MAX; i ++) {
    char path[96];
    snprintf(path, sizeof(path), "assets/chunks/chunk_template_%02d.bin", i);
    files_found += loadChunkTemplateFile(path);
  }

  // Backward-compatible fallback: old single-template capture path.
  if (chunk_template_0x2c_pool_count == 0) {
    files_found += loadChunkTemplateFile("assets/chunk_template_1.21.11_0x2c.bin");
  }

  if (chunk_template_0x2c_pool_count == 0) {
    printf("Chunk template pool unavailable (assets/chunks empty or invalid); using built-in encoder\n");
    printf("Hint: run `make template-refresh` while Notchian is running on 127.0.0.1:25566\n\n");
    return;
  }

  // Detect whether templates form a complete rectangular source grid.
  // If true we can preserve neighbor continuity when remapping chunks.
  chunk_template_grid_min_x = chunk_template_grid_max_x = chunk_template_0x2c_src_x[0];
  chunk_template_grid_min_z = chunk_template_grid_max_z = chunk_template_0x2c_src_z[0];
  for (int i = 1; i < chunk_template_0x2c_pool_count; i ++) {
    if (chunk_template_0x2c_src_x[i] < chunk_template_grid_min_x) chunk_template_grid_min_x = chunk_template_0x2c_src_x[i];
    if (chunk_template_0x2c_src_x[i] > chunk_template_grid_max_x) chunk_template_grid_max_x = chunk_template_0x2c_src_x[i];
    if (chunk_template_0x2c_src_z[i] < chunk_template_grid_min_z) chunk_template_grid_min_z = chunk_template_0x2c_src_z[i];
    if (chunk_template_0x2c_src_z[i] > chunk_template_grid_max_z) chunk_template_grid_max_z = chunk_template_0x2c_src_z[i];
  }
  chunk_template_grid_width = chunk_template_grid_max_x - chunk_template_grid_min_x + 1;
  chunk_template_grid_height = chunk_template_grid_max_z - chunk_template_grid_min_z + 1;
  for (int i = 0; i < CHUNK_TEMPLATE_POOL_MAX; i ++) chunk_template_grid_lookup[i] = -1;
  chunk_template_spawn_anchor_index = -1;
  chunk_template_spawn_anchor_gx = 0;
  chunk_template_spawn_anchor_gz = 0;
  chunk_template_grid_complete = false;
  if (chunk_template_grid_width > 0 && chunk_template_grid_height > 0 &&
    chunk_template_grid_width * chunk_template_grid_height == chunk_template_0x2c_pool_count
  ) {
    chunk_template_grid_complete = true;
    for (int z = chunk_template_grid_min_z; z <= chunk_template_grid_max_z; z ++) {
      for (int x = chunk_template_grid_min_x; x <= chunk_template_grid_max_x; x ++) {
        uint8_t found = false;
        for (int i = 0; i < chunk_template_0x2c_pool_count; i ++) {
          if (chunk_template_0x2c_src_x[i] == x && chunk_template_0x2c_src_z[i] == z) {
            found = true;
            int gx = x - chunk_template_grid_min_x;
            int gz = z - chunk_template_grid_min_z;
            int lookup = gridLookupIndex(gx, gz);
            if (lookup != -1) chunk_template_grid_lookup[lookup] = (int16_t)i;
            break;
          }
        }
        if (!found) {
          chunk_template_grid_complete = false;
          break;
        }
      }
      if (!chunk_template_grid_complete) break;
    }
  }

  if (chunk_template_grid_complete) {
    // Spawn anchor heuristic:
    // choose the template with the smallest packet body (usually flatter,
    // lower-detail terrain) so joining feels closer to grassland/plains.
    size_t best_len = (size_t)-1;
    for (int i = 0; i < chunk_template_0x2c_pool_count; i ++) {
      size_t len = chunk_template_0x2c_pool_len[i];
      if (len < best_len) {
        best_len = len;
        chunk_template_spawn_anchor_index = i;
      }
    }
    if (chunk_template_spawn_anchor_index >= 0) {
      templateGridXY(chunk_template_spawn_anchor_index, &chunk_template_spawn_anchor_gx, &chunk_template_spawn_anchor_gz);
    }
  }

  printf(
    "Loaded notchian chunk template pool (0x2C): %d templates (files_loaded=%d)\n",
    chunk_template_0x2c_pool_count, files_found
  );
  printf(
    "  Source span: x=[%d..%d] z=[%d..%d], grid=%dx%d, complete=%s, spawn_safe_radius=%d\n",
    chunk_template_grid_min_x, chunk_template_grid_max_x,
    chunk_template_grid_min_z, chunk_template_grid_max_z,
    chunk_template_grid_width, chunk_template_grid_height,
    chunk_template_grid_complete ? "yes" : "no",
    CHUNK_TEMPLATE_SPAWN_SAFE_RADIUS
  );
  if (chunk_template_spawn_anchor_index >= 0) {
    printf(
      "  Spawn anchor: template=%d src=(%d,%d) body_len=%zu (flat/plains heuristic)\n\n",
      chunk_template_spawn_anchor_index,
      chunk_template_0x2c_src_x[chunk_template_spawn_anchor_index],
      chunk_template_0x2c_src_z[chunk_template_spawn_anchor_index],
      chunk_template_0x2c_pool_len[chunk_template_spawn_anchor_index]
    );
  } else {
    printf("  Spawn anchor: unavailable (non-complete grid)\n\n");
  }
}

static int readVarIntFromMemory (const uint8_t *data, size_t len, size_t *offset, uint32_t *value) {
  uint32_t result = 0;
  int shift = 0;
  while (*offset < len && shift <= 28) {
    uint8_t byte = data[(*offset)++];
    result |= (uint32_t)(byte & 0x7F) << shift;
    if (!(byte & 0x80)) {
      *value = result;
      return 0;
    }
    shift += 7;
  }
  return -1;
}

static void logPacketStreamSummary (const char *label, const uint8_t *data, size_t len) {
  printf("%s stream summary (%zu bytes):\n", label, len);

  size_t offset = 0;
  int packet_index = 0;
  while (offset < len) {
    size_t length_offset = offset;
    uint32_t packet_len = 0;
    if (readVarIntFromMemory(data, len, &offset, &packet_len)) {
      printf("  [%d] invalid packet length varint at offset %zu\n", packet_index, length_offset);
      break;
    }
    if (offset + packet_len > len) {
      printf(
        "  [%d] invalid packet boundary: offset=%zu packet_len=%" PRIu32 " total=%zu\n",
        packet_index, offset, packet_len, len
      );
      break;
    }

    size_t packet_start = offset;
    uint32_t packet_id = 0;
    if (readVarIntFromMemory(data, len, &offset, &packet_id) || offset > packet_start + packet_len) {
      printf("  [%d] invalid packet id varint at payload offset %zu\n", packet_index, packet_start);
      break;
    }

    printf(
      "  [%d] id=0x%02" PRIX32 " payload=%" PRIu32 " packet_len=%" PRIu32 "\n",
      packet_index, packet_id, (uint32_t)(packet_len - (offset - packet_start)), packet_len
    );

    offset = packet_start + packet_len;
    packet_index ++;
  }

  if (offset == len) printf("  stream parse complete (%d packets)\n", packet_index);
  printf("\n");
}

static void logRegistryDataDetails (const uint8_t *data, size_t len) {
  size_t offset = 0;
  int packet_index = 0;

  while (offset < len) {
    uint32_t packet_len = 0;
    size_t packet_len_off = offset;
    if (readVarIntFromMemory(data, len, &offset, &packet_len)) {
      printf("  [registry:%d] invalid packet length at offset %zu\n", packet_index, packet_len_off);
      return;
    }
    if (offset + packet_len > len) {
      printf("  [registry:%d] packet overruns stream (off=%zu len=%" PRIu32 " total=%zu)\n", packet_index, offset, packet_len, len);
      return;
    }

    size_t packet_end = offset + packet_len;
    uint32_t packet_id = 0;
    if (readVarIntFromMemory(data, packet_end, &offset, &packet_id)) {
      printf("  [registry:%d] invalid packet id\n", packet_index);
      return;
    }
    if (packet_id != 0x07) {
      printf("  [registry:%d] unexpected packet id 0x%02" PRIX32 "\n", packet_index, packet_id);
      offset = packet_end;
      packet_index ++;
      continue;
    }

    uint32_t registry_name_len = 0;
    if (readVarIntFromMemory(data, packet_end, &offset, &registry_name_len) ||
      offset + registry_name_len > packet_end
    ) {
      printf("  [registry:%d] invalid registry name\n", packet_index);
      return;
    }
    const char *registry_name = (const char *)(data + offset);
    int is_dimension_type =
      registry_name_len == strlen("minecraft:dimension_type") &&
      memcmp(registry_name, "minecraft:dimension_type", registry_name_len) == 0;
    printf("  [registry:%d] name=%.*s\n", packet_index, (int)registry_name_len, registry_name);
    offset += registry_name_len;

    uint32_t entry_count = 0;
    if (readVarIntFromMemory(data, packet_end, &offset, &entry_count)) {
      printf("  [registry:%d] invalid entry count\n", packet_index);
      return;
    }
    printf("    entries=%" PRIu32 "\n", entry_count);

    for (uint32_t i = 0; i < entry_count; i ++) {
      uint32_t entry_name_len = 0;
      if (readVarIntFromMemory(data, packet_end, &offset, &entry_name_len) ||
        offset + entry_name_len > packet_end
      ) {
        printf("    entry[%" PRIu32 "] invalid name\n", i);
        return;
      }
      const char *entry_name = (const char *)(data + offset);
      offset += entry_name_len;

      if (offset >= packet_end) {
        printf("    entry[%" PRIu32 "] missing data flag\n", i);
        return;
      }
      uint8_t has_data = data[offset++];

      if (i < 3) {
        printf(
          "    entry[%" PRIu32 "]=%.*s has_data=%u\n",
          i, (int)entry_name_len, entry_name, has_data
        );
      }
      if (is_dimension_type && has_data != 0) {
        printf(
          "    WARNING: dimension_type entry %.*s has_data=%u (expected 0/reference in current protocol)\n",
          (int)entry_name_len, entry_name, has_data
        );
      }
    }

    if (entry_count > 3) printf("    ... %" PRIu32 " more entries\n", entry_count - 3);
    if (offset != packet_end) {
      printf("    WARNING: packet has %zu unread trailing bytes\n", packet_end - offset);
      offset = packet_end;
    }
    packet_index ++;
  }
  printf("\n");
}

static size_t appendByte (uint8_t *out, size_t off, uint8_t v) {
  out[off++] = v;
  return off;
}

static size_t appendUint32BE (uint8_t *out, size_t off, uint32_t v) {
  out[off++] = (uint8_t)((v >> 24) & 0xFF);
  out[off++] = (uint8_t)((v >> 16) & 0xFF);
  out[off++] = (uint8_t)((v >> 8) & 0xFF);
  out[off++] = (uint8_t)(v & 0xFF);
  return off;
}

static size_t appendUint64BE (uint8_t *out, size_t off, uint64_t v) {
  out[off++] = (uint8_t)((v >> 56) & 0xFF);
  out[off++] = (uint8_t)((v >> 48) & 0xFF);
  out[off++] = (uint8_t)((v >> 40) & 0xFF);
  out[off++] = (uint8_t)((v >> 32) & 0xFF);
  out[off++] = (uint8_t)((v >> 24) & 0xFF);
  out[off++] = (uint8_t)((v >> 16) & 0xFF);
  out[off++] = (uint8_t)((v >> 8) & 0xFF);
  out[off++] = (uint8_t)(v & 0xFF);
  return off;
}

static size_t appendVarInt (uint8_t *out, size_t off, uint32_t v) {
  while (true) {
    if ((v & ~0x7FU) == 0) {
      out[off++] = (uint8_t)v;
      return off;
    }
    out[off++] = (uint8_t)((v & 0x7F) | 0x80);
    v >>= 7;
  }
}

static void dumpHex (const char *label, const uint8_t *buf, size_t len) {
  printf("%s (%zu bytes)\n", label, len);
  for (size_t i = 0; i < len; i += 16) {
    printf("  %04zx: ", i);
    size_t row_end = i + 16;
    if (row_end > len) row_end = len;
    for (size_t j = i; j < row_end; j ++) printf("%02X ", buf[j]);
    printf("\n");
  }
  printf("\n");
}

// S->C Status Response (server list ping)
int sc_statusResponse (int client_fd) {

  char header[] = "{"
    "\"version\":{\"name\":\"1.21.11\",\"protocol\":774},"
    "\"description\":{\"text\":\"";
  char footer[] = "\"}}";

  uint16_t string_len = sizeof(header) + sizeof(footer) + motd_len - 2;

  writeVarInt(client_fd, 1 + string_len + sizeVarInt(string_len));
  writeByte(client_fd, 0x00);

  writeVarInt(client_fd, string_len);
  send_all(client_fd, header, sizeof(header) - 1);
  send_all(client_fd, motd, motd_len);
  send_all(client_fd, footer, sizeof(footer) - 1);

  return 0;
}

// C->S Handshake
int cs_handshake (int client_fd) {
  printf("Received Handshake:\n");

  printf("  Protocol version: %d\n", (int)readVarInt(client_fd));
  readString(client_fd);
  if (recv_count == -1) return 1;
  printf("  Server address: %s\n", recv_buffer);
  printf("  Server port: %u\n", readUint16(client_fd));
  int intent = readVarInt(client_fd);
  if (intent == VARNUM_ERROR) return 1;
  printf("  Intent: %d\n\n", intent);
  setClientState(client_fd, intent);

  return 0;
}

// C->S Login Start
int cs_loginStart (int client_fd, uint8_t *uuid, char *name) {
  printf("Received Login Start:\n");

  readString(client_fd);
  if (recv_count == -1) return 1;
  strncpy(name, (char *)recv_buffer, 16 - 1);
  name[16 - 1] = '\0';
  printf("  Player name: %s\n", name);
  recv_count = recv_all(client_fd, recv_buffer, 16, false);
  if (recv_count == -1) return 1;
  memcpy(uuid, recv_buffer, 16);
  printf("  Player UUID: ");
  for (int i = 0; i < 16; i ++) printf("%x", uuid[i]);
  printf("\n\n");

  return 0;
}

// S->C Login Success
int sc_loginSuccess (int client_fd, uint8_t *uuid, char *name) {
  printf("Sending Login Success...\n\n");

  uint8_t name_length = strlen(name);
  writeVarInt(client_fd, 1 + 16 + sizeVarInt(name_length) + name_length + 1);
  writeVarInt(client_fd, 0x02);
  send_all(client_fd, uuid, 16);
  writeVarInt(client_fd, name_length);
  send_all(client_fd, name, name_length);
  writeVarInt(client_fd, 0);

  return 0;
}

int cs_clientInformation (int client_fd) {
  int tmp;
  printf("Received Client Information:\n");
  readString(client_fd);
  if (recv_count == -1) return 1;
  printf("  Locale: %s\n", recv_buffer);
  tmp = readByte(client_fd);
  if (recv_count == -1) return 1;
  printf("  View distance: %d\n", tmp);
  tmp = readVarInt(client_fd);
  if (recv_count == -1) return 1;
  printf("  Chat mode: %d\n", tmp);
  tmp = readByte(client_fd);
  if (recv_count == -1) return 1;
  if (tmp) printf("  Chat colors: on\n");
  else printf("  Chat colors: off\n");
  tmp = readByte(client_fd);
  if (recv_count == -1) return 1;
  printf("  Skin parts: %d\n", tmp);
  tmp = readVarInt(client_fd);
  if (recv_count == -1) return 1;
  if (tmp) printf("  Main hand: right\n");
  else printf("  Main hand: left\n");
  tmp = readByte(client_fd);
  if (recv_count == -1) return 1;
  if (tmp) printf("  Text filtering: on\n");
  else printf("  Text filtering: off\n");
  tmp = readByte(client_fd);
  if (recv_count == -1) return 1;
  if (tmp) printf("  Allow listing: on\n");
  else printf("  Allow listing: off\n");
  tmp = readVarInt(client_fd);
  if (recv_count == -1) return 1;
  printf("  Particles: %d\n\n", tmp);
  return 0;
}

// S->C Clientbound Known Packs
int sc_knownPacks (int client_fd) {
  printf("Sending Server's Known Packs\n\n");
  char known_packs[] = {
    0x0e, 0x01, 0x09, 0x6d, 0x69, 0x6e,
    0x65, 0x63, 0x72, 0x61, 0x66, 0x74, 0x04, 0x63,
    0x6f, 0x72, 0x65, 0x07, 0x31, 0x2e, 0x32, 0x31,
    0x2e, 0x31, 0x31
  };
  writeVarInt(client_fd, 25);
  send_all(client_fd, &known_packs, 25);
  return 0;
}

// S->C Update Enabled Features (configuration)
int sc_updateEnabledFeatures (int client_fd) {
  static const char feature_vanilla[] = "minecraft:vanilla";
  int feature_len = (int)strlen(feature_vanilla);

  printf("Sending Update Enabled Features\n");
  printf("  [0] %s\n\n", feature_vanilla);

  // packet id + feature count + string length + string bytes
  writeVarInt(client_fd, 1 + 1 + sizeVarInt(feature_len) + feature_len);
  writeVarInt(client_fd, 0x0C);
  writeVarInt(client_fd, 1);
  writeVarInt(client_fd, feature_len);
  send_all(client_fd, feature_vanilla, feature_len);
  return 0;
}

// C->S Serverbound Plugin Message
int cs_pluginMessage (int client_fd) {
  printf("Received Plugin Message:\n");
  readString(client_fd);
  if (recv_count == -1) return 1;
  printf("  Channel: \"%s\"\n", recv_buffer);
  if (strcmp((char *)recv_buffer, "minecraft:brand") == 0) {
    readString(client_fd);
    if (recv_count == -1) return 1;
    printf("  Brand: \"%s\"\n", recv_buffer);
  }
  printf("\n");
  return 0;
}

int cs_knownPacks (int client_fd, int payload_len) {
  uint64_t start_bytes = total_bytes_received;
  int count = readVarInt(client_fd);
  if (recv_count == -1) return 1;

  printf("Received Client's Known Packs\n");
  printf("  Entry count: %d\n", count);

  for (int i = 0; i < count; i ++) {
    readString(client_fd);
    if (recv_count == -1) return 1;
    printf("  [%d] Namespace: %s\n", i, recv_buffer);

    readString(client_fd);
    if (recv_count == -1) return 1;
    printf("  [%d] ID: %s\n", i, recv_buffer);

    readString(client_fd);
    if (recv_count == -1) return 1;
    printf("  [%d] Version: %s\n", i, recv_buffer);
  }

  uint64_t consumed = total_bytes_received - start_bytes;
  if ((int)consumed < payload_len) {
    size_t trailing = (size_t)(payload_len - consumed);
    printf("  WARNING: %zu trailing bytes left in known packs payload, discarding\n", trailing);
    discard_all(client_fd, trailing, false);
  } else if ((int)consumed > payload_len) {
    printf("  WARNING: Known packs parser consumed %" PRIu64 " bytes, expected payload_len=%d\n", consumed, payload_len);
  }

  printf("  Parsed payload bytes: %" PRIu64 " (expected %d)\n", consumed, payload_len);
  printf("  Finishing configuration\n\n");
  return 0;
}

// S->C Clientbound Plugin Message
int sc_sendPluginMessage (int client_fd, const char *channel, const uint8_t *data, size_t data_len) {
  printf("Sending Plugin Message\n\n");
  int channel_len = (int)strlen(channel);

  writeVarInt(client_fd, 1 + sizeVarInt(channel_len) + channel_len + sizeVarInt(data_len) + data_len);
  writeByte(client_fd, 0x01);

  writeVarInt(client_fd, channel_len);
  send_all(client_fd, channel, channel_len);

  writeVarInt(client_fd, data_len);
  send_all(client_fd, data, data_len);

  return 0;
}

// S->C Finish Configuration
int sc_finishConfiguration (int client_fd) {
  printf("Sending Finish Configuration (packet id 0x03)\n\n");
  writeVarInt(client_fd, 1);
  writeVarInt(client_fd, 0x03);
  return 0;
}

// S->C Login (play)
int sc_loginPlay (int client_fd) {
  const char *spawn_dimension = "minecraft:overworld";
  const char *dimensions[] = {
    "minecraft:overworld",
    "minecraft:the_nether",
    "minecraft:the_end"
  };
  int dimension_count = (int)(sizeof(dimensions) / sizeof(dimensions[0]));
  int spawn_dimension_len = (int)strlen(spawn_dimension);
  int dimensions_len = 0;
  for (int i = 0; i < dimension_count; i ++) {
    int len = (int)strlen(dimensions[i]);
    dimensions_len += sizeVarInt(len) + len;
  }
  int common_spawn_info_len =
    sizeVarInt(0) +
    sizeVarInt(spawn_dimension_len) + spawn_dimension_len +
    8 +
    1 + 1 +
    1 + 1 +
    1 +
    sizeVarInt(0) +
    sizeVarInt(63);
  int payload_len =
    4 +
    1 +
    sizeVarInt(dimension_count) +
    dimensions_len +
    sizeVarInt(MAX_PLAYERS) +
    sizeVarInt(VIEW_DISTANCE) +
    sizeVarInt(VIEW_DISTANCE) +
    1 + 1 + 1 +
    common_spawn_info_len +
    1;
  int framed_len = payload_len + 1;

  // 1.21.11 play/clientbound "login" packet id is 0x30.
  // Payload layout follows ClientboundLoginPacket + CommonPlayerSpawnInfo.
  printf("Sending Play Login (packet id 0x30, length %d)\n", framed_len);
  printf("  Spawn dimension key: %s, dimensionTypeHolderId=%d\n", spawn_dimension, 0);
  printf("  Breakdown: commonSpawnInfo=%d, payload=%d, framed=%d\n\n", common_spawn_info_len, payload_len, framed_len);

  uint8_t login_dbg[256];
  size_t off = 0;
  off = appendVarInt(login_dbg, off, (uint32_t)framed_len);
  off = appendByte(login_dbg, off, 0x30);
  off = appendUint32BE(login_dbg, off, (uint32_t)client_fd);
  off = appendByte(login_dbg, off, false);
  off = appendVarInt(login_dbg, off, (uint32_t)dimension_count);
  for (int i = 0; i < dimension_count; i ++) {
    int len = (int)strlen(dimensions[i]);
    off = appendVarInt(login_dbg, off, (uint32_t)len);
    memcpy(login_dbg + off, dimensions[i], (size_t)len);
    off += (size_t)len;
  }
  off = appendVarInt(login_dbg, off, MAX_PLAYERS);
  off = appendVarInt(login_dbg, off, VIEW_DISTANCE);
  off = appendVarInt(login_dbg, off, VIEW_DISTANCE);
  off = appendByte(login_dbg, off, 0);
  off = appendByte(login_dbg, off, true);
  off = appendByte(login_dbg, off, false);
  off = appendVarInt(login_dbg, off, 0);
  off = appendVarInt(login_dbg, off, (uint32_t)spawn_dimension_len);
  memcpy(login_dbg + off, spawn_dimension, (size_t)spawn_dimension_len);
  off += (size_t)spawn_dimension_len;
  off = appendUint64BE(login_dbg, off, 0x0123456789ABCDEFULL);
  off = appendByte(login_dbg, off, GAMEMODE);
  off = appendByte(login_dbg, off, 0xFF);
  off = appendByte(login_dbg, off, 0);
  off = appendByte(login_dbg, off, 0);
  off = appendByte(login_dbg, off, 0);
  off = appendVarInt(login_dbg, off, 0);
  off = appendVarInt(login_dbg, off, 63);
  off = appendByte(login_dbg, off, false);
  dumpHex("Play Login bytes", login_dbg, off);
  if ((int)off != framed_len + sizeVarInt(framed_len)) {
    printf(
      "WARNING: Play Login debug frame size mismatch: expected total=%d got=%zu\n\n",
      framed_len + sizeVarInt(framed_len), off
    );
  }

  writeVarInt(client_fd, framed_len);
  writeByte(client_fd, 0x30);
  // Entity id
  writeUint32(client_fd, client_fd);
  // Hardcore
  writeByte(client_fd, false);
  // Dimensions
  writeVarInt(client_fd, dimension_count);
  for (int i = 0; i < dimension_count; i ++) {
    int len = (int)strlen(dimensions[i]);
    writeVarInt(client_fd, len);
    send_all(client_fd, dimensions[i], len);
  }
  // Maxplayers
  writeVarInt(client_fd, MAX_PLAYERS);
  // View distance
  writeVarInt(client_fd, VIEW_DISTANCE);
  // Sim distance
  writeVarInt(client_fd, VIEW_DISTANCE);
  // Reduced debug info
  writeByte(client_fd, 0);
  // Respawn screen
  writeByte(client_fd, true);
  // Limited crafting
  writeByte(client_fd, false);
  // CommonPlayerSpawnInfo.
  writeOverworldContext(client_fd);
  // enforcesSecureChat
  writeByte(client_fd, false);

  return 0;

}

// S->C Synchronize Player Position
int sc_synchronizePlayerPosition (int client_fd, double x, double y, double z, float yaw, float pitch) {

  writeVarInt(client_fd, 61 + sizeVarInt(-1));
  // 1.21.11: play/clientbound player_position
  writeByte(client_fd, 0x46);

  // Teleport ID
  writeVarInt(client_fd, -1);

  // Position
  writeDouble(client_fd, x);
  writeDouble(client_fd, y);
  writeDouble(client_fd, z);

  // Velocity
  writeDouble(client_fd, 0);
  writeDouble(client_fd, 0);
  writeDouble(client_fd, 0);

  // Angles (Yaw/Pitch)
  writeFloat(client_fd, yaw);
  writeFloat(client_fd, pitch);

  // Flags
  writeUint32(client_fd, 0);

  return 0;

}

// S->C Set Default Spawn Position
int sc_setDefaultSpawnPosition (int client_fd, const char *dimension, int64_t x, int64_t y, int64_t z, float yaw, float pitch) {

  // 1.21.11: play/clientbound set_default_spawn_position
  int dimension_len = (int)strlen(dimension);
  int payload_len = sizeVarInt(dimension_len) + dimension_len + 8 + 4 + 4;
  writeVarInt(client_fd, sizeVarInt(0x5F) + payload_len);
  writeVarInt(client_fd, 0x5F);

  uint64_t packed_pos =
    (((uint64_t)x & 0x3FFFFFFULL) << 38) |
    (((uint64_t)z & 0x3FFFFFFULL) << 12) |
    ((uint64_t)y & 0xFFFULL);
  printf(
    "Sending Set Default Spawn Position (packet id 0x5F, dim=%s x=%lld y=%lld z=%lld yaw=%.2f pitch=%.2f packed=0x%016llX)\n\n",
    dimension, (long long)x, (long long)y, (long long)z, yaw, pitch, (unsigned long long)packed_pos
  );
  writeVarInt(client_fd, dimension_len);
  send_all(client_fd, dimension, dimension_len);
  writeUint64(client_fd, packed_pos);
  writeFloat(client_fd, yaw);
  writeFloat(client_fd, pitch);

  return 0;
}

// S->C Player Abilities (clientbound)
int sc_playerAbilities (int client_fd, uint8_t flags) {

  writeVarInt(client_fd, 10);
  // 1.21.11: play/clientbound player_abilities
  writeByte(client_fd, 0x3E);

  writeByte(client_fd, flags);
  writeFloat(client_fd, 0.05f);
  writeFloat(client_fd, 0.1f);

  return 0;
}

// S->C Update Time
int sc_updateTime (int client_fd, uint64_t ticks) {

  writeVarInt(client_fd, 18);
  // 1.21.11: play/clientbound set_time
  writeVarInt(client_fd, 0x6F);

  uint64_t world_age = get_program_time() / 50000;
  #ifdef CHUNK_TEMPLATE_VISIBILITY_COMPAT
    ticks = 6000; // Midday for better visibility in template-chunk mode.
  #endif
  writeUint64(client_fd, world_age);
  writeUint64(client_fd, ticks);
  #ifdef CHUNK_TEMPLATE_VISIBILITY_COMPAT
    writeByte(client_fd, false); // Freeze daylight cycle while debugging.
  #else
    writeByte(client_fd, true);
  #endif

  return 0;
}

// S->C Game Event 13 (Start waiting for level chunks)
int sc_startWaitingForChunks (int client_fd) {
  writeVarInt(client_fd, 6);
  // 1.21.11: play/clientbound game_event
  writeByte(client_fd, 0x26);
  writeByte(client_fd, 13);
  writeUint32(client_fd, 0);
  return 0;
}

// S->C Set Center Chunk
int sc_setCenterChunk (int client_fd, int x, int y) {
  writeVarInt(client_fd, 1 + sizeVarInt(x) + sizeVarInt(y));
  // 1.21.11: play/clientbound set_chunk_cache_center
  writeByte(client_fd, 0x5C);
  writeVarInt(client_fd, x);
  writeVarInt(client_fd, y);
  return 0;
}

// S->C Chunk Data and Update Light
int sc_chunkDataAndUpdateLight (int client_fd, int _x, int _z) {
  tryLoadChunkTemplate0x2cPool();
  if (chunk_template_0x2c_pool_count > 0) {
    // Assign once per world chunk and reuse forever in this process.
    int template_index = getChunkTemplateAssignment(_x, _z);
    if (template_index < 0 || template_index >= chunk_template_0x2c_pool_count) {
      template_index = selectTemplateByNeighbors(_x, _z);
      setChunkTemplateAssignment(_x, _z, template_index);
    }
    size_t body_len = chunk_template_0x2c_pool_len[template_index];
    uint8_t *body = malloc(body_len);
    if (body == NULL) return 1;
    memcpy(body, chunk_template_0x2c_pool[template_index], body_len);
    // Packet body layout starts with: id(0x2C), chunk_x(i32), chunk_z(i32)
    writeInt32BE(body + 1, _x);
    writeInt32BE(body + 5, _z);

    static uint8_t logged_once = false;
    if (!logged_once) {
      printf(
        "Chunk encoder v7: using notchian 0x2C template pool (%d variants), grid_complete=%s, sample_body_len=%zu\n\n",
        chunk_template_0x2c_pool_count, chunk_template_grid_complete ? "yes" : "no", body_len
      );
      logged_once = true;
    }

    writeVarInt(client_fd, (uint32_t)body_len);
    send_all(client_fd, body, (ssize_t)body_len);
    free(body);
    return 0;
  }

  initSkyLightBuffers();

  const int chunk_data_size = (4101 + sizeVarInt(256) + sizeof(network_block_palette)) * 20 + 6 * 12;
  const int light_data_size = 14 + (sizeVarInt(2048) + 2048) * 26;
  static uint8_t logged_once = false;
  if (!logged_once) {
    printf(
      "Chunk encoder v5: packet_id=0x2C body_len=%d chunk_data_size=%d (legacy-large)\n\n",
      11 + sizeVarInt(chunk_data_size) + chunk_data_size + light_data_size, chunk_data_size
    );
    logged_once = true;
  }

  writeVarInt(client_fd, 11 + sizeVarInt(chunk_data_size) + chunk_data_size + light_data_size);
  // 1.21.11: play/clientbound level_chunk_with_light
  writeByte(client_fd, 0x2C);

  writeUint32(client_fd, _x);
  writeUint32(client_fd, _z);

  writeVarInt(client_fd, 0); // Omit heightmaps

  writeVarInt(client_fd, chunk_data_size);

  int x = _x * 16, z = _z * 16, y;

  // Send 4 chunk sections (up to Y=0) with no blocks
  for (int i = 0; i < 4; i ++) {
    writeUint16(client_fd, 4096); // Block count
    writeByte(client_fd, 0); // Block bits
    writeVarInt(client_fd, 85); // Block palette (bedrock)
    writeByte(client_fd, 0); // Biome bits
    writeByte(client_fd, 0); // Biome palette
  }
  task_yield();

  // Send chunk sections
  for (int i = 0; i < 20; i ++) {
    y = i * 16;
    writeUint16(client_fd, 4096); // Block count
    writeByte(client_fd, 8); // Bits per entry
    writeVarInt(client_fd, 256); // Block palette length
    send_all(client_fd, network_block_palette, sizeof(network_block_palette));
    uint8_t biome = buildChunkSection(x, y, z);
    send_all(client_fd, chunk_section, 4096);
    writeByte(client_fd, 0); // Bits per entry
    writeByte(client_fd, biome); // Biome palette
    task_yield();
  }

  // Send 8 chunk sections (up to Y=192) with no blocks
  for (int i = 0; i < 8; i ++) {
    writeUint16(client_fd, 4096); // Block count
    writeByte(client_fd, 0); // Block bits
    writeVarInt(client_fd, 0); // Block palette (air)
    writeByte(client_fd, 0); // Biome bits
    writeByte(client_fd, 0); // Biome palette
  }

  writeVarInt(client_fd, 0); // Omit block entities

  // Light data
  writeVarInt(client_fd, 1);
  writeUint64(client_fd, 0b11111111111111111111111111);
  writeVarInt(client_fd, 0);
  writeVarInt(client_fd, 0);
  writeVarInt(client_fd, 0);

  // Sky light array
  writeVarInt(client_fd, 28);
  for (int i = 0; i < 8; i ++) {
    writeVarInt(client_fd, 2048);
    send_all(client_fd, sky_light_dark, 2048);
  }
  for (int i = 0; i < 18; i ++) {
    writeVarInt(client_fd, 2048);
    send_all(client_fd, sky_light_full, 2048);
  }
  // Don't send block light
  writeVarInt(client_fd, 0);

  // Sending block updates changes light prediciton on the client.
  // Light-emitting blocks are omitted from chunk data so that they can
  // Be overlayed here. This seems to be cheaper than sending actual
  // Block light data.
  for (int i = firstBlockChangeInChunk(_x, _z); i != -1; i = nextIndexedBlockChange(i)) {
    if (div_floor(block_changes[i].x, 16) != _x) continue;
    if (div_floor(block_changes[i].z, 16) != _z) continue;
    #ifdef ALLOW_CHESTS
      if (block_changes[i].block != B_torch && block_changes[i].block != B_chest) continue;
    #else
      if (block_changes[i].block != B_torch) continue;
    #endif
    sc_blockUpdate(client_fd, block_changes[i].x, block_changes[i].y, block_changes[i].z, block_changes[i].block);
  }

  return 0;

}

// S->C Clientbound Keep Alive (play)
int sc_keepAlive (int client_fd) {

  writeVarInt(client_fd, 9);
  writeByte(client_fd, 0x2B);

  writeUint64(client_fd, 0);

  return 0;
}

// S->C Set Container Slot
int sc_setContainerSlot (int client_fd, int window_id, uint16_t slot, uint8_t count, uint16_t item) {

  writeVarInt(client_fd,
    1 +
    sizeVarInt(window_id) +
    1 + 2 +
    sizeVarInt(count) +
    (count > 0 ? sizeVarInt(item) + 2 : 0)
  );
  writeByte(client_fd, 0x14);

  writeVarInt(client_fd, window_id);
  writeVarInt(client_fd, 0);
  writeUint16(client_fd, slot);

  writeVarInt(client_fd, count);
  if (count > 0) {
    writeVarInt(client_fd, item);
    writeVarInt(client_fd, 0);
    writeVarInt(client_fd, 0);
  }

  return 0;

}

// S->C Block Update
int sc_blockUpdate (int client_fd, int64_t x, int64_t y, int64_t z, uint8_t block) {
  writeVarInt(client_fd, 9 + sizeVarInt(block_palette[block]));
  writeByte(client_fd, 0x08);
  writeUint64(client_fd, ((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF));
  writeVarInt(client_fd, block_palette[block]);
  return 0;
}

// S->C Acknowledge Block Change
int sc_acknowledgeBlockChange (int client_fd, int sequence) {
  writeVarInt(client_fd, 1 + sizeVarInt(sequence));
  writeByte(client_fd, 0x04);
  writeVarInt(client_fd, sequence);
  return 0;
}

// C->S Player Action
int cs_playerAction (int client_fd) {

  uint8_t action = readByte(client_fd);

  int64_t pos = readInt64(client_fd);
  int x = pos >> 38;
  int y = pos << 52 >> 52;
  int z = pos << 26 >> 38;

  readByte(client_fd); // Ignore face

  int sequence = readVarInt(client_fd);
  sc_acknowledgeBlockChange(client_fd, sequence);

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

  handlePlayerAction(player, action, x, y, z);

  return 0;

}

// S->C Open Screen
int sc_openScreen (int client_fd, uint8_t window, const char *title, uint16_t length) {

  writeVarInt(client_fd, 1 + 2 * sizeVarInt(window) + 1 + 2 + length);
  // 1.21.11: play/clientbound open_screen
  writeByte(client_fd, 0x39);

  writeVarInt(client_fd, window);
  writeVarInt(client_fd, window);

  writeByte(client_fd, 8); // String nbt tag
  writeUint16(client_fd, length); // String length
  send_all(client_fd, title, length);

  return 0;
}

// C->S Use Item
int cs_useItem (int client_fd) {

  uint8_t hand = readByte(client_fd);
  int sequence = readVarInt(client_fd);

  // Ignore yaw/pitch
  recv_all(client_fd, recv_buffer, 8, false);

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

  handlePlayerUseItem(player, 0, 0, 0, 255);

  return 0;
}

// C->S Use Item On
int cs_useItemOn (int client_fd) {

  uint8_t hand = readByte(client_fd);

  int64_t pos = readInt64(client_fd);
  int x = pos >> 38;
  int y = pos << 52 >> 52;
  int z = pos << 26 >> 38;

  uint8_t face = readByte(client_fd);

  // Ignore cursor position
  readUint32(client_fd);
  readUint32(client_fd);
  readUint32(client_fd);

  // Ignore "inside block" and "world border hit"
  readByte(client_fd);
  readByte(client_fd);

  int sequence = readVarInt(client_fd);
  sc_acknowledgeBlockChange(client_fd, sequence);

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

  handlePlayerUseItem(player, x, y, z, face);

  return 0;
}

// C->S Click Container
int cs_clickContainer (int client_fd) {

  int window_id = readVarInt(client_fd);

  readVarInt(client_fd); // Ignore state id

  int16_t clicked_slot = readInt16(client_fd);
  uint8_t button = readByte(client_fd);
  uint8_t mode = readVarInt(client_fd);

  int changes_count = readVarInt(client_fd);

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

  uint8_t apply_changes = true;
  // Prevent dropping items
  if (mode == 4 && clicked_slot != -999) {
    // When using drop button, re-sync the respective slot
    uint8_t slot = clientSlotToServerSlot(window_id, clicked_slot);
    sc_setContainerSlot(client_fd, window_id, clicked_slot, player->inventory_count[slot], player->inventory_items[slot]);
    apply_changes = false;
  } else if (mode == 0 && clicked_slot == -999) {
    // When clicking outside inventory, return the dropped item to the player
    if (button == 0) {
      givePlayerItem(player, player->flagval_16, player->flagval_8);
      player->flagval_16 = 0;
      player->flagval_8 = 0;
    } else {
      givePlayerItem(player, player->flagval_16, 1);
      player->flagval_8 -= 1;
      if (player->flagval_8 == 0) player->flagval_16 = 0;
    }
    apply_changes = false;
  }

  uint8_t slot, count, craft = false;
  uint16_t item;
  int tmp;

  uint16_t *p_item;
  uint8_t *p_count;

  #ifdef ALLOW_CHESTS
  // See the handlePlayerUseItem function for more info on this hack
  uint8_t *storage_ptr;
  memcpy(&storage_ptr, player->craft_items, sizeof(storage_ptr));
  #endif

  for (int i = 0; i < changes_count; i ++) {

    slot = clientSlotToServerSlot(window_id, readUint16(client_fd));
    // Slots outside of the inventory overflow into the crafting buffer
    if (slot > 40 && apply_changes) craft = true;

    #ifdef ALLOW_CHESTS
    if (window_id == 2 && slot > 40) {
      // Get item pointers from the player's storage pointer
      // See the handlePlayerUseItem function for more info on this hack
      p_item = (uint16_t *)(storage_ptr + (slot - 41) * 3);
      p_count = storage_ptr + (slot - 41) * 3 + 2;
    } else
    #endif
    {
      // Prevent accessing crafting-related slots when craft_items is locked
      if (slot > 40 && player->flags & 0x80) return 1;
      p_item = &player->inventory_items[slot];
      p_count = &player->inventory_count[slot];
    }

    if (!readByte(client_fd)) { // No item?
      if (slot != 255 && apply_changes) {
        *p_item = 0;
        *p_count = 0;
        #ifdef ALLOW_CHESTS
        if (window_id == 2 && slot > 40) {
          broadcastChestUpdate(client_fd, storage_ptr, 0, 0, slot - 41);
        }
        #endif
      }
      continue;
    }

    item = readVarInt(client_fd);
    count = (uint8_t)readVarInt(client_fd);

    // Ignore components
    readLengthPrefixedData(client_fd);
    readLengthPrefixedData(client_fd);

    if (count > 0 && apply_changes) {
      *p_item = item;
      *p_count = count;
      #ifdef ALLOW_CHESTS
      if (window_id == 2 && slot > 40) {
        broadcastChestUpdate(client_fd, storage_ptr, item, count, slot - 41);
      }
      #endif
    }

  }

  // Window 0 is player inventory, window 12 is crafting table
  if (craft && (window_id == 0 || window_id == 12)) {
    getCraftingOutput(player, &count, &item);
    sc_setContainerSlot(client_fd, window_id, 0, count, item);
  } else if (window_id == 14) { // Furnace
    getSmeltingOutput(player);
    for (int i = 0; i < 3; i ++) {
      sc_setContainerSlot(client_fd, window_id, i, player->craft_count[i], player->craft_items[i]);
    }
  }

  // Assign cursor-carried item slot
  if (readByte(client_fd)) {
    player->flagval_16 = readVarInt(client_fd);
    player->flagval_8 = readVarInt(client_fd);
    // Ignore components
    readLengthPrefixedData(client_fd);
    readLengthPrefixedData(client_fd);
  } else {
    player->flagval_16 = 0;
    player->flagval_8 = 0;
  }

  return 0;

}

// S->C Set Cursor Item
int sc_setCursorItem (int client_fd, uint16_t item, uint8_t count) {

  writeVarInt(client_fd, 1 + sizeVarInt(count) + (count != 0 ? sizeVarInt(item) + 2 : 0));
  // 1.21.11: play/clientbound set_cursor_item
  writeByte(client_fd, 0x5E);

  writeVarInt(client_fd, count);
  if (count == 0) return 0;

  writeVarInt(client_fd, item);

  // Skip components
  writeByte(client_fd, 0);
  writeByte(client_fd, 0);

  return 0;
}

// C->S Set Player Position And Rotation
int cs_setPlayerPositionAndRotation (int client_fd, double *x, double *y, double *z, float *yaw, float *pitch, uint8_t *on_ground) {

  *x = readDouble(client_fd);
  *y = readDouble(client_fd);
  *z = readDouble(client_fd);

  *yaw = readFloat(client_fd);
  *pitch = readFloat(client_fd);

  *on_ground = readByte(client_fd) & 0x01;

  return 0;
}

// C->S Set Player Position
int cs_setPlayerPosition (int client_fd, double *x, double *y, double *z, uint8_t *on_ground) {

  *x = readDouble(client_fd);
  *y = readDouble(client_fd);
  *z = readDouble(client_fd);

  *on_ground = readByte(client_fd) & 0x01;

  return 0;
}

// C->S Set Player Rotation
int cs_setPlayerRotation (int client_fd, float *yaw, float *pitch, uint8_t *on_ground) {

  *yaw = readFloat(client_fd);
  *pitch = readFloat(client_fd);

  *on_ground = readByte(client_fd) & 0x01;

  return 0;
}

int cs_setPlayerMovementFlags (int client_fd, uint8_t *on_ground) {

  *on_ground = readByte(client_fd) & 0x01;

  PlayerData *player;
  if (!getPlayerData(client_fd, &player))
    broadcastPlayerMetadata(player);

  return 0;
}

// C->S Swing Arm (serverbound)
int cs_swingArm (int client_fd) {

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

  uint8_t hand = readVarInt(client_fd);

  uint8_t animation = 255;
  switch (hand) {
    case 0: {
      animation = 0;
      break;
    }
    case 1: {
      animation = 2;
      break;
    }
  }

  if (animation == 255)
    return 1;

  // Forward animation to all connected players
  FOR_EACH_VISIBLE_OTHER_PLAYER(i, player->client_fd) {
    PlayerData* other_player = &player_data[i];
    sc_entityAnimation(other_player->client_fd, player->client_fd, animation);
  }

  return 0;
}

// C->S Set Held Item (serverbound)
int cs_setHeldItem (int client_fd) {

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

  uint8_t slot = readUint16(client_fd);
  if (slot >= 9) return 1;

  player->hotbar = slot;

  return 0;
}

// S->C Set Held Item (clientbound)
int sc_setHeldItem (int client_fd, uint8_t slot) {

  // 1.21.11: play/clientbound set_held_slot
  writeVarInt(client_fd, sizeVarInt(0x67) + 1);
  writeVarInt(client_fd, 0x67);

  writeByte(client_fd, slot);

  return 0;
}

// C->S Close Container (serverbound)
int cs_closeContainer (int client_fd) {

  uint8_t window_id = readVarInt(client_fd);

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

  // Return all items in crafting slots to the player
  // Or, in the case of chests, simply clear the storage pointer
  for (uint8_t i = 0; i < 9; i ++) {
    if (window_id != 2) {
      givePlayerItem(player, player->craft_items[i], player->craft_count[i]);
      uint8_t client_slot = serverSlotToClientSlot(window_id, 41 + i);
      if (client_slot != 255) sc_setContainerSlot(player->client_fd, window_id, client_slot, 0, 0);
    }
    player->craft_items[i] = 0;
    player->craft_count[i] = 0;
    // Unlock craft_items
    player->flags &= ~0x80;
  }

  givePlayerItem(player, player->flagval_16, player->flagval_8);
  sc_setCursorItem(client_fd, 0, 0);
  player->flagval_16 = 0;
  player->flagval_8 = 0;

  return 0;
}

// S->C Player Info Update, "Add Player" action
int sc_playerInfoUpdateAddPlayer (int client_fd, PlayerData player) {

  writeVarInt(client_fd, 21 + strlen(player.name)); // Packet length
  // 1.21.11: play/clientbound player_info_update
  writeByte(client_fd, 0x44); // Packet ID

  writeByte(client_fd, 0x01); // EnumSet: Add Player
  writeByte(client_fd, 1); // Player count (1 per packet)

  // Player UUID
  send_all(client_fd, player.uuid, 16);
  // Player name
  writeByte(client_fd, strlen(player.name));
  send_all(client_fd, player.name, strlen(player.name));
  // Properties (don't send any)
  writeByte(client_fd, 0);

  return 0;
}

// S->C Spawn Entity
int sc_spawnEntity (
  int client_fd,
  int id, uint8_t *uuid, int type,
  double x, double y, double z,
  uint8_t yaw, uint8_t pitch
) {

  writeVarInt(client_fd, 51 + sizeVarInt(id) + sizeVarInt(type));
  writeByte(client_fd, 0x01);

  writeVarInt(client_fd, id); // Entity ID
  send_all(client_fd, uuid, 16); // Entity UUID
  writeVarInt(client_fd, type); // Entity type

  // Position
  writeDouble(client_fd, x);
  writeDouble(client_fd, y);
  writeDouble(client_fd, z);

  // 1.21.11 layout matches Notchian order:
  // position -> velocity -> rotations -> data (VarInt).
  // Previous order caused decoder "bytes extra" on add_entity.
  // Velocity (delta movement)
  writeUint16(client_fd, 0);
  writeUint16(client_fd, 0);
  writeUint16(client_fd, 0);

  // Angles
  writeByte(client_fd, pitch);
  writeByte(client_fd, yaw);
  writeByte(client_fd, yaw);

  // Data (VarInt, mostly unused)
  writeVarInt(client_fd, 0);

  return 0;
}

// S->C Set Entity Metadata
int sc_setEntityMetadata (int client_fd, int id, EntityData *metadata, size_t length) {
  int entity_metadata_size = sizeEntityMetadata(metadata, length);
  if (entity_metadata_size == -1) return 1;

  writeVarInt(client_fd, 2 + sizeVarInt(id) + entity_metadata_size);
  // 1.21.11: play/clientbound set_entity_data
  writeByte(client_fd, 0x61);

  writeVarInt(client_fd, id); // Entity ID

  for (size_t i = 0; i < length; i ++) {
    EntityData *data = &metadata[i];
    writeEntityData(client_fd, data);
  }

  writeByte(client_fd, 0xFF); // End

  return 0;
}

// S->C Spawn Entity (from PlayerData)
int sc_spawnEntityPlayer (int client_fd, PlayerData player) {
  return sc_spawnEntity(
    client_fd,
    player.client_fd, player.uuid, 149,
    player.x > 0 ? (double)player.x + 0.5 : (double)player.x - 0.5,
    player.y,
    player.z > 0 ? (double)player.z + 0.5 : (float)player.z - 0.5,
    player.yaw, player.pitch
  );
}

// S->C Entity Animation
int sc_entityAnimation (int client_fd, int id, uint8_t animation) {
  writeVarInt(client_fd, 2 + sizeVarInt(id));
  writeByte(client_fd, 0x02);

  writeVarInt(client_fd, id); // Entity ID
  writeByte(client_fd, animation); // Animation

  return 0;
}

// S->C Teleport Entity
int sc_teleportEntity (
  int client_fd, int id,
  double x, double y, double z,
  float yaw, float pitch
) {

  // Packet length and ID
  writeVarInt(client_fd, 58 + sizeVarInt(id));
  // 1.21.11: play/clientbound teleport_entity
  writeByte(client_fd, 0x7B);

  // Entity ID
  writeVarInt(client_fd, id);
  // Position
  writeDouble(client_fd, x);
  writeDouble(client_fd, y);
  writeDouble(client_fd, z);
  // Velocity
  writeUint64(client_fd, 0);
  writeUint64(client_fd, 0);
  writeUint64(client_fd, 0);
  // Angles
  writeFloat(client_fd, yaw);
  writeFloat(client_fd, pitch);
  // On ground flag
  writeByte(client_fd, 1);

  return 0;
}

// S->C Set Head Rotation
int sc_setHeadRotation (int client_fd, int id, uint8_t yaw) {

  // Packet length and ID
  writeByte(client_fd, 2 + sizeVarInt(id));
  // 1.21.11: play/clientbound rotate_head
  writeByte(client_fd, 0x51);
  // Entity ID
  writeVarInt(client_fd, id);
  // Head yaw
  writeByte(client_fd, yaw);

  return 0;
}

// S->C Set Head Rotation
int sc_updateEntityRotation (int client_fd, int id, uint8_t yaw, uint8_t pitch) {

  // Packet length and ID
  writeByte(client_fd, 4 + sizeVarInt(id));
  // 1.21.11: play/clientbound move_entity_rot
  writeByte(client_fd, 0x36);
  // Entity ID
  writeVarInt(client_fd, id);
  // Angles
  writeByte(client_fd, yaw);
  writeByte(client_fd, pitch);
  // "On ground" flag
  writeByte(client_fd, 1);

  return 0;
}

// S->C Damage Event
int sc_damageEvent (int client_fd, int entity_id, int type) {

  writeVarInt(client_fd, 4 + sizeVarInt(entity_id) + sizeVarInt(type));
  writeByte(client_fd, 0x19);

  writeVarInt(client_fd, entity_id);
  writeVarInt(client_fd, type);
  writeByte(client_fd, 0);
  writeByte(client_fd, 0);
  writeByte(client_fd, false);

  return 0;
}

// S->C Set Health
int sc_setHealth (int client_fd, uint8_t health, uint8_t food, uint16_t saturation) {

  writeVarInt(client_fd, 9 + sizeVarInt(food));
  // 1.21.11: play/clientbound set_health
  writeByte(client_fd, 0x66);

  writeFloat(client_fd, (float)health);
  writeVarInt(client_fd, food);
  writeFloat(client_fd, (float)(saturation - 200) / 500.0f);

  return 0;
}

// S->C Respawn
int sc_respawn (int client_fd) {
  const char *dimension = "minecraft:overworld";
  int dimension_len = (int)strlen(dimension);
  int common_spawn_info_len =
    sizeVarInt(1) +
    sizeVarInt(dimension_len) + dimension_len +
    8 +
    1 + 1 +
    1 + 1 +
    1 +
    sizeVarInt(0) +
    sizeVarInt(63);

  writeVarInt(client_fd, common_spawn_info_len + 2);
  // 1.21.11: play/clientbound respawn
  writeByte(client_fd, 0x50);

  // Dimension/game context.
  writeOverworldContext(client_fd);
  // Keep data mask (none)
  writeByte(client_fd, 0);

  return 0;
}

// C->S Client Status
int cs_clientStatus (int client_fd) {

  uint8_t id = readByte(client_fd);

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

  if (id == 0) {
    sc_respawn(client_fd);
    resetPlayerData(player);
    spawnPlayer(player);
  }

  return 0;
}

// S->C System Chat
int sc_systemChat (int client_fd, char* message, uint16_t len) {

  writeVarInt(client_fd, 5 + len);
  // 1.21.11: play/clientbound system_chat
  writeByte(client_fd, 0x77);

  // String NBT tag
  writeByte(client_fd, 8);
  writeUint16(client_fd, len);
  send_all(client_fd, message, len);

  // Is action bar message?
  writeByte(client_fd, false);

  return 0;
}

// C->S Chat Message
int cs_chat (int client_fd) {

  // To be safe, cap messages to 32 bytes before the buffer length
  readStringN(client_fd, 224);

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

  size_t message_len = strlen((char *)recv_buffer);
  uint8_t name_len = strlen(player->name);

  if (recv_buffer[0] != '!') { // Standard chat message

    // Shift message contents forward to make space for player name tag
    memmove(recv_buffer + name_len + 3, recv_buffer, message_len + 1);
    // Copy player name to index 1
    memcpy(recv_buffer + 1, player->name, name_len);
    // Surround player name with brackets and a space
    recv_buffer[0] = '<';
    recv_buffer[name_len + 1] = '>';
    recv_buffer[name_len + 2] = ' ';

    // Forward message to all connected players
    FOR_EACH_VISIBLE_PLAYER(i) {
      sc_systemChat(player_data[i].client_fd, (char *)recv_buffer, message_len + name_len + 3);
    }

    goto cleanup;
  }

  // Handle chat commands

  if (!strncmp((char *)recv_buffer, "!msg", 4)) {

    int target_offset = 5;
    int target_end_offset = 0;
    int text_offset = 0;

    // Skip spaces after "!msg"
    while (recv_buffer[target_offset] == ' ') target_offset++;
    target_end_offset = target_offset;
    // Extract target name
    while (recv_buffer[target_end_offset] != ' ' && recv_buffer[target_end_offset] != '\0' && target_end_offset < 21) target_end_offset++;
    text_offset = target_end_offset;
    // Skip spaces before message
    while (recv_buffer[text_offset] == ' ') text_offset++;

    // Send usage guide if arguments are missing
    if (target_offset == target_end_offset || target_end_offset == text_offset) {
      sc_systemChat(client_fd, "§7Usage: !msg <player> <message>", 33);
      goto cleanup;
    }

    // Query the target player
    PlayerData *target = getPlayerByName(target_offset, target_end_offset, recv_buffer);
    if (target == NULL) {
      sc_systemChat(client_fd, "Player not found", 16);
      goto cleanup;
    }

    // Format output as a vanilla whisper
    int name_len = strlen(player->name);
    int text_len = message_len - text_offset;
    memmove(recv_buffer + name_len + 24, recv_buffer + text_offset, text_len);
    snprintf((char *)recv_buffer, sizeof(recv_buffer), "§7§o%s whispers to you:", player->name);
    recv_buffer[name_len + 23] = ' ';
    // Send message to target player
    sc_systemChat(target->client_fd, (char *)recv_buffer, (uint16_t)(name_len + 24 + text_len));

    // Format output for sending player
    int target_len = target_end_offset - target_offset;
    memmove(recv_buffer + target_len + 23, recv_buffer + name_len + 24, text_len);
    snprintf((char *)recv_buffer, sizeof(recv_buffer), "§7§oYou whisper to %s:", target->name);
    recv_buffer[target_len + 22] = ' ';
    // Report back to sending player
    sc_systemChat(client_fd, (char *)recv_buffer, (uint16_t)(target_len + 23 + text_len));

    goto cleanup;
  }

  if (!strncmp((char *)recv_buffer, "!help", 5)) {
    // Send command guide
    const char help_msg[] = "§7Commands:\n"
    "  !msg <player> <message> - Send a private message\n"
    "  !nether - Teleport to nether zone\n"
    "  !overworld - Return from nether zone\n"
    "  !help - Show this help message";
    sc_systemChat(client_fd, (char *)help_msg, (uint16_t)sizeof(help_msg) - 1);
    goto cleanup;
  }

  if (!strncmp((char *)recv_buffer, "!nether", 7)) {
    movePlayerToNetherZone(player, true);
    goto cleanup;
  }

  if (!strncmp((char *)recv_buffer, "!overworld", 10)) {
    movePlayerToNetherZone(player, false);
    goto cleanup;
  }

  // Handle fall-through case
  sc_systemChat(client_fd, "§7Unknown command", 18);

cleanup:
  readUint64(client_fd); // Ignore timestamp
  readUint64(client_fd); // Ignore salt
  // Ignore signature (if any)
  uint8_t has_signature = readByte(client_fd);
  if (has_signature) recv_all(client_fd, recv_buffer, 256, false);
  readVarInt(client_fd); // Ignore message count
  // Ignore acknowledgement bitmask and checksum
  recv_all(client_fd, recv_buffer, 4, false);

  return 0;
}

// C->S Interact
int cs_interact (int client_fd) {

  int entity_id = readVarInt(client_fd);
  uint8_t type = readByte(client_fd);

  if (type == 2) {
    // Ignore target coordinates
    recv_all(client_fd, recv_buffer, 12, false);
  }
  if (type != 1) {
    // Ignore hand
    recv_all(client_fd, recv_buffer, 1, false);
  }

  // Ignore sneaking flag
  recv_all(client_fd, recv_buffer, 1, false);

  if (type == 0) { // Interact
    interactEntity(entity_id, client_fd);
  } else if (type == 1) { // Attack
    hurtEntity(entity_id, client_fd, D_generic, 1);
  }

  return 0;
}

// S->C Entity Event
int sc_entityEvent (int client_fd, int entity_id, uint8_t status) {

  writeVarInt(client_fd, 6);
  // 1.21.11: play/clientbound entity_event
  writeByte(client_fd, 0x22);

  writeUint32(client_fd, entity_id);
  writeByte(client_fd, status);

  return 0;
}

// S->C Remove Entities, but for only one entity per packet
int sc_removeEntity (int client_fd, int entity_id) {

  writeVarInt(client_fd, 2 + sizeVarInt(entity_id));
  // 1.21.11: play/clientbound remove_entities
  writeByte(client_fd, 0x4B);

  writeByte(client_fd, 1);
  writeVarInt(client_fd, entity_id);

  return 0;
}

// C->S Player Input
int cs_playerInput (int client_fd) {

  uint8_t flags = readByte(client_fd);

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

  // Set or clear sneaking flag
  if (flags & 0x20) player->flags |= 0x04;
  else player->flags &= ~0x04;

  broadcastPlayerMetadata(player);

  return 0;
}

// C->S Player Command
int cs_playerCommand (int client_fd) {

  readVarInt(client_fd); // Ignore entity ID
  uint8_t action = readByte(client_fd);
  readVarInt(client_fd); // Ignore "Jump Boost" value

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

  // Handle sprinting
  if (action == 1) player->flags |= 0x08;
  else if (action == 2) player->flags &= ~0x08;

  broadcastPlayerMetadata(player);

  return 0;
}

// S->C Pickup Item (take_item_entity)
int sc_pickupItem (int client_fd, int collected, int collector, uint8_t count) {

  writeVarInt(client_fd, 1 + sizeVarInt(collected) + sizeVarInt(collector) + sizeVarInt(count));
  // 1.21.11: play/clientbound take_item_entity
  writeByte(client_fd, 0x7A);

  writeVarInt(client_fd, collected);
  writeVarInt(client_fd, collector);
  writeVarInt(client_fd, count);

  return 0;
}

// C->S Player Loaded
int cs_playerLoaded (int client_fd) {

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

  // Redirect handling to player join procedure
  handlePlayerJoin(player);

  return 0;
}

// C->S Accept Teleportation
int cs_acceptTeleportation (int client_fd) {
  int teleport_id = readVarInt(client_fd);
  printf("Play RX: accept_teleportation id=%d\n", teleport_id);
  return 0;
}

// C->S Chunk Batch Received
int cs_chunkBatchReceived (int client_fd) {
  float desired = readFloat(client_fd);
  printf("Play RX: chunk_batch_received desiredChunksPerTick=%.2f\n", desired);
  return 0;
}

// S->C Registry Data (multiple packets) and Update Tags (configuration, multiple packets)
int sc_registries (int client_fd) {

  printf("Sending Registries (%zu bytes)\n\n", sizeof(registries_bin));
  #ifdef DEBUG_REGISTRY_VERBOSE
    logPacketStreamSummary("Registries", registries_bin, sizeof(registries_bin));
    printf("Registries detailed decode:\n");
    logRegistryDataDetails(registries_bin, sizeof(registries_bin));
  #endif
  send_all(client_fd, registries_bin, sizeof(registries_bin));

  printf("Sending Tags (%zu bytes)\n\n", sizeof(tags_bin));
  #ifdef DEBUG_REGISTRY_VERBOSE
    logPacketStreamSummary("Tags", tags_bin, sizeof(tags_bin));
  #endif
  send_all(client_fd, tags_bin, sizeof(tags_bin));

  return 0;

}
