
#include "globals.h"
#include "tools.h"
#include "registries.h"
#include "worldgen.h"
#include "procedures.h"
#include "structures.h"

void setBlockIfReplaceable (short x, uint8_t y, short z, uint8_t block) {
  uint8_t target = getBlockAt(x, y, z);
  if (!isReplaceableBlock(target) && target != B_oak_leaves) return;
  makeBlockChange(x, y, z, block);
}

// Places an oak tree centered on the given coordinates.
void placeTreeStructure (short x, uint8_t y, short z) {

  // Seed canopy variation and trunk height.
  uint32_t r = fast_rand();
  uint8_t height = 4 + (r % 3);

  // Convert sapling/base blocks into trunk foundation.
  makeBlockChange(x, y - 1, z, B_dirt);
  makeBlockChange(x, y, z, B_oak_log);
  // Build vertical trunk.
  for (int i = 1; i < height; i ++) {
    setBlockIfReplaceable(x, y + i, z, B_oak_log);
  }
  // Corner-variation cursor into random bitstream.
  uint8_t t = 2;
  // Lower canopy layers.
  for (int i = -2; i <= 2; i ++) {
    for (int j = -2; j <= 2; j ++) {
      setBlockIfReplaceable(x + i, y + height - 3, z + j, B_oak_leaves);
      // Skip selected corners for a less cubic canopy.
      if ((i == 2 || i == -2) && (j == 2 || j == -2)) {
        t ++;
        if ((r >> t) & 1) continue;
      }
      setBlockIfReplaceable(x + i, y + height - 2, z + j, B_oak_leaves);
    }
  }
  // Upper canopy layers.
  for (int i = -1; i <= 1; i ++) {
    for (int j = -1; j <= 1; j ++) {
      setBlockIfReplaceable(x + i, y + height - 1, z + j, B_oak_leaves);
      if ((i == 1 || i == -1) && (j == 1 || j == -1)) {
        t ++;
        if ((r >> t) & 1) continue;
      }
      setBlockIfReplaceable(x + i, y + height, z + j, B_oak_leaves);
    }
  }

}
