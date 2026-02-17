#include "globals.h"

#ifdef SYNC_WORLD_TO_DISK

#ifdef ESP_PLATFORM
  #include "esp_littlefs.h"
  #define FILE_PATH "/littlefs/world.bin"
#else
  #include <stdio.h>
  #define FILE_PATH "world.bin"
#endif

#include "tools.h"
#include "registries.h"
#include "serialize.h"

int64_t last_disk_sync_time = 0;

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
