#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "globals.h"
#include "tools.h"
#include "registries.h"
#include "procedures.h"
#include "worldgen.h"

static uint8_t isNetherZone (int z) {
  return z >= NETHER_ZONE_OFFSET;
}

#define BIOME_CACHE_CAPACITY 4096

typedef struct {
  short x;
  short z;
  uint8_t biome;
  uint8_t used;
} BiomeCacheEntry;

static BiomeCacheEntry biome_cache[BIOME_CACHE_CAPACITY];

static uint32_t hashChunkXZ (short x, short z) {
  uint32_t ux = (uint16_t)x;
  uint32_t uz = (uint16_t)z;
  return (ux * 73856093u) ^ (uz * 19349663u);
}

static uint8_t getBiomeFromClimateUncached (short x, short z);

static float lerp01 (float a, float b, float t) {
  return a + (b - a) * t;
}

static float smoothstep01 (float t) {
  return t * t * (3.0f - 2.0f * t);
}

static float hash01_2d (int x, int z, uint64_t salt) {
  uint64_t key = ((uint64_t)(uint32_t)x << 32) | (uint32_t)z;
  uint32_t h = (uint32_t)splitmix64(key ^ salt ^ world_seed);
  return (float)(h & 0x00FFFFFFu) / 16777215.0f;
}

static float valueNoise2D (int x, int z, int scale, uint64_t salt) {
  int cell_x = div_floor(x, scale);
  int cell_z = div_floor(z, scale);
  float tx = (float)mod_abs(x, scale) / (float)scale;
  float tz = (float)mod_abs(z, scale) / (float)scale;
  tx = smoothstep01(tx);
  tz = smoothstep01(tz);

  float n00 = hash01_2d(cell_x, cell_z, salt);
  float n10 = hash01_2d(cell_x + 1, cell_z, salt);
  float n01 = hash01_2d(cell_x, cell_z + 1, salt);
  float n11 = hash01_2d(cell_x + 1, cell_z + 1, salt);

  float nx0 = lerp01(n00, n10, tx);
  float nx1 = lerp01(n01, n11, tx);
  return lerp01(nx0, nx1, tz);
}

static float fractalNoise2D (int x, int z, uint64_t salt) {
  // Deliberately higher-frequency blend to increase visible terrain variation.
  float n0 = valueNoise2D(x, z, 32, salt ^ 0x9E3779B97F4A7C15ULL);
  float n1 = valueNoise2D(x, z, 12, salt ^ 0xD1B54A32D192ED03ULL);
  float n2 = valueNoise2D(x, z, 6, salt ^ 0x94D049BB133111EBULL);
  return n0 * 0.45f + n1 * 0.33f + n2 * 0.22f;
}

static uint8_t getSurfaceBlockForBiome (uint8_t biome, uint8_t variant, uint8_t height) {
  (void)variant;
  if (height < 63) return B_water;
  if (biome == W_mangrove_swamp) return B_mud;
  if (biome == W_snowy_plains) return B_snowy_grass_block;
  if (biome == W_desert) return B_sand;
  if (biome == W_beach) return B_sand;
  // Plains top layer stays grass; dirt appears below surface.
  return B_grass_block;
}

static uint8_t getFlowerBlockFromHash (uint32_t hash, uint8_t biome) {
  uint8_t v = (uint8_t)(hash & 15);
  if (biome == W_snowy_plains) {
    if (v < 4) return B_allium;
    if (v < 8) return B_azure_bluet;
    if (v < 11) return B_white_tulip;
    if (v < 13) return B_oxeye_daisy;
    return B_lily_of_the_valley;
  }
  // Plains: mixed meadow flowers.
  if (v == 0) return B_dandelion;
  if (v == 1) return B_poppy;
  if (v == 2) return B_cornflower;
  if (v == 3) return B_allium;
  if (v == 4) return B_azure_bluet;
  if (v == 5) return B_red_tulip;
  if (v == 6) return B_orange_tulip;
  if (v == 7) return B_white_tulip;
  if (v == 8) return B_pink_tulip;
  if (v == 9) return B_oxeye_daisy;
  return B_lily_of_the_valley;
}

static uint8_t scaleChanceU8 (uint8_t base, int scale) {
  int v = (int)base * scale;
  if (v > 255) v = 255;
  if (v < 0) v = 0;
  return (uint8_t)v;
}

static float getRiverChannelMask (int x, int z) {
  // Coherent river mask shared between biome/height/surface rules.
  float river_primary = valueNoise2D(x, z, 36, 0xF13A5B9C6D7E8A01ULL) * 2.0f - 1.0f;
  float river_secondary = valueNoise2D(x, z, 14, 0x29CE4AB1D70685F3ULL) * 2.0f - 1.0f;
  float river_abs0 = river_primary < 0.0f ? -river_primary : river_primary;
  float river_abs1 = river_secondary < 0.0f ? -river_secondary : river_secondary;
  float river_shape = river_abs0 * 0.72f + river_abs1 * 0.28f;
  float mask = (0.16f - river_shape) / 0.16f;
  if (mask < 0.0f) mask = 0.0f;
  if (mask > 1.0f) mask = 1.0f;
  return mask;
}

static uint8_t getLocalSlopeAt (int x, int z) {
  int h_n = getHeightAt(x, z - 1);
  int h_s = getHeightAt(x, z + 1);
  int h_w = getHeightAt(x - 1, z);
  int h_e = getHeightAt(x + 1, z);
  int h_min = h_n;
  int h_max = h_n;
  if (h_s < h_min) h_min = h_s;
  if (h_w < h_min) h_min = h_w;
  if (h_e < h_min) h_min = h_e;
  if (h_s > h_max) h_max = h_s;
  if (h_w > h_max) h_max = h_w;
  if (h_e > h_max) h_max = h_e;
  return (uint8_t)(h_max - h_min);
}

static float absf_local (float v) {
  return v < 0.0f ? -v : v;
}

static float samplePseudo3DCaveNoise (int x, int y, int z) {
  // Cheap 3D-ish field via y-warped 2D samples (faster than full valueNoise3D).
  float a = valueNoise2D(x + y * 2, z - y * 2, 28, 0xB13D7A9C24E65F01ULL) * 2.0f - 1.0f;
  float b = valueNoise2D(x - y * 3, z + y, 14, 0xC57E19A40D2B6F83ULL) * 2.0f - 1.0f;
  float c = valueNoise2D(x + y * 5, z + y * 2, 8, 0x91F24DE37A6BC105ULL) * 2.0f - 1.0f;
  return absf_local(a) * 0.50f + absf_local(b) * 0.32f + absf_local(c) * 0.18f;
}

static uint8_t isCaveOpenAt (int x, int y, int z, uint8_t surface_height) {
  if (y <= 1) return false;
  if (y >= (int)surface_height - 5) return false;

  float cave_field = samplePseudo3DCaveNoise(x, y, z);
  float cavern = valueNoise2D(x + y, z + y * 2, 52, 0x2AC9157DB03E64F1ULL);
  float roughness = valueNoise2D(x, z, 24, 0xE43BD8217A6F19C5ULL);

  // Wider cave bands in the lower/mid underground, tighter near surface.
  float depth = (float)((int)surface_height - y);
  float depth_t = depth / 80.0f;
  if (depth_t < 0.0f) depth_t = 0.0f;
  if (depth_t > 1.0f) depth_t = 1.0f;
  float threshold = 0.20f + depth_t * 0.16f + (roughness - 0.5f) * 0.04f;

  // Rare larger caverns.
  if (y > 8 && y < 56 && cavern > 0.74f && cave_field < 0.62f) return true;
  return cave_field < threshold;
}

static uint8_t isSurfaceCaveEntranceAt (int x, int y, int z, uint8_t surface_height, uint8_t biome) {
  if (biome == W_beach || biome == W_mangrove_swamp) return false;
  if (surface_height < 76) return false;
  if (y > (int)surface_height || y < (int)surface_height - 3) return false;

  // Favor cave mouths on steeper/mountainous terrain.
  uint8_t slope = getLocalSlopeAt(x, z);
  if (slope < 4) return false;

  float ridge = valueNoise2D(x, z, 42, 0x6D239C4FA17BE205ULL);
  if (ridge < 0.68f) return false;

  // Create clustered entrances instead of isolated single-block holes.
  float mouth_mask = valueNoise2D(x + y * 2, z - y, 18, 0xA34E716BC59D208FULL);
  if (mouth_mask < 0.80f) return false;

  // Ensure near-surface opening connects to an underground cave body.
  uint8_t connected = false;
  for (int depth = 4; depth <= 10; depth++) {
    int cave_y = (int)surface_height - depth;
    if (cave_y <= 2) break;
    if (isCaveOpenAt(x, cave_y, z, surface_height)) {
      connected = true;
      break;
    }
  }
  if (!connected) return false;

  return samplePseudo3DCaveNoise(x, y, z) < 0.34f;
}

static uint8_t getAquiferFluidAt (int x, int y, int z) {
  if (y < 8) return B_lava;
  if (y >= 64) return B_air;

  float aquifer = valueNoise2D(x, z, 40, 0x7F21CD94AE630B15ULL);
  int fluid_level = 40 + (int)(aquifer * 24.0f); // 40..64
  if (y <= fluid_level) return B_water;
  return B_air;
}

static uint8_t getSurfaceLavaPoolBlock (int x, int y, int z, uint8_t height, uint8_t biome) {
  (void)z;
  if (biome == W_snowy_plains || biome == W_mangrove_swamp) return 0xFF;
  if (height < 64 || height > 98) return 0xFF;

  // Notchian reference: placed_feature/lake_lava_surface uses rarity_filter chance=200.
  // Approximation here: one candidate check per chunk with 1/200 activation.
  int chunk_x = div_floor(x, 16);
  int chunk_z = div_floor(z, 16);
  for (int dz = -1; dz <= 1; dz++) {
    for (int dx = -1; dx <= 1; dx++) {
      int cx = chunk_x + dx;
      int cz = chunk_z + dz;
      uint32_t h = getChunkHash((short)cx, (short)cz);
      if (h % 200 != 0) continue;

      int center_x = cx * 16 + (int)((h >> 5) & 15);
      int center_z = cz * 16 + (int)((h >> 9) & 15);
      int radius = 1 + (int)((h >> 13) & 2); // 1..3

      int adx = x > center_x ? x - center_x : center_x - x;
      int adz = z > center_z ? z - center_z : center_z - z;
      int dist = adx + adz;
      if (dist > radius) continue;

      uint8_t slope = getLocalSlopeAt(center_x, center_z);
      if (slope > 3) continue;

      if (dist <= radius - 1) {
        if (y == height || y == height - 1) return B_lava;
      } else {
        if (y == height || y == height - 1) return B_netherrack;
      }
    }
  }
  return 0xFF;
}

static uint8_t tryRuinedPortalBlock (int x, int y, int z, uint8_t biome, uint8_t *out_block) {
  if (biome == W_beach || biome == W_mangrove_swamp) return false;

  // Notchian reference: structure_set/ruined_portals random_spread spacing=40 separation=15.
  int chunk_x = div_floor(x, 16);
  int chunk_z = div_floor(z, 16);
  int region_x = div_floor(chunk_x, 40);
  int region_z = div_floor(chunk_z, 40);

  for (int rz = -1; rz <= 1; rz++) {
    for (int rx = -1; rx <= 1; rx++) {
      int rgx = region_x + rx;
      int rgz = region_z + rz;
      uint64_t key = ((uint64_t)(uint32_t)rgx << 32) | (uint32_t)rgz;
      uint32_t h = (uint32_t)splitmix64(key ^ world_seed ^ 34222645u);

      int off_x = (int)(h % 25);               // spacing-separation = 25
      int off_z = (int)((h >> 8) % 25);
      int cand_chunk_x = rgx * 40 + off_x;
      int cand_chunk_z = rgz * 40 + off_z;

      int cx = cand_chunk_x * 16 + 8 + (int)((h >> 16) % 5) - 2;
      int cz = cand_chunk_z * 16 + 8 + (int)((h >> 20) % 5) - 2;

      int adx = x > cx ? x - cx : cx - x;
      int adz = z > cz ? z - cz : cz - z;
      if (adx > 5 || adz > 5) continue;

      uint8_t base_y = getHeightAt(cx, cz) + 1;
      if (base_y < 60 || base_y > 116) continue;

      int orient = (h >> 24) & 1;
      int lx = orient ? (z - cz) : (x - cx);
      int lz = orient ? -(x - cx) : (z - cz);
      int ly = y - base_y;

      // Netherrack spread around the ruin base.
      if (ly == -1 && lz >= -2 && lz <= 2 && lx >= -3 && lx <= 3) {
        if (((h >> ((lx + 3) + ((lz + 2) * 3))) & 1) == 0) {
          *out_block = B_netherrack;
          return true;
        }
      }

      // Simplified ruined frame (4x5), partially broken.
      if (lz == 0) {
        uint8_t frame = false;
        if ((lx == -1 || lx == 2) && ly >= 0 && ly <= 4) frame = true;
        if ((ly == 0 || ly == 4) && lx >= -1 && lx <= 2) frame = true;
        if (frame) {
          uint32_t bh = (uint32_t)splitmix64(((uint64_t)(lx + 8) << 32) ^ ((uint64_t)(ly + 16) << 8) ^ (uint64_t)(h));
          if ((bh & 7) == 0) return false; // broken piece
          *out_block = B_obsidian;
          return true;
        }
      }

      // Occasional lava spill near base.
      if (ly == 0 && lz == 1 && lx >= -1 && lx <= 1) {
        if (((h >> 27) & 3) == 0) {
          *out_block = B_lava;
          return true;
        }
      }
    }
  }

  return false;
}

static uint8_t shouldPlaceRiverSurfaceWater (int x, int z, uint8_t height, uint8_t biome, float river_mask) {
  if (biome == W_desert || biome == W_beach) return false;
  // Keep river surface close to sea level to avoid random floating water.
  if (height < 59 || height > 64) return false;
  if (river_mask < 0.86f) return false;

  // Require continuity: at least two direct neighbors also look like river cells.
  int neighbors = 0;
  if (getRiverChannelMask(x + 1, z) > 0.82f) neighbors++;
  if (getRiverChannelMask(x - 1, z) > 0.82f) neighbors++;
  if (getRiverChannelMask(x, z + 1) > 0.82f) neighbors++;
  if (getRiverChannelMask(x, z - 1) > 0.82f) neighbors++;
  return neighbors >= 2;
}

static uint32_t getCoordinateHash (int x, int y, int z) {
  uint64_t xy = ((uint64_t)(uint32_t)x << 32) | (uint32_t)y;
  uint64_t h = splitmix64(xy ^ world_seed);
  return splitmix64(h ^ (uint32_t)z);
}

static uint8_t isWaterfallSpringCandidate (int x, int z, uint8_t height, uint8_t biome) {
  if (biome == W_desert || biome == W_beach) return false;
  if (height < 76) return false;

  float moisture = fractalNoise2D(x, z, 0x4A7C159E1D2B3F67ULL);
  float spring = valueNoise2D(x, z, 20, 0xC7134E9A2B5D8F01ULL);
  if (moisture < 0.52f || spring < 0.82f) return false;

  // Require a local steep edge so springs look like cliff waterfalls.
  int h_n = getHeightAt(x, z - 1);
  int h_s = getHeightAt(x, z + 1);
  int h_w = getHeightAt(x - 1, z);
  int h_e = getHeightAt(x + 1, z);
  int h_min = h_n;
  if (h_s < h_min) h_min = h_s;
  if (h_w < h_min) h_min = h_w;
  if (h_e < h_min) h_min = h_e;
  if ((int)height - h_min < 6) return false;

  return true;
}

uint32_t getChunkHash (short x, short z) {

  uint8_t buf[8];
  memcpy(buf, &x, 2);
  memcpy(buf + 2, &z, 2);
  memcpy(buf + 4, &world_seed, 4);

  return splitmix64(*((uint64_t *)buf));

}

typedef struct {
  float temperature;
  float humidity;
  float continentalness;
  float erosion;
  float weirdness;
} ClimatePoint;

typedef struct {
  uint8_t biome;
  float temperature;
  float humidity;
  float continentalness;
  float erosion;
  float weirdness;
} ClimateTarget;

static float sampleClimateAxis (int qx, int qz, int scale_quarts, uint64_t salt) {
  float n0 = valueNoise2D(qx, qz, scale_quarts, salt ^ 0x9E3779B97F4A7C15ULL);
  float n1 = valueNoise2D(qx, qz, scale_quarts / 2, salt ^ 0xD1B54A32D192ED03ULL);
  float n2 = valueNoise2D(qx, qz, scale_quarts / 4, salt ^ 0x94D049BB133111EBULL);
  return (n0 * 0.62f + n1 * 0.26f + n2 * 0.12f) * 2.0f - 1.0f;
}

static ClimatePoint sampleClimatePoint (short chunk_x, short chunk_z) {
  // Vanilla uses quart positions (4-block grid) for biome climate lookup.
  // We sample the center of our minichunk on the same style of grid.
  int block_x = chunk_x * CHUNK_SIZE + CHUNK_SIZE / 2;
  int block_z = chunk_z * CHUNK_SIZE + CHUNK_SIZE / 2;
  int qx = div_floor(block_x, 4);
  int qz = div_floor(block_z, 4);

  ClimatePoint p;
  p.temperature = sampleClimateAxis(qx, qz, 96, 0xA7F3D95B6C1209E1ULL);
  p.humidity = sampleClimateAxis(qx, qz, 96, 0xC6BC279692B5CC83ULL);
  p.continentalness = sampleClimateAxis(qx, qz, 128, 0x8EBC6AF09C88C6E3ULL);
  p.erosion = sampleClimateAxis(qx, qz, 96, 0x8AF1C94372DE10B5ULL);
  p.weirdness = sampleClimateAxis(qx, qz, 64, 0xD7A9F13E21C4B6A5ULL);
  return p;
}

static float climateDistanceSq (ClimatePoint p, ClimateTarget t) {
  float dt = p.temperature - t.temperature;
  float dh = p.humidity - t.humidity;
  float dc = p.continentalness - t.continentalness;
  float de = p.erosion - t.erosion;
  float dw = p.weirdness - t.weirdness;
  // Weighted distance: continentalness + temperature dominate coarse biome feel.
  return dt * dt * 1.25f + dh * dh * 0.95f + dc * dc * 1.35f + de * de * 0.85f + dw * dw * 0.70f;
}

static uint8_t getBiomeFromClimateUncached (short x, short z) {
  if (isNetherZone(z * CHUNK_SIZE)) return W_desert;
  // Keep spawn approachable, but allow nearby biome diversity.
  if (abs((int)x) <= 10 && abs((int)z) <= 10) return W_plains;

  ClimatePoint climate = sampleClimatePoint(x, z);

  // Limited biome set: fold ocean/coast buckets into beach proxy.
  if (climate.continentalness < -0.40f) return W_beach;
  if (climate.continentalness < -0.20f && climate.erosion > -0.10f) return W_beach;

  static const ClimateTarget targets[] = {
    // Notchian-inspired centers reduced to this project's supported biome set.
    // Centers were derived from vanilla overworld biome-parameter buckets.
    { W_snowy_plains, -0.74f, -0.08f,  0.26f, -0.30f,  0.00f },
    { W_snowy_plains, -0.58f,  0.20f,  0.34f, -0.08f,  0.24f },
    { W_desert,        0.82f, -0.12f,  0.22f, -0.18f,  0.00f },
    { W_desert,        0.72f,  0.18f,  0.40f, -0.06f, -0.10f },
    { W_mangrove_swamp,0.36f,  0.58f,  0.18f,  0.62f,  0.05f },
    { W_mangrove_swamp,0.24f,  0.36f,  0.42f,  0.84f, -0.05f },
    { W_plains,        0.14f,  0.06f,  0.30f,  0.05f,  0.00f },
    { W_plains,       -0.02f, -0.12f,  0.46f, -0.14f,  0.18f },
    { W_plains,        0.32f,  0.28f,  0.14f,  0.30f, -0.18f }
  };

  float best_dist = 1e9f;
  uint8_t best = W_plains;
  for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
    ClimateTarget t = targets[i];
    // Keep snowy climates actually cold.
    if (t.biome == W_snowy_plains && climate.temperature > -0.22f) continue;
    // Keep deserts meaningfully warm.
    if (t.biome == W_desert && climate.temperature < 0.32f) continue;
    // Mangrove/swamp proxy should stay warm/humid/eroded lowlands.
    if (t.biome == W_mangrove_swamp && (climate.humidity < 0.22f || climate.erosion < 0.22f)) continue;
    // Avoid swamp/desert in high inland continental zones.
    if (climate.continentalness > 0.55f && t.biome != W_plains && t.biome != W_snowy_plains) continue;
    float d = climateDistanceSq(climate, t);
    if (d < best_dist) {
      best_dist = d;
      best = t.biome;
    }
  }

  // Narrow river/coastal ribbons (still deterministic by seed).
  float river_noise = sampleClimateAxis(
    div_floor(x * CHUNK_SIZE, 4),
    div_floor(z * CHUNK_SIZE, 4),
    48, 0xF13A5B9C6D7E8A01ULL
  );
  float river_band = river_noise < 0.0f ? -river_noise : river_noise;
  if (climate.continentalness > -0.05f && climate.continentalness < 0.28f && river_band < 0.035f) {
    return W_beach;
  }

  // Extra deterministic guards to avoid over-dominant plains in hot/cold climates.
  if (best == W_plains) {
    if (climate.temperature < -0.48f) return W_snowy_plains;
    if (climate.temperature > 0.62f && climate.humidity < 0.10f) return W_desert;
  }

  return best;
}

uint8_t getChunkBiome (short x, short z) {

  // Biome lookup is hot-path for chunk generation; keep a tiny fixed cache.
  uint32_t h = hashChunkXZ(x, z);
  int first_free = -1;
  for (int i = 0; i < BIOME_CACHE_CAPACITY; i ++) {
    int slot = (int)((h + (uint32_t)i) % BIOME_CACHE_CAPACITY);
    BiomeCacheEntry *entry = &biome_cache[slot];
    if (!entry->used) {
      if (first_free == -1) first_free = slot;
      break;
    }
    if (entry->x == x && entry->z == z) return entry->biome;
  }

  uint8_t biome = getBiomeFromClimateUncached(x, z);
  int slot = first_free;
  if (slot == -1) slot = (int)(h % BIOME_CACHE_CAPACITY);
  biome_cache[slot].used = true;
  biome_cache[slot].x = x;
  biome_cache[slot].z = z;
  biome_cache[slot].biome = biome;
  return biome;

}

uint8_t getCornerHeight (short anchor_x, short anchor_z, uint32_t hash, uint8_t biome) {
  (void)hash;

  // Vanilla-like macro signals in [-1..1]:
  // continentalness (landmass), erosion (roughness), ridges (mountain chains).
  float continental = valueNoise2D(anchor_x, anchor_z, WORLDGEN_CONTINENT_SCALE, 0x4E3F9C27D1B6508AULL) * 2.0f - 1.0f;
  float erosion = valueNoise2D(anchor_x, anchor_z, WORLDGEN_EROSION_SCALE, 0x8AF1C94372DE10B5ULL) * 2.0f - 1.0f;
  float ridge_src = valueNoise2D(anchor_x, anchor_z, WORLDGEN_RIDGE_SCALE, 0xB7D2186E9035AC41ULL) * 2.0f - 1.0f;
  float ridge = ridge_src;
  float ridge_abs = ridge < 0.0f ? -ridge : ridge;
  float ridge_folded = -3.0f * (-0.33333333f + (((ridge_abs - 0.66666667f) < 0.0f ? -(ridge_abs - 0.66666667f) : (ridge_abs - 0.66666667f))));
  if (ridge_folded < 0.0f) ridge_folded = 0.0f;
  if (ridge_folded > 1.0f) ridge_folded = 1.0f;

  // Secondary local relief so plains are not flat carpets.
  float rolling = fractalNoise2D(anchor_x, anchor_z, 0x11E96B3AA7E5B74DULL) - 0.5f;
  float hills = valueNoise2D(anchor_x, anchor_z, 10, 0x4C8A7D13F20B5E91ULL) - 0.5f;
  float cliff_noise = valueNoise2D(anchor_x, anchor_z, 6, 0x7E3B19AC40D25F91ULL) - 0.5f;
  float peak_noise = valueNoise2D(anchor_x, anchor_z, 28, 0x5F91D2A34C7B18E6ULL);
  float chain_axis = valueNoise2D(anchor_x, anchor_z, 56, 0x3DA9F40B6E2187C5ULL) * 2.0f - 1.0f;
  float chain_presence = valueNoise2D(anchor_x, anchor_z, 104, 0x1F7C8B24D65EA903ULL);
  float river_primary = valueNoise2D(anchor_x, anchor_z, 36, 0xF13A5B9C6D7E8A01ULL) * 2.0f - 1.0f;
  float river_secondary = valueNoise2D(anchor_x, anchor_z, 14, 0x29CE4AB1D70685F3ULL) * 2.0f - 1.0f;

  // Broad valleys for high erosion and low-mid continentalness.
  float valley_mask = 0.0f;
  float valley_continent_max = (float)WORLDGEN_VALLEY_CONTINENT_MAX / 100.0f * 2.0f - 1.0f;
  float valley_erosion_min = (float)WORLDGEN_VALLEY_EROSION_MIN / 100.0f * 2.0f - 1.0f;
  if (continental < valley_continent_max && erosion > valley_erosion_min) {
    float c = (valley_continent_max - continental) / (valley_continent_max + 1.0f);
    float e = (erosion - valley_erosion_min) / (1.0f - valley_erosion_min);
    valley_mask = c * e;
    if (valley_mask > 1.0f) valley_mask = 1.0f;
    valley_mask *= valley_mask;
  }

  float mountain_t = 0.0f;
  float mountain_continent_min = (float)WORLDGEN_MOUNTAIN_CONTINENT_MIN / 100.0f * 2.0f - 1.0f;
  float mountain_erosion_max = (float)WORLDGEN_MOUNTAIN_EROSION_MAX / 100.0f * 2.0f - 1.0f;
  if (continental > mountain_continent_min && erosion < (mountain_erosion_max + 0.12f)) {
    float c = (continental - mountain_continent_min) / (1.0f - mountain_continent_min);
    float e = ((mountain_erosion_max + 0.12f) - erosion) / ((mountain_erosion_max + 0.12f) + 1.0f);
    float r = ridge_folded;
    mountain_t = c * e * r;
    if (mountain_t > 1.0f) mountain_t = 1.0f;
    mountain_t *= mountain_t;
  }

  float biome_base = 0.0f;
  float biome_shape_scale = 1.0f;
  switch (biome) {
    case W_mangrove_swamp:
      biome_base = -3.0f;
      biome_shape_scale = 0.6f;
      break;
    case W_desert:
      biome_base = 1.0f;
      biome_shape_scale = 0.85f;
      break;
    case W_snowy_plains:
      biome_base = 4.0f;
      biome_shape_scale = 1.15f;
      break;
    case W_beach:
      return 62;
    case W_plains:
    default:
      biome_base = 0.0f;
      biome_shape_scale = 1.0f;
      break;
  }

  // Piecewise continental baseline, inspired by Overworld continental bands:
  // deep ocean < -0.55, ocean/coast until ~-0.15, inland beyond.
  float height_f;
  if (continental < -0.55f) {
    height_f = 49.0f + (continental + 1.0f) * 8.0f;
  } else if (continental < -0.15f) {
    height_f = 58.0f + (continental + 0.55f) * 15.0f;
  } else {
    height_f = 64.0f + (continental + 0.15f) * 28.0f;
  }
  height_f += biome_base;

  // Erosion flattens terrain for high values and sharpens for low values.
  float erosion_shape = 0.0f - erosion;
  height_f += erosion_shape * 5.0f;
  height_f += rolling * (float)WORLDGEN_ROLLING_AMPLITUDE * biome_shape_scale;
  height_f += hills * (float)WORLDGEN_HILL_AMPLITUDE * biome_shape_scale;
  height_f -= valley_mask * (float)WORLDGEN_VALLEY_DEPTH;

  if (mountain_t > 0.0f) {
    // Mountains stay rare and chunk-spanning.
    float mountain_gain = (0.35f + ridge_folded * 0.65f) * mountain_t * (float)WORLDGEN_MOUNTAIN_AMPLITUDE;
    if (biome == W_snowy_plains) mountain_gain *= 1.15f;
    if (biome == W_mangrove_swamp) mountain_gain *= 0.45f;
    height_f += mountain_gain;
  }

  // Long mountain chains: form ridges along coherent noise axes.
  float chain_axis_abs = chain_axis < 0.0f ? -chain_axis : chain_axis;
  float chain_line = 1.0f - chain_axis_abs;
  float chain_t = (chain_line - 0.62f) / 0.38f;
  if (chain_t < 0.0f) chain_t = 0.0f;
  if (chain_t > 1.0f) chain_t = 1.0f;
  float chain_presence_t = (chain_presence - 0.45f) / 0.55f;
  if (chain_presence_t < 0.0f) chain_presence_t = 0.0f;
  if (chain_presence_t > 1.0f) chain_presence_t = 1.0f;
  if (continental > 0.06f && erosion < 0.34f) {
    float chain_gain = chain_t * chain_presence_t * (6.0f + 24.0f * ridge_folded);
    if (biome == W_snowy_plains) chain_gain *= 1.18f;
    if (biome == W_mangrove_swamp) chain_gain *= 0.50f;
    height_f += chain_gain;
  }

  // Rare high peaks in cold/highland contexts.
  if (continental > 0.35f && erosion < -0.20f && ridge_folded > 0.70f && peak_noise > 0.70f) {
    float peak_t = (peak_noise - 0.70f) / 0.30f;
    if (peak_t > 1.0f) peak_t = 1.0f;
    peak_t *= peak_t;
    float peak_gain = 10.0f + 22.0f * peak_t;
    if (biome == W_snowy_plains) peak_gain *= 1.2f;
    if (biome == W_mangrove_swamp) peak_gain *= 0.45f;
    height_f += peak_gain;
  }

  // Notchian-like weirdness strongly influences peak/valley extremes.
  float weird_abs = (ridge_src < 0.0f) ? -ridge_src : ridge_src;
  if (continental > 0.18f && erosion < 0.12f && weird_abs > 0.82f) {
    float weird_t = (weird_abs - 0.82f) / 0.18f;
    if (weird_t > 1.0f) weird_t = 1.0f;
    height_f += 8.0f + weird_t * 18.0f;
  }

  // Cliff sharpening around ridge zones for more dramatic transitions.
  if (ridge_folded > 0.62f && erosion < 0.15f) {
    float cliff_t = (ridge_folded - 0.62f) / 0.38f;
    if (cliff_t > 1.0f) cliff_t = 1.0f;
    float cliff_gain = (cliff_noise > 0.12f) ? (cliff_noise - 0.12f) * 20.0f * cliff_t : 0.0f;
    height_f += cliff_gain;
  }

  // River valleys: coherent channel mask + wider bank cut, not just a thin line.
  float river_abs0 = river_primary < 0.0f ? -river_primary : river_primary;
  float river_abs1 = river_secondary < 0.0f ? -river_secondary : river_secondary;
  float river_shape = river_abs0 * 0.72f + river_abs1 * 0.28f;
  if (continental > -0.10f && continental < 0.62f) {
    float river_core = (0.080f - river_shape) / 0.080f;
    if (river_core < 0.0f) river_core = 0.0f;
    if (river_core > 1.0f) river_core = 1.0f;
    float river_bank = (0.145f - river_shape) / 0.145f;
    if (river_bank < 0.0f) river_bank = 0.0f;
    if (river_bank > 1.0f) river_bank = 1.0f;
    float inland_t = (continental + 0.10f) / 0.72f;
    if (inland_t < 0.0f) inland_t = 0.0f;
    if (inland_t > 1.0f) inland_t = 1.0f;
    float carve_depth = river_core * (8.0f + 14.0f * inland_t);
    float bank_depth = river_bank * 3.0f;
    height_f -= carve_depth;
    height_f -= bank_depth;
  }

  if (height_f < 48.0f) height_f = 48.0f;
  float height_cap = (float)WORLDGEN_HEIGHT_CAP - 2.0f;
  if (height_f > height_cap) height_f = height_cap;
  return (uint8_t)(height_f + 0.5f);
}

uint8_t interpolate (uint8_t a, uint8_t b, uint8_t c, uint8_t d, int x, int z) {
  uint16_t top    = a * (CHUNK_SIZE - x) + b * x;
  uint16_t bottom = c * (CHUNK_SIZE - x) + d * x;
  return (top * (CHUNK_SIZE - z) + bottom * z) / (CHUNK_SIZE * CHUNK_SIZE);
}

// Calculates terrain height using a pointer to an array of anchors
// The pointer should point towards the minichunk containing the desired
// Coordinates, with available neighbors on +X and +Z.
uint8_t getHeightAtFromAnchors (int rx, int rz, ChunkAnchor *anchor_ptr) {

  if (rx == 0 && rz == 0) {
    int height = getCornerHeight(anchor_ptr[0].x, anchor_ptr[0].z, anchor_ptr[0].hash, anchor_ptr[0].biome);
    if (height > 67) return height - 1;
  }
  return interpolate(
    getCornerHeight(anchor_ptr[0].x, anchor_ptr[0].z, anchor_ptr[0].hash, anchor_ptr[0].biome),
    getCornerHeight(anchor_ptr[1].x, anchor_ptr[1].z, anchor_ptr[1].hash, anchor_ptr[1].biome),
    getCornerHeight(
      anchor_ptr[16 / CHUNK_SIZE + 1].x,
      anchor_ptr[16 / CHUNK_SIZE + 1].z,
      anchor_ptr[16 / CHUNK_SIZE + 1].hash,
      anchor_ptr[16 / CHUNK_SIZE + 1].biome
    ),
    getCornerHeight(
      anchor_ptr[16 / CHUNK_SIZE + 2].x,
      anchor_ptr[16 / CHUNK_SIZE + 2].z,
      anchor_ptr[16 / CHUNK_SIZE + 2].hash,
      anchor_ptr[16 / CHUNK_SIZE + 2].biome
    ),
    rx, rz
  );

}

uint8_t getHeightAtFromHash (int rx, int rz, int _x, int _z, uint32_t chunk_hash, uint8_t biome) {

  if (rx == 0 && rz == 0) {
    int height = getCornerHeight(_x, _z, chunk_hash, biome);
    if (height > 67) return height - 1;
  }
  return interpolate(
    getCornerHeight(_x, _z, chunk_hash, biome),
    getCornerHeight(_x + 1, _z, getChunkHash(_x + 1, _z), getChunkBiome(_x + 1, _z)),
    getCornerHeight(_x, _z + 1, getChunkHash(_x, _z + 1), getChunkBiome(_x, _z + 1)),
    getCornerHeight(_x + 1, _z + 1, getChunkHash(_x + 1, _z + 1), getChunkBiome(_x + 1, _z + 1)),
    rx, rz
  );

}

// Get terrain height at the given coordinates
// Does *not* account for block changes
uint8_t getHeightAt (int x, int z) {

  int _x = div_floor(x, CHUNK_SIZE);
  int _z = div_floor(z, CHUNK_SIZE);
  int rx = mod_abs(x, CHUNK_SIZE);
  int rz = mod_abs(z, CHUNK_SIZE);
  uint32_t chunk_hash = getChunkHash(_x, _z);
  uint8_t biome = getChunkBiome(_x, _z);

  return getHeightAtFromHash(rx, rz, _x, _z, chunk_hash, biome);

}

uint8_t getTerrainAtFromCache (int x, int y, int z, int rx, int rz, ChunkAnchor anchor, ChunkFeature feature, uint8_t height) {
  uint8_t variant = (anchor.hash >> 20) & 3;
  float river_mask = getRiverChannelMask(x, z);
  uint8_t slope = 0;

  // Structure pass first so ruined portal blocks can intentionally override
  // base terrain/decor on the target columns.
  uint8_t structure_block = 0xFF;
  if (tryRuinedPortalBlock(x, y, z, anchor.biome, &structure_block)) {
    return structure_block;
  }

  // Surface lava pools are a rare top-layer feature; apply before regular
  // surface composition so lava/netherrack can replace dirt/grass cleanly.
  if (y >= (int)height - 1 && y <= (int)height) {
    uint8_t lava_pool_block = getSurfaceLavaPoolBlock(x, y, z, height, anchor.biome);
    if (lava_pool_block != 0xFF) return lava_pool_block;
  }

  // Allow occasional mountain cave mouths that are visible from surface.
  if (y <= (int)height && y >= (int)height - 3) {
    if (isSurfaceCaveEntranceAt(x, y, z, height, anchor.biome)) return B_air;
  }

  if (y >= 64 && y >= height && feature.y != 255) switch (anchor.biome) {
    case W_plains:
    case W_mangrove_swamp: {
      // Biome-aware tree pass with deterministic silhouette and leaf mix.
      if (feature.y < 64) break;

      // Tree feature must only affect columns near the tree center.
      // Without this guard, a selected feature can suppress normal surface
      // decoration across the whole minichunk and create visual artifacts.
      uint8_t dx = x > feature.x ? x - feature.x : feature.x - x;
      uint8_t dz = z > feature.z ? z - feature.z : feature.z - z;
      if (dx > 2 || dz > 2) break;

      if (anchor.biome == W_mangrove_swamp) {
        if (x == feature.x && z == feature.z && y == 64 && height < 63) return B_lily_pad;
        if (y == height + 1) {
          uint8_t mdx = x > feature.x ? x - feature.x : feature.x - x;
          uint8_t mdz = z > feature.z ? z - feature.z : feature.z - z;
          if (mdx + mdz < 4) return B_moss_carpet;
        }
      }

      uint8_t tree_type = feature.variant & 3;
      uint8_t tall = (feature.variant >> 2) & 1;
      uint8_t crown = (feature.variant >> 3) & 1;
      uint8_t trunk_h = (uint8_t)(4 + tall + ((tree_type == 1) ? 1 : 0));
      uint8_t base_block = (anchor.biome == W_mangrove_swamp) ? B_mud : B_dirt;

      uint8_t leaf_primary = B_oak_leaves;
      uint8_t leaf_secondary = B_oak_leaves;
      if (tree_type == 1) {
        leaf_primary = B_azalea_leaves;
        leaf_secondary = B_flowering_azalea_leaves;
      } else if (tree_type == 2) {
        leaf_primary = B_flowering_azalea_leaves;
        leaf_secondary = B_azalea_leaves;
      }

      if (x == feature.x && z == feature.z) {
        if (y == feature.y - 1) return base_block;
        if (y >= feature.y && y < feature.y + trunk_h) return B_oak_log;
      }
      int rel = y - ((int)feature.y + (int)trunk_h - 3);

      if (rel == 0 || rel == 1) {
        if (dx <= 2 && dz <= 2) {
          if ((dx == 2 && dz == 2) && (((feature.x + feature.z + y) & 1) == 0)) break;
          return (tree_type == 2) ? leaf_secondary : leaf_primary;
        }
      }
      if (rel == 2 && dx <= 1 && dz <= 1) return leaf_primary;
      if (rel == 3 && crown && dx == 0 && dz == 0) return leaf_secondary;

      if (y == height) return getSurfaceBlockForBiome(anchor.biome, variant, height);
      return B_air;
    }

    case W_desert: { // Generate dead bushes and cacti in deserts

      if (x != feature.x || z != feature.z) break;

      if (feature.variant == 0) {
        if (y == height + 1) return B_dead_bush;
      } else if (y > height) {
        // The size of the cactus is determined based on whether the terrain
        // Height is even or odd at the target location
        if (height & 1 && y <= height + 3) {
          if (y == height + 3 && (((x ^ z) & 255) < WORLDGEN_DESERT_CACTUS_FLOWER_CHANCE)) return B_cactus_flower;
          return B_cactus;
        }
        if (y <= height + 2) return B_cactus;
      }

      break;

    }

    default: break;
  }

  // Handle surface-level terrain (the very topmost blocks)
  if (height >= 63) {
    if (y == height) {
      // Vanilla-like surface-rule approximation:
      // rivers get gravel/sand beds, steep zones expose stone.
      if (river_mask > 0.74f && anchor.biome != W_desert && anchor.biome != W_beach && height >= 58 && height <= 82) {
        return B_gravel;
      }
      slope = getLocalSlopeAt(x, z);
      if (slope >= 7 && height >= 76 && anchor.biome != W_mangrove_swamp) {
        return B_stone;
      }
      return getSurfaceBlockForBiome(anchor.biome, variant, height);
    }
    if (y == height + 1 && height >= 64) {
      // Fill only coherent low-altitude river beds.
      if (shouldPlaceRiverSurfaceWater(x, z, height, anchor.biome, river_mask)) {
        return B_water;
      }

      if (isWaterfallSpringCandidate(x, z, height, anchor.biome)) return B_water;

      // Surface decorator pass: deterministic biome-specific patches/clusters.
      uint8_t deco = (uint8_t)((getCoordinateHash(x, 0, z) >> 9) & 255);
      uint8_t deco_hi = (uint8_t)((getCoordinateHash(x, 9, z) >> 11) & 255);
      uint8_t flowers_plain_chance = scaleChanceU8(
        WORLDGEN_PLAINS_FLOWER_CHANCE,
        WORLDGEN_DECOR_DENSITY_SCALE * WORLDGEN_FLOWER_DENSITY_SCALE
      );
      uint8_t mushrooms_plain_chance = scaleChanceU8(
        WORLDGEN_PLAINS_MUSHROOM_CHANCE,
        WORLDGEN_MUSHROOM_DENSITY_SCALE
      );
      uint8_t mushrooms_swamp_chance = scaleChanceU8(
        WORLDGEN_SWAMP_MUSHROOM_CHANCE,
        WORLDGEN_MUSHROOM_DENSITY_SCALE
      );
      uint8_t surface = getSurfaceBlockForBiome(anchor.biome, variant, height);
      if (anchor.biome == W_plains) {
        if (surface == B_grass_block) {
          // Vanilla-like pumpkin patches: rare, local clusters, not isolated noise.
          float pumpkin_patch = valueNoise2D(
            x, z, WORLDGEN_PUMPKIN_PATCH_SCALE, 0x36C492A5E17B4D09ULL
          );
          if (
            pumpkin_patch > ((float)WORLDGEN_PUMPKIN_PATCH_THRESHOLD / 100.0f) &&
            deco < WORLDGEN_PLAINS_PUMPKIN_CHANCE
          ) {
            return B_pumpkin;
          }

          // Flowers spawn in patch regions, with local per-column randomness.
          float flower_patch = valueNoise2D(
            x, z, WORLDGEN_FLOWER_PATCH_SCALE, 0x91BD3EF0762CA845ULL
          );
          if (
            flower_patch > ((float)WORLDGEN_FLOWER_PATCH_THRESHOLD / 100.0f) &&
            deco < flowers_plain_chance
          ) return getFlowerBlockFromHash(getCoordinateHash(x, 1, z), W_plains);

          if (deco < mushrooms_plain_chance) {
            return ((getCoordinateHash(x, 5, z) & 1) == 0) ? B_brown_mushroom : B_red_mushroom;
          }

          if (deco < scaleChanceU8(WORLDGEN_PLAINS_GRASS_CHANCE, WORLDGEN_DECOR_DENSITY_SCALE)) {
            if (deco_hi < 84) return B_fern;
            return B_short_grass;
          }
        }
      } else if (anchor.biome == W_desert) {
        if (deco < scaleChanceU8(WORLDGEN_DESERT_DEAD_BUSH_CHANCE, WORLDGEN_DECOR_DENSITY_SCALE)) return B_dead_bush;
      } else if (anchor.biome == W_mangrove_swamp) {
        if (deco < mushrooms_swamp_chance && y > 64) {
          return ((getCoordinateHash(x, 8, z) & 1) == 0) ? B_brown_mushroom : B_red_mushroom;
        }
        if (deco < scaleChanceU8((uint8_t)(WORLDGEN_SWAMP_GRASS_CHANCE / 2), WORLDGEN_DECOR_DENSITY_SCALE) && y > 64) return B_fern;
        if (deco < scaleChanceU8(WORLDGEN_SWAMP_GRASS_CHANCE, WORLDGEN_DECOR_DENSITY_SCALE) && y > 64) return B_short_grass;
      }
    }
    if (anchor.biome == W_snowy_plains && y == height + 1) {
      return B_snow;
    }
  }
  // Starting at 4 blocks below terrain level, generate minerals and caves
  if (y <= height - 4) {
    // Coherent cave field + aquifer/lava fluid fill.
    if (isCaveOpenAt(x, y, z, height)) {
      uint8_t fluid = getAquiferFluidAt(x, y, z);
      if (fluid != B_air) return fluid;
      return B_air;
    }

    // The chunk-relative X and Z coordinates are used as the seed for an
    // Xorshift RNG/hash function to generate the Y coordinate of the ore
    // In this column. This way, each column is guaranteed to have exactly
    // One ore candidate, as there will always be a Y value to reference.
    uint8_t ore_y = ((rx & 15) << 4) + (rz & 15);
    ore_y ^= ore_y << 4;
    ore_y ^= ore_y >> 5;
    ore_y ^= ore_y << 1;
    ore_y &= 63;

    if (y == ore_y) {
      // Since the ore Y coordinate is effectely a random number in range [0;64),
      // We use it in a bit shift with the chunk's anchor hash to get another
      // Pseudo-random number for the ore's rarity.
      uint8_t ore_probability = (anchor.hash >> (ore_y % 24)) & 255;
      // Ore placement is determined by Y level and "probability"
      if (y < 15) {
        if (ore_probability < 10) return B_diamond_ore;
        if (ore_probability < 12) return B_gold_ore;
        if (ore_probability < 15) return B_redstone_ore;
      }
      if (y < 30) {
        if (ore_probability < 3) return B_gold_ore;
        if (ore_probability < 8) return B_redstone_ore;
      }
      if (y < 54) {
        if (ore_probability < 30) return B_iron_ore;
        if (ore_probability < 40) return B_copper_ore;
      }
      if (ore_probability < 60) return B_coal_ore;
      if (y < 5) return B_lava;
      return B_cobblestone;
    }

    // For everything else, fall back to stone
    return B_stone;
  }
  // Handle the space between stone and grass
  if (y <= height) {
    if (anchor.biome == W_desert) return B_sandstone;
    if (anchor.biome == W_mangrove_swamp) return B_mud;
    if (anchor.biome == W_beach && height > 64) return B_sandstone;
    return B_dirt;
  }
  // If all else failed, but we're below sea level, generate water (or ice)
  if (y == 63 && anchor.biome == W_snowy_plains) return B_ice;
  if (y < 64) return B_water;

  // For everything else, fall back to air
  return B_air;

}

ChunkFeature getFeatureFromAnchor (ChunkAnchor anchor) {

  ChunkFeature feature;
  uint8_t feature_position = anchor.hash % (CHUNK_SIZE * CHUNK_SIZE);

  feature.x = feature_position % CHUNK_SIZE;
  feature.z = feature_position / CHUNK_SIZE;
  uint8_t skip_feature = false;

  // Keep tree crowns mostly within minichunk bounds.
  int margin = WORLDGEN_TREE_EDGE_MARGIN;
  if (margin < 0) margin = 0;
  if (margin >= CHUNK_SIZE) margin = CHUNK_SIZE - 1;
  if (feature.x < margin || feature.x > CHUNK_SIZE - 1 - margin) skip_feature = true;
  else if (feature.z < margin || feature.z > CHUNK_SIZE - 1 - margin) skip_feature = true;

  if (skip_feature) {
    // Skipped features are indicated by a Y coordinate of 0xFF (255)
    feature.y = 0xFF;
  } else {
    feature.x += anchor.x * CHUNK_SIZE;
    feature.z += anchor.z * CHUNK_SIZE;
    feature.y = getHeightAtFromHash(
      mod_abs(feature.x, CHUNK_SIZE), mod_abs(feature.z, CHUNK_SIZE),
      anchor.x, anchor.z, anchor.hash, anchor.biome
    ) + 1;

    // Tree placement rules: biome-specific chance plus grove clustering.
    uint8_t top = getSurfaceBlockForBiome(anchor.biome, (anchor.hash >> 20) & 3, (uint8_t)(feature.y - 1));
    if (
      top != B_grass_block &&
      top != B_snowy_grass_block &&
      top != B_dirt &&
      top != B_mud
    ) {
      feature.y = 0xFF;
      return feature;
    }

    int tree_chance = 0;
    float tree_patch = valueNoise2D(
      anchor.x, anchor.z, WORLDGEN_TREE_PATCH_SCALE, 0xAF43D2895B1EC704ULL
    );
    float grove = tree_patch - 0.45f;
    if (grove < 0.0f) grove = 0.0f;
    grove *= 2.0f;
    if (grove > 1.0f) grove = 1.0f;
    grove *= grove;

    if (anchor.biome == W_plains) {
      tree_chance = scaleChanceU8(WORLDGEN_PLAINS_TREE_BASE_CHANCE, WORLDGEN_TREE_DENSITY_SCALE)
        + (int)(grove * scaleChanceU8(WORLDGEN_PLAINS_TREE_PATCH_BONUS, WORLDGEN_TREE_DENSITY_SCALE));
    } else if (anchor.biome == W_mangrove_swamp) {
      tree_chance = scaleChanceU8(WORLDGEN_SWAMP_TREE_BASE_CHANCE, WORLDGEN_TREE_DENSITY_SCALE)
        + (int)(grove * scaleChanceU8(WORLDGEN_SWAMP_TREE_PATCH_BONUS, WORLDGEN_TREE_DENSITY_SCALE));
    } else {
      feature.y = 0xFF;
      return feature;
    }
    if (tree_chance > 255) tree_chance = 255;

    uint8_t roll = (uint8_t)((anchor.hash >> 24) & 255);
    if (roll >= (uint8_t)tree_chance) {
      feature.y = 0xFF;
      return feature;
    }

    // Tree variant packs type/height/crown bits for lightweight silhouettes:
    // bits 0..1 type, bit 2 tall, bit 3 top crown.
    uint8_t shape_bits = (uint8_t)((anchor.hash >> ((feature.x + feature.z) & 15)) & 0x0F);
    if (anchor.biome == W_mangrove_swamp) shape_bits = (shape_bits & 0x0C) | 2;
    feature.variant = shape_bits;
  }

  return feature;

}

uint8_t getTerrainAt (int x, int y, int z, ChunkAnchor anchor) {

  if (y > WORLDGEN_HEIGHT_CAP) return B_air;

  int rx = x % CHUNK_SIZE;
  int rz = z % CHUNK_SIZE;
  if (rx < 0) rx += CHUNK_SIZE;
  if (rz < 0) rz += CHUNK_SIZE;

  ChunkFeature feature = getFeatureFromAnchor(anchor);
  uint8_t height = getHeightAtFromHash(rx, rz, anchor.x, anchor.z, anchor.hash, anchor.biome);

  return getTerrainAtFromCache(x, y, z, rx, rz, anchor, feature, height);

}

static uint8_t getNetherTerrainAt (int x, int y, int z) {

  if (y < 0) return B_bedrock;
  if (y == 0) return B_bedrock;
  if (y >= 127) return B_bedrock;

  uint32_t hash = getCoordinateHash(x, y, z);
  uint8_t floor_height = 26 + ((hash >> 3) & 11);
  uint8_t roof_height = 102 + ((hash >> 7) & 18);

  // Keep a large lava sea in the lower part of the nether zone.
  if (y <= 30 && y < floor_height) return B_lava;

  // Cave density is intentionally high to keep movement viable.
  uint8_t cave_noise = (hash >> ((x ^ z) & 15)) & 31;
  uint8_t is_cave = (cave_noise < 11) && y > floor_height && y < roof_height;

  if (!is_cave) {
    if ((hash & 255) < 6 && y < 110 && y > 10) return B_gold_ore;
    if ((hash >> 8 & 255) < 10 && y < 120 && y > 8) return B_coal_ore;
    return B_netherrack;
  }

  if (y < 30) return B_lava;
  return B_air;

}

uint8_t getBlockAt (int x, int y, int z) {

  if (y < 0) return B_bedrock;

  uint8_t block_change = getBlockChange(x, y, z);
  if (block_change != 0xFF) return block_change;

  if (isNetherZone(z)) return getNetherTerrainAt(x, y, z);

  short anchor_x = div_floor(x, CHUNK_SIZE);
  short anchor_z = div_floor(z, CHUNK_SIZE);
  ChunkAnchor anchor = {
    .x = anchor_x,
    .z = anchor_z,
    .hash = getChunkHash(anchor_x, anchor_z),
    .biome = getChunkBiome(anchor_x, anchor_z)
  };

  return getTerrainAt(x, y, z, anchor);

}

uint8_t chunk_section[4096];
ChunkAnchor chunk_anchors[(16 / CHUNK_SIZE + 1) * (16 / CHUNK_SIZE + 1)];
ChunkFeature chunk_features[256 / (CHUNK_SIZE * CHUNK_SIZE)];
uint8_t chunk_section_height[16][16];

// Builds a 16x16x16 chunk of blocks and writes it to `chunk_section`
// Returns the biome at the origin corner of the chunk
uint8_t buildChunkSection (int cx, int cy, int cz) {

  if (isNetherZone(cz)) {
    for (int j = 0; j < 4096; j += 8) {
      int y = j / 256 + cy;
      int rz = j / 16 % 16;
      for (int offset = 7; offset >= 0; offset--) {
        int k = j + offset;
        int rx = k % 16;
        chunk_section[j + 7 - offset] = getNetherTerrainAt(rx + cx, y, rz + cz);
      }
    }
    return W_desert;
  }

  // Precompute hashes, anchors and features for each relevant minichunk
  int anchor_index = 0, feature_index = 0;
  for (int i = cz; i < cz + 16 + CHUNK_SIZE; i += CHUNK_SIZE) {
    for (int j = cx; j < cx + 16 + CHUNK_SIZE; j += CHUNK_SIZE) {

      ChunkAnchor *anchor = chunk_anchors + anchor_index;

      anchor->x = j / CHUNK_SIZE;
      anchor->z = i / CHUNK_SIZE;
      anchor->hash = getChunkHash(anchor->x, anchor->z);
      anchor->biome = getChunkBiome(anchor->x, anchor->z);

      // Compute chunk features for the minichunks within this section
      if (i != cz + 16 && j != cx + 16) {
        chunk_features[feature_index] = getFeatureFromAnchor(*anchor);
        feature_index ++;
      }

      anchor_index ++;
    }
  }

  // Precompute terrain height for entire chunk section
  for (int i = 0; i < 16; i ++) {
    for (int j = 0; j < 16; j ++) {
      anchor_index = (j / CHUNK_SIZE) + (i / CHUNK_SIZE) * (16 / CHUNK_SIZE + 1);
      ChunkAnchor *anchor_ptr = chunk_anchors + anchor_index;
      chunk_section_height[j][i] = getHeightAtFromAnchors(j % CHUNK_SIZE, i % CHUNK_SIZE, anchor_ptr);
    }
  }

  // Generate 4096 blocks in one buffer to reduce overhead
  for (int j = 0; j < 4096; j += 8) {
    // These values don't change in the lower array,
    // Since all of the operations are on multiples of 8
    int y = j / 256 + cy;
    int rz = j / 16 % 16;
    int rz_mod = rz % CHUNK_SIZE;
    feature_index = (j % 16) / CHUNK_SIZE + (j / 16 % 16) / CHUNK_SIZE * (16 / CHUNK_SIZE);
    anchor_index = (j % 16) / CHUNK_SIZE + (j / 16 % 16) / CHUNK_SIZE * (16 / CHUNK_SIZE + 1);
    // The client expects "big-endian longs", which in our
    // Case means reversing the order in which we store/send
    // Each 8 block sequence.
    for (int offset = 7; offset >= 0; offset--) {
      int k = j + offset;
      int rx = k % 16;
      // Combine all of the cached data to retrieve the block
      chunk_section[j + 7 - offset] = getTerrainAtFromCache(
        rx + cx, y, rz + cz,
        rx % CHUNK_SIZE, rz_mod,
        chunk_anchors[anchor_index],
        chunk_features[feature_index],
        chunk_section_height[rx][rz]
      );
    }
  }

  // Apply block changes on top of terrain
  // This does mean that we're generating some terrain only to replace it,
  // But it's better to apply changes in one run rather than in individual
  // Runs per block, as this is more expensive than terrain generation.
  short chunk_x = div_floor(cx, 16);
  short chunk_z = div_floor(cz, 16);
  for (int i = firstBlockChangeInChunk(chunk_x, chunk_z); i != -1; i = nextIndexedBlockChange(i)) {
    if (div_floor(block_changes[i].x, 16) != chunk_x) continue;
    if (div_floor(block_changes[i].z, 16) != chunk_z) continue;
    // Skip blocks that behave better when sent using a block update
    if (block_changes[i].block == B_torch) continue;
    #ifdef ALLOW_CHESTS
      if (block_changes[i].block == B_chest) continue;
    #endif
    if (block_changes[i].y >= cy && block_changes[i].y < cy + 16) {
      int dx = block_changes[i].x - cx;
      int dy = block_changes[i].y - cy;
      int dz = block_changes[i].z - cz;
      // Same 8-block sequence reversal as before, this time 10x dirtier
      // Because we're working with specific indexes.
      unsigned address = (unsigned)(dx + (dz << 4) + (dy << 8));
      unsigned index = (address & ~7u) | (7u - (address & 7u));
      chunk_section[index] = block_changes[i].block;
    }
  }

  return chunk_anchors[0].biome;

}
