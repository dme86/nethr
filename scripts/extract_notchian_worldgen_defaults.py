#!/usr/bin/env python3
import json
import math
import os
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DATA_ROOT = ROOT / "notchian" / "generated" / "data" / "minecraft" / "worldgen"
GLOBALS_H = ROOT / "include" / "globals.h"
OUT_H = ROOT / "include" / "worldgen_notchian_defaults.h"


def read_json(path: Path):
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def get_chunk_size() -> int:
    text = GLOBALS_H.read_text(encoding="utf-8")
    m = re.search(r"#define\s+CHUNK_SIZE\s+(\d+)", text)
    return int(m.group(1)) if m else 8


def expected_attempts_from_count(count_obj):
    if isinstance(count_obj, int):
        return float(count_obj)
    if isinstance(count_obj, float):
        return float(count_obj)
    if isinstance(count_obj, dict):
        t = count_obj.get("type", "")
        if t == "minecraft:weighted_list":
            dist = count_obj.get("distribution", [])
            total_w = 0.0
            total_v = 0.0
            for item in dist:
                w = float(item.get("weight", 0))
                v = float(item.get("data", 0))
                total_w += w
                total_v += v * w
            return (total_v / total_w) if total_w > 0 else 0.0
    return 1.0


def expected_attempts_from_placed_feature(name: str) -> float:
    path = DATA_ROOT / "placed_feature" / f"{name}.json"
    if not path.exists():
        return 0.0
    data = read_json(path)
    attempts = 1.0
    for mod in data.get("placement", []):
        t = mod.get("type", "")
        if t == "minecraft:count":
            attempts *= expected_attempts_from_count(mod.get("count", 1))
        elif t == "minecraft:noise_threshold_count":
            above = float(mod.get("above_noise", 0))
            below = float(mod.get("below_noise", 0))
            # Coarse average without simulating the full noise distribution.
            attempts *= (above + below) / 2.0
        elif t == "minecraft:rarity_filter":
            chance = float(mod.get("chance", 1))
            if chance > 0:
                attempts *= 1.0 / chance
    return attempts


def to_chance_byte_per_column(attempts_per_chunk: float) -> int:
    p = attempts_per_chunk / 256.0
    v = int(round(p * 255.0))
    if attempts_per_chunk > 0.0 and v < 1:
        v = 1
    return max(0, min(255, v))


def to_chance_byte_per_minichunk(attempts_per_chunk: float, chunk_size: int) -> int:
    features_per_chunk = max(1, (16 * 16) // (chunk_size * chunk_size))
    p = attempts_per_chunk / float(features_per_chunk)
    v = int(round(p * 255.0))
    if attempts_per_chunk > 0.0 and v < 1:
        v = 1
    return max(0, min(255, v))


def load_biome_features(biome_name: str):
    data = read_json(DATA_ROOT / "biome" / f"{biome_name}.json")
    out = set()
    for step in data.get("features", []):
        for entry in step:
            if isinstance(entry, str) and entry.startswith("minecraft:"):
                out.add(entry.split(":", 1)[1])
    return out


def pick_feature(biome_features, candidates):
    for c in candidates:
        if c in biome_features:
            return c
    return None


def main():
    if not DATA_ROOT.exists():
        raise SystemExit(f"Missing worldgen data dir: {DATA_ROOT}")

    chunk_size = get_chunk_size()

    plains = load_biome_features("plains")
    snowy = load_biome_features("snowy_plains")
    swamp = load_biome_features("mangrove_swamp")
    desert = load_biome_features("desert")

    f_trees_plains = pick_feature(plains, ["trees_plains", "trees_flower_forest"]) or "trees_plains"
    f_trees_snowy = pick_feature(snowy, ["trees_snowy"]) or "trees_snowy"
    f_trees_swamp = pick_feature(swamp, ["trees_mangrove", "trees_swamp"]) or "trees_mangrove"

    f_grass_plains = pick_feature(plains, ["patch_grass_plain", "patch_grass_normal"]) or "patch_grass_plain"
    f_grass_snowy = pick_feature(snowy, ["patch_grass_badlands", "patch_grass_normal"]) or "patch_grass_badlands"
    f_grass_swamp = pick_feature(swamp, ["patch_grass_normal", "patch_grass_plain"]) or "patch_grass_normal"

    f_flowers_plains = pick_feature(plains, ["flower_plains", "flower_default"]) or "flower_plains"
    f_flowers_snowy = pick_feature(snowy, ["flower_default", "flower_plains"]) or "flower_default"

    f_pumpkin_plains = pick_feature(plains, ["patch_pumpkin"]) or "patch_pumpkin"

    f_brown = "brown_mushroom_normal"
    f_red = "red_mushroom_normal"

    f_dead_bush_desert = pick_feature(desert, ["patch_dead_bush_2", "patch_dead_bush"]) or "patch_dead_bush_2"
    f_cactus_desert = pick_feature(desert, ["patch_cactus_desert", "patch_cactus_decorated"]) or "patch_cactus_desert"

    a_trees_plains = expected_attempts_from_placed_feature(f_trees_plains)
    a_trees_snowy = expected_attempts_from_placed_feature(f_trees_snowy)
    a_trees_swamp = expected_attempts_from_placed_feature(f_trees_swamp)

    a_grass_plains = expected_attempts_from_placed_feature(f_grass_plains)
    a_grass_snowy = expected_attempts_from_placed_feature(f_grass_snowy)
    a_grass_swamp = expected_attempts_from_placed_feature(f_grass_swamp)

    a_flowers_plains = expected_attempts_from_placed_feature(f_flowers_plains)
    a_flowers_snowy = expected_attempts_from_placed_feature(f_flowers_snowy)

    a_pumpkin = expected_attempts_from_placed_feature(f_pumpkin_plains)
    a_mushrooms = expected_attempts_from_placed_feature(f_brown) + expected_attempts_from_placed_feature(f_red)

    a_dead_bush = expected_attempts_from_placed_feature(f_dead_bush_desert)
    a_cactus = expected_attempts_from_placed_feature(f_cactus_desert)

    noise_overworld = read_json(DATA_ROOT / "noise_settings" / "overworld.json")
    noise = noise_overworld.get("noise", {})
    min_y = int(noise.get("min_y", -64))
    height = int(noise.get("height", 384))
    top_y = min_y + height - 1
    # Current engine stores height in uint8_t; clamp until vertical rewrite lands.
    worldgen_height_cap = max(96, min(255, top_y))

    macros = {
        "WORLDGEN_HEIGHT_CAP": worldgen_height_cap,
        "WORLDGEN_PLAINS_TREE_BASE_CHANCE": to_chance_byte_per_minichunk(a_trees_plains, chunk_size),
        "WORLDGEN_SNOWY_TREE_BASE_CHANCE": to_chance_byte_per_minichunk(a_trees_snowy, chunk_size),
        "WORLDGEN_SWAMP_TREE_BASE_CHANCE": to_chance_byte_per_minichunk(a_trees_swamp, chunk_size),
        "WORLDGEN_SWAMP_TREE_PATCH_BONUS": min(255, to_chance_byte_per_minichunk(a_trees_swamp, chunk_size) // 2),
        "WORLDGEN_PLAINS_TREE_PATCH_BONUS": min(255, to_chance_byte_per_minichunk(a_trees_plains, chunk_size) // 2),
        "WORLDGEN_PLAINS_GRASS_CHANCE": to_chance_byte_per_column(a_grass_plains),
        "WORLDGEN_SNOWY_GRASS_CHANCE": to_chance_byte_per_column(a_grass_snowy),
        "WORLDGEN_SWAMP_GRASS_CHANCE": to_chance_byte_per_column(a_grass_swamp),
        "WORLDGEN_PLAINS_FLOWER_CHANCE": to_chance_byte_per_column(a_flowers_plains),
        "WORLDGEN_PLAINS_PUMPKIN_CHANCE": to_chance_byte_per_column(a_pumpkin),
        "WORLDGEN_PLAINS_MUSHROOM_CHANCE": to_chance_byte_per_column(a_mushrooms),
        "WORLDGEN_SNOWY_MUSHROOM_CHANCE": to_chance_byte_per_column(a_mushrooms),
        "WORLDGEN_SWAMP_MUSHROOM_CHANCE": to_chance_byte_per_column(a_mushrooms * 1.5),
        "WORLDGEN_DESERT_DEAD_BUSH_CHANCE": to_chance_byte_per_column(a_dead_bush),
        "WORLDGEN_DESERT_CACTUS_FLOWER_CHANCE": max(1, min(255, to_chance_byte_per_column(a_cactus) * 16)),
    }

    lines = []
    lines.append("#ifndef H_WORLDGEN_NOTCHIAN_DEFAULTS")
    lines.append("#define H_WORLDGEN_NOTCHIAN_DEFAULTS")
    lines.append("")
    lines.append("// Auto-generated from Notchian worldgen JSON data.")
    lines.append("// Source: notchian/generated/data/minecraft/worldgen")
    lines.append("// Regenerate with: make worldgen-sync-defaults")
    lines.append(f"// Derived from: noise_settings/overworld (min_y={min_y}, height={height}, top_y={top_y})")
    lines.append("")
    for k in sorted(macros.keys()):
      lines.append(f"#define {k} {macros[k]}")
    lines.append("")
    lines.append("#endif")
    lines.append("")

    OUT_H.write_text("\n".join(lines), encoding="utf-8")

    print(f"Wrote {OUT_H}")
    print("Derived attempts/chunk:")
    print(f"  {f_trees_plains}: {a_trees_plains:.4f}")
    print(f"  {f_trees_snowy}: {a_trees_snowy:.4f}")
    print(f"  {f_trees_swamp}: {a_trees_swamp:.4f}")
    print(f"  {f_grass_plains}: {a_grass_plains:.4f}")
    print(f"  {f_flowers_plains}: {a_flowers_plains:.4f}")
    print(f"  {f_pumpkin_plains}: {a_pumpkin:.6f}")
    print(f"  mushrooms total: {a_mushrooms:.6f}")


if __name__ == "__main__":
    main()
