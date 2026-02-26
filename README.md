# mod-auto-gather

An AzerothCore module that automatically gathers mining nodes, herbs, and skinnable creatures as players walk near them. Loot goes directly into bags — no cast bars, no loot windows, no clicking.

## Features

- **Auto-Loot Gathering Nodes** — Mining veins and herb nodes within range are automatically harvested. The node despawns and follows its normal respawn cycle.
- **Auto-Skinning** — Dead creatures that have been fully looted and are flagged as skinnable are automatically skinned. Works with creatures that require Herbalism or Mining to skin as well.
- **Minimap Tracking** — Herb and mineral tracking are automatically enabled on the minimap for players with the corresponding skill. Both can be active simultaneously (bypasses the normal one-tracking-spell limit).
- **Skill Progression** — Gathering skill-ups happen normally, using the same formulas as the base game. Elite creatures give bonus skinning skill-ups.
- **Bag Space Safety** — All items are checked against available bag space before gathering. If bags are full, the node/corpse is left intact.
- **Quest Items & Gold** — Quest loot and gold from gathering nodes are collected automatically.

## Requirements

- AzerothCore with the latest source

## Installation

1. Clone into your `modules/` directory:
   ```bash
   cd modules
   git clone https://github.com/YOUR_USERNAME/mod-auto-gather.git
   ```
2. Re-run CMake and rebuild the server.
3. Copy `conf/mod_auto_gather.conf.dist` to your server's config directory as `mod_auto_gather.conf`.

## Configuration

All options are in `mod_auto_gather.conf`:

| Option | Default | Description |
|---|---|---|
| `AutoGather.Enable` | `1` | Enable/disable the module entirely |
| `AutoGather.Announce` | `1` | Show a chat message on login |
| `AutoGather.AutoTrack` | `1` | Auto-enable minimap tracking for gathering professions |
| `AutoGather.AutoLoot` | `1` | Auto-gather nodes and auto-skin creatures |
| `AutoGather.LootRange` | `10.0` | Range in yards to trigger auto-gathering |
| `AutoGather.ScanIntervalMs` | `1000` | Milliseconds between scans per player |
| `AutoGather.AllowInCombat` | `0` | Allow gathering while in combat |
| `AutoGather.AllowWhileMounted` | `1` | Allow gathering while mounted |

All settings are hot-reloadable (`.reload config`).

## How It Works

Each player is scanned on a configurable timer. On each scan, the module:

1. Checks player state (alive, not flying, not in a vehicle, not casting, not CC'd).
2. Sets minimap tracking flags if the player has Herbalism or Mining.
3. Searches nearby game objects for harvestable nodes matching the player's skill.
4. Searches nearby dead creatures for skinnable corpses matching the player's skill.
5. Gathers one node **or** skins one creature per cycle to keep the experience feeling natural.

Players must have the actual gathering profession trained — the module respects skill requirements exactly as the base game does.

## License

This module is released under the [GNU GPL v2](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
