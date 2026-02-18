#include "globals.h"

#ifdef SYNC_WORLD_TO_DISK

#ifdef ESP_PLATFORM
  #include "esp_littlefs.h"
  #define FILE_PATH "/littlefs/world.bin"
  #define META_FILE_PATH "/littlefs/world.meta"
#else
  #include <stdio.h>
  #define FILE_PATH "world.bin"
  #define META_FILE_PATH "world.meta"
#endif
#include <string.h>

#include "tools.h"
#include "registries.h"
#include "serialize.h"
#include "procedures.h"

int64_t last_disk_sync_time = 0;

// Loads world seed/spawn metadata when present.
// Returns 1 when loaded, 0 when missing, -1 on parse error.
int loadWorldMeta () {
  FILE *file = fopen(META_FILE_PATH, "rb");
  if (!file) return 0;

  char line[128];
  if (!fgets(line, sizeof(line), file)) {
    fclose(file);
    return -1;
  }
  if (strncmp(line, "NETHR_META_V1", 13) != 0) {
    fclose(file);
    return -1;
  }

  uint8_t has_world_seed = false;
  uint8_t has_rng_seed = false;
  uint8_t has_spawn_x = false;
  uint8_t has_spawn_y = false;
  uint8_t has_spawn_z = false;

  while (fgets(line, sizeof(line), file)) {
    unsigned int u = 0;
    int d = 0;
    if (sscanf(line, "WORLD_SEED=%u", &u) == 1) {
      world_seed_raw = (uint32_t)u;
      has_world_seed = true;
      continue;
    }
    if (sscanf(line, "RNG_SEED=%u", &u) == 1) {
      rng_seed_raw = (uint32_t)u;
      has_rng_seed = true;
      continue;
    }
    if (sscanf(line, "SPAWN_X=%d", &d) == 1) {
      world_spawn_x = (short)d;
      has_spawn_x = true;
      continue;
    }
    if (sscanf(line, "SPAWN_Y=%u", &u) == 1) {
      world_spawn_y = (uint8_t)u;
      has_spawn_y = true;
      continue;
    }
    if (sscanf(line, "SPAWN_Z=%d", &d) == 1) {
      world_spawn_z = (short)d;
      has_spawn_z = true;
      continue;
    }
  }
  fclose(file);

  if (!has_world_seed || !has_rng_seed) return -1;
  if (has_spawn_x && has_spawn_y && has_spawn_z) {
    world_spawn_locked = true;
  }

  printf(
    "Loaded world.meta: raw_world_seed=%u raw_rng_seed=%u spawn=%d,%u,%d%s\n",
    world_seed_raw, rng_seed_raw,
    world_spawn_x, world_spawn_y, world_spawn_z,
    world_spawn_locked ? " (fixed)" : " (pending)"
  );

  return 1;
}

// Persists world seed/spawn metadata.
void saveWorldMeta () {
  FILE *file = fopen(META_FILE_PATH, "wb");
  if (!file) {
    perror("Failed to open \"world.meta\" for writing");
    return;
  }
  fprintf(file, "NETHR_META_V1\n");
  fprintf(file, "WORLD_SEED=%u\n", world_seed_raw);
  fprintf(file, "RNG_SEED=%u\n", rng_seed_raw);
  fprintf(file, "SPAWN_X=%d\n", world_spawn_x);
  fprintf(file, "SPAWN_Y=%u\n", world_spawn_y);
  fprintf(file, "SPAWN_Z=%d\n", world_spawn_z);
  fclose(file);
}

// Restores world data from disk, or initializes a new world file.
int initSerializer () {

  last_disk_sync_time = get_program_time();

  #ifdef ESP_PLATFORM
    esp_vfs_littlefs_conf_t conf = {
      .base_path = "/littlefs",
      .partition_label = "littlefs",
      .format_if_mount_failed = true,
      .dont_mount = false
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
      printf("LittleFS error %d\n", ret);
      perror("Failed to mount LittleFS. Aborting.");
      return 1;
    }
  #endif

  // Open existing world file if present.
  FILE *file = fopen(FILE_PATH, "rb");
  if (file) {

    // Read persisted block changes.
    size_t read = fread(block_changes, 1, sizeof(block_changes), file);
    if (read != sizeof(block_changes)) {
      printf("Read %zu bytes from \"world.bin\", expected %zu (block changes). Aborting.\n", read, sizeof(block_changes));
      fclose(file);
      return 1;
    }
    // Rebuild block_changes_count from populated entries.
    for (int i = 0; i < MAX_BLOCK_CHANGES; i ++) {
      if (block_changes[i].block == 0xFF) continue;
      if (block_changes[i].block == B_chest) i += 14;
      if (i >= block_changes_count) block_changes_count = i + 1;
    }
    invalidateBlockChangeIndex();
    // Seek to persisted player section.
    if (fseek(file, sizeof(block_changes), SEEK_SET) != 0) {
      perror("Failed to seek to player data in \"world.bin\". Aborting.");
      fclose(file);
      return 1;
    }
    // Read persisted player data.
    read = fread(player_data, 1, sizeof(player_data), file);
    fclose(file);
    if (read != sizeof(player_data)) {
      printf("Read %zu bytes from \"world.bin\", expected %zu (player data). Aborting.\n", read, sizeof(player_data));
      return 1;
    }

  } else { // No existing world file.
    printf("No \"world.bin\" file found, creating one...\n\n");

    // Create new world file.
    file = fopen(FILE_PATH, "wb");
    if (!file) {
      perror(
        "Failed to open \"world.bin\" for writing.\n"
        "Consider checking permissions or disabling SYNC_WORLD_TO_DISK in \"globals.h\"."
      );
      return 1;
    }
    // Write initial block-change buffer.
    size_t written = fwrite(block_changes, 1, sizeof(block_changes), file);
    if (written != sizeof(block_changes)) {
      perror(
        "Failed to write initial block data to \"world.bin\".\n"
        "Consider checking permissions or disabling SYNC_WORLD_TO_DISK in \"globals.h\"."
      );
      fclose(file);
      return 1;
    }
    // Seek to player section.
    if (fseek(file, sizeof(block_changes), SEEK_SET) != 0) {
      perror(
        "Failed to seek past block changes in \"world.bin\"."
        "Consider checking permissions or disabling SYNC_WORLD_TO_DISK in \"globals.h\"."
      );
      fclose(file);
      return 1;
    }
    // Write initial player buffer.
    written = fwrite(player_data, 1, sizeof(player_data), file);
    fclose(file);
    if (written != sizeof(player_data)) {
      perror(
        "Failed to write initial player data to \"world.bin\".\n"
        "Consider checking permissions or disabling SYNC_WORLD_TO_DISK in \"globals.h\"."
      );
      return 1;
    }

  }

  return 0;
}

// Writes a block-change index range to disk.
void writeBlockChangesToDisk (int from, int to) {

  // Open world file without truncation.
  FILE *file = fopen(FILE_PATH, "r+b");
  if (!file) {
    perror("Failed to open \"world.bin\". Block updates have been dropped.");
    return;
  }

  for (int i = from; i <= to; i ++) {
    // Seek to target block-change entry.
    if (fseek(file, i * sizeof(BlockChange), SEEK_SET) != 0) {
      fclose(file);
      perror("Failed to seek in \"world.bin\". Block updates have been dropped.");
      return;
    }
    // Persist one block-change entry.
    if (fwrite(&block_changes[i], 1, sizeof(BlockChange), file) != sizeof(BlockChange)) {
      fclose(file);
      perror("Failed to write to \"world.bin\". Block updates have been dropped.");
      return;
    }
  }

  fclose(file);
}

// Writes the complete player buffer to disk.
void writePlayerDataToDisk () {

  // Open world file without truncation.
  FILE *file = fopen(FILE_PATH, "r+b");
  if (!file) {
    perror("Failed to open \"world.bin\". Player updates have been dropped.");
    return;
  }
  // Seek to start of player section.
  if (fseek(file, sizeof(block_changes), SEEK_SET) != 0) {
    fclose(file);
    perror("Failed to seek in \"world.bin\". Player updates have been dropped.");
    return;
  }
  // Persist full player array.
  if (fwrite(&player_data, 1, sizeof(player_data), file) != sizeof(player_data)) {
    fclose(file);
    perror("Failed to write to \"world.bin\". Player updates have been dropped.");
    return;
  }

  fclose(file);
}

// Flushes interval-scheduled persistence tasks when due.
void writeDataToDiskOnInterval () {

  // Skip until interval has elapsed.
  if (get_program_time() - last_disk_sync_time < DISK_SYNC_INTERVAL) return;
  last_disk_sync_time = get_program_time();

  // Flush queued datasets.
  writePlayerDataToDisk();
  #ifdef DISK_SYNC_BLOCKS_ON_INTERVAL
  writeBlockChangesToDisk(0, block_changes_count);
  #endif

}

#ifdef ALLOW_CHESTS
// Persists one chest slot update.
void writeChestChangesToDisk (uint8_t *storage_ptr, uint8_t slot) {
  /**
   * Chest item payload is encoded inline after the chest block entry in
   * block_changes. Translate the storage pointer plus slot index back to
   * the owning BlockChange entry so only that record is rewritten.
   */
  int index = (int)(storage_ptr - (uint8_t *)block_changes) / sizeof(BlockChange) + slot / 2;
  writeBlockChangesToDisk(index, index);
}
#endif

#endif
