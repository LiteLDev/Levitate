# Levitate behaviour spec

What Levitate reproduces from the Java mod [Create](https://modrinth.com/mod/create) **6.0.10** (simibubi and the Creators of Create).
This is the behaviour reference for every feature unit (see [AGENTS.md](../AGENTS.md)). It describes
behaviour only: never copy code or assets from Create.

**Goal: no compromise.** Every block, item, mechanic and screen of Create 6.0.10 exists in Levitate and
behaves the same, on the dedicated server (BDS) and on the client (local worlds). Only what Bedrock
really cannot do is listed in §8, each with the reason and the closest behaviour we ship instead.

**Who owns what.** Each section in §2–§7 names the unit that owns it (§9). The owner may correct and
extend its own sections in its PR (numbers, missing details); a value marked ⚠ is from memory and
**must** be checked by the owner against Create 6.0.10 (in-game behaviour and its default config
values, which are behaviour) before it is implemented. Never edit another unit's section.

## 1. Overview

### 1.1 Platforms

| | Dedicated server (BDS) | Client (LeviLauncher) |
|---|---|---|
| World logic | the mod on the server | the mod on the integrated server of local worlds |
| Rendering of moving parts | players with the client mod see it animated; vanilla players see static models (§1.4) | animated |
| Screens | native screens for players with the client mod; server forms for vanilla players | native screens |
| Resource pack | `PackDeployer` puts it on the world (required by default) | global resource pack stack |

The **client mod is optional** for joining a server: a vanilla Bedrock client gets every mechanic,
static models for rotating parts and form fallbacks for screens. With the client mod (detected by the
companion handshake, U3) a player gets animated kinetics, contraptions, curved tracks, native screens,
the goggles overlay, value boxes, Ponder and every other client-side feature.

### 1.2 Bedrock building blocks

- **Blocks**: data-driven blocks in the behavior pack (`assets/pack/bp/blocks/<id>.json`) with block
  states for everything that must survive a reload and be visible (facing, axis, powered, part, ...).
- **Block entity replacement**: custom blocks can't have block entities. Per-position data
  (inventories, tanks, kinetic speed, settings) lives in `BlockData` (U3), keyed by dimension +
  position, saved in the world DB per chunk, removed when the block is broken or replaced.
- **Kinetic network**: server-side graph (U2) recomputed incrementally on place/break/state change.
- **Moving structures** (contraptions, trains): server entities that carry a block snapshot (U4); the
  client mod renders their blocks; vanilla clients see a placeholder entity (§8).
- **Rendering**: the client mod draws rotating parts, belts, contraptions, curved tracks, fluids in
  tanks, overlays and screens natively. Every such block also has a static Bedrock geometry so that a
  vanilla client sees a correct (non-moving) block.
- **Recipes**: vanilla crafting/furnace/stonecutter recipes as BP recipe files; Create's own recipe
  types (crushing, milling, pressing, mixing, compacting, splashing, smoking via fan, blasting via
  fan, haunting, deploying, filling, emptying, cutting, sandpaper polishing, item application,
  sequenced assembly, mechanical crafting) as JSON data in `assets/recipes/<type>/<id>.json`, loaded by
  the owning unit (U3 provides the loader, §3.3).

### 1.3 Units and ticks

Create uses 20 TPS like Bedrock, so tick counts are the same. Speeds are **RPM**, stress is **SU**
(stress units): a component with impact *i* at speed *s* uses `i·|s|` SU; a source with capacity *c*
at its generated speed *g* provides `c·|g|` SU (Create semantics; config values are per RPM).

### 1.4 What a vanilla client sees

Static geometry of every block (shaft, cog, belt, etc.) in its current orientation; contraptions are
rendered as their blocks being placed/removed at the assembled position when they stop, and as a
placeholder entity while moving (§8); native screens become forms; overlays become action-bar text
(goggles) and chat (value boxes through forms).

## 2. Blocks and items

Ids are `levitate:<create path>` (Create's registry names). Block names come from RP `texts.d`.
Every block drops itself unless noted; loot tables in `loot_tables/blocks/<id>.json`. Recipes: the
Create 6.0.10 recipe for every item (crafting shape and every processing route), written as our own
data files.

### 2.1 Kinetic transmission (U6)

`shaft`, `cogwheel`, `large_cogwheel`, `gearbox`, `vertical_gearbox` (item only, places a gearbox),
`clutch`, `gearshift`, `encased_chain_drive`, `adjustable_chain_gearshift`, `andesite_encased_shaft`,
`brass_encased_shaft`, `andesite_encased_cogwheel`, `brass_encased_cogwheel`,
`andesite_encased_large_cogwheel`, `brass_encased_large_cogwheel`, `sequenced_gearshift`,
`rotation_speed_controller`, `speedometer`, `stressometer`, `belt` connector item is U11.
States: `axis` (x/y/z) for shafts and cogs, `facing` for gauges, `powered` for clutch/gearshift,
`part` for chain drives, `encased` variants are separate ids (as Create).

### 2.2 Generators (U7)

`hand_crank`, `copper_valve_handle` + `<colour>_valve_handle` (16), `water_wheel`, `large_water_wheel`
(+ structural `water_wheel_structure`), `creative_motor`. Windmill bearing and sails are U21; steam
engine is U18.

### 2.3 Tools and info (U8)

`wrench`, `goggles` (engineer's goggles, head slot).

### 2.4 Materials, food, world (U9)

Metals: `zinc_ore`, `deepslate_zinc_ore`, `raw_zinc`, `raw_zinc_block`, `zinc_ingot`, `zinc_nugget`,
`zinc_block`, `brass_ingot`, `brass_nugget`, `brass_block`, `brass_sheet`, `copper_sheet`, `iron_sheet`,
`golden_sheet`, `copper_nugget`, `andesite_alloy`, `andesite_alloy_block`, `industrial_iron_block`,
`weathered_iron_block`, `crushed_raw_<metal>` (iron, gold, copper, zinc, + compat metals listed by
Create: osmium, platinum, silver, tin, lead, quicksilver, aluminum, uranium, nickel), `powdered_obsidian`,
`cinder_flour`, `wheat_flour`, `dough`, `rose_quartz`, `polished_rose_quartz`, `rose_quartz_block`,
`rose_quartz_tiles`, `small_rose_quartz_tiles`, `electron_tube`, `precision_mechanism`,
`incomplete_precision_mechanism`, `propeller`, `whisk`, `brass_hand`, `crafter_slot_cover`,
`sturdy_sheet`, `unprocessed_obsidian_sheet`, `transmitter`, `pulp`, `cardboard`, `cardboard_block`,
`bound_cardboard_block`, `experience_nugget`, `experience_block`, `refined_radiance`, `shadow_steel`,
`chromatic_compound`, `bar_of_chocolate`, `sweet_roll`, `chocolate_glazed_berries`, `honeyed_apple`,
`builders_tea`, `blaze_cake_base`, `blaze_cake`, `creative_blaze_cake`, `dough`, `tree_fertilizer` is U31.
World generation: zinc ore veins (overworld, y −63..70 ⚠ Create 6 vein config), striated stone layers
(asurine, crimsite, ochrum, veridium, limestone, scorchia, scoria in their biomes/depths ⚠).

Decorative stone (U9): for each of `granite, diorite, andesite, calcite, dripstone, deepslate, tuff,
asurine, crimsite, limestone, ochrum, scoria, scorchia, veridium`: the raw stone (for Create's own
stones) and `cut_<s>`, `cut_<s>_stairs/_slab/_wall`, `polished_cut_<s>` (+ stairs/slab/wall),
`cut_<s>_bricks` (+ stairs/slab/wall), `small_<s>_bricks` (+ stairs/slab/wall), `layered_<s>`,
`<s>_pillar`. Stonecutter recipes as Create.

### 2.5 Building blocks (U10)

`andesite_casing`, `brass_casing`, `copper_casing`, `shadow_steel_casing`, `refined_radiance_casing`,
`railway_casing`; framed/tiled/horizontal/vertical framed glass + panes; windows for every wood type +
`ornate_iron_window` + panes; `copper_shingles`/`copper_tiles` (all oxidation + waxed stages, stairs,
slabs); `andesite_scaffolding`, `brass_scaffolding`, `copper_scaffolding`; `andesite_ladder`,
`brass_ladder`, `copper_ladder`; `andesite_bars`, `brass_bars`, `copper_bars`; doors `andesite_door`,
`brass_door`, `copper_door`, `train_door`, `framed_glass_door`, trapdoors `train_trapdoor`,
`framed_glass_trapdoor`; `metal_girder` (+ encased shaft variant), `metal_bracket`, `wooden_bracket`;
`<colour>_table_cloth` + `andesite/brass/copper_table_cloth` (deco; shop behaviour is U26);
`placard`; `<colour>_toolbox` is U31; `<colour>_seat` is U22; `<colour>_valve_handle` is U7;
`copycat_panel`, `copycat_step` (take the look of an applied block).

### 2.6 Belts and depots (U11)

`belt_connector` (item), `belt` (block, states `part` start/middle/end/pulley, `slope`, `facing`,
`casing`), `depot`, `weighted_ejector`.

### 2.7 Item logistics (U12)

`andesite_funnel`, `brass_funnel`, `andesite_tunnel`, `brass_tunnel`, `chute`, `smart_chute`,
`item_vault`, `creative_crate`.

### 2.8 Bulk processing (U13)

`millstone`, `crushing_wheel` (+ `crushing_wheel_controller`, invisible), `encased_fan`, `nozzle`.

### 2.9 Basin processing (U14)

`basin`, `mechanical_press`, `mechanical_mixer`, `blaze_burner` (states `blaze` none/smouldering/
fading/kindled/seething), `empty_blaze_burner`, `lit_blaze_burner`.

### 2.10 Assembly (U15)

`deployer`, `mechanical_arm`, sequenced assembly intermediates (`incomplete_*`).

### 2.11 Crafting tools (U16)

`mechanical_crafter`, `mechanical_saw`, `mechanical_drill`.

### 2.12 Fluid transport (U17)

`fluid_pipe`, `glass_fluid_pipe`, `encased_fluid_pipe`, `mechanical_pump`, `fluid_valve`, `smart_fluid_pipe`.

### 2.13 Tanks and steam (U18)

`fluid_tank`, `creative_fluid_tank`, `steam_engine`, `steam_whistle` (+ `steam_whistle_extension`), `powered_shaft` (internal).

### 2.14 Fluid machines (U19)

`spout`, `item_drain`, `hose_pulley`, `portable_fluid_interface`, fluids `honey`, `chocolate`,
`tea` (builder's tea), potion fluid (by potion type), `bucket` variants `honey_bucket`, `chocolate_bucket`.

### 2.15 Linear contraptions (U20)

`mechanical_piston`, `sticky_mechanical_piston`, `piston_extension_pole`, `mechanical_piston_head`,
`rope_pulley`, `rope`, `pulley_magnet`, `elevator_pulley`, `elevator_contact`, `gantry_carriage`,
`gantry_shaft`.

### 2.16 Rotating contraptions (U21)

`mechanical_bearing`, `windmill_bearing`, `clockwork_bearing`, `sail_frame`, `white_sail` +
`<colour>_sail`, `cart_assembler`, `minecart_coupling` (item), `controller_rail`, `turntable`.

### 2.17 Contraption support (U4)

`super_glue` (item), `linear_chassis`, `secondary_linear_chassis`, `radial_chassis`, `sticker`.

- `super_glue`: tool, stack 1, 99 uses (one use per glued region; creative uses none). Right-click a
  block: first corner ("Click again to confirm"); right-click the second corner: the box is glued if
  every side is at most `maxSuperGlueRange` (24) blocks ("Selected area is too big") and the corners
  are connected through solid blocks inside the box ("Selected blocks must be connected"); sneak +
  right-click discards ("Selection discarded"). Hitting a glued block while holding glue removes
  its regions. Players holding glue see the regions (particle outlines, §8).
- `linear_chassis` / `secondary_linear_chassis`: states `levitate:axis` 0..2 (x, y, z; from the clicked
  face when placed), `levitate:sticky_top` (positive axis end), `levitate:sticky_bottom`. Slime ball or
  super glue on a non-sticky end makes it sticky; on a sticky end makes both ends sticky; sneak +
  empty hand on a sticky end clears it (no item is used, as Create). Range 1..`maxChassisRange`,
  default `maxChassisRange / 2` = 8, shared by the connected chassis group (U3's value box; until then
  `/create contraption chassis <x y z> <range>`). The secondary chassis connects only to secondary
  chassis (shapeless crafting turns one into the other).
- `radial_chassis`: `levitate:axis`, sticky sides `levitate:sticky_north|south|east|west` (for axis y
  the world sides; axis x: north, south, up, down; axis z: up, down, east, west).
- `sticker`: states `levitate:facing` 0..5 (down, up, north, south, west, east; placed facing the
  player, sneaking: away), `levitate:extended` = powered by redstone (polled every 4 ticks). Extended,
  it holds the block in front on a contraption.
- Recipes (Create 6.0.10): super glue = andesite alloy, slime ball / iron sheet, andesite alloy;
  linear chassis ×3 = andesite alloy / log / andesite alloy; radial chassis ×3 = andesite alloy, log,
  andesite alloy; sticker = andesite alloy, slime ball, andesite alloy / cobblestone, redstone,
  cobblestone. They need U9's andesite alloy and iron sheet and are added with U9; the shapeless
  linear ↔ secondary conversions ship now.

### 2.18 Contraption actors (U22)

`mechanical_harvester`, `mechanical_plough`, `mechanical_roller`, `portable_storage_interface`,
`contraption_controls`, `<colour>_seat` (16).

### 2.19 Redstone (U23)

`redstone_link`, `analog_lever`, `pulse_repeater`, `pulse_extender`, `pulse_timer`, `powered_latch`,
`powered_toggle_latch`, `smart_observer`, `threshold_switch`, `redstone_contact`.

### 2.20 Display (U24)

`display_link`, `display_board`, `nixie_tube` + `<colour>_nixie_tube`, `clipboard`.

### 2.21 Packages (U25)

`packager`, `repackager`, `cardboard_package_*` (box styles), `package_frogport`, `chain_conveyor`,
`<wood>_postbox`, `package_filter`, `filter` + `attribute_filter` are U3.

### 2.22 Stock network (U26)

`stock_link`, `stock_ticker`, `factory_gauge`, `redstone_requester`, `blaze_burner` shopkeeper role on
`<colour>_table_cloth`, `shopping_list` (item).

### 2.23 Trains (U5 tracks, U27 trains, U28 signals)

U5: `track`, curved track (no separate id: stored in BlockData + graph), `track_station` is U27.
U27: `track_station`, `small_bogey`, `large_bogey`, `controls`, `schedule` (item), `railway_casing` is
U10, `train_door` is U10. U28: `track_signal`, `track_observer`.

### 2.24 Schematics (U29)

`schematic_table`, `schematicannon`, `empty_schematic`, `schematic`, `schematic_and_quill`,
`crafting_blueprint`.

### 2.25 Equipment (U31)

`<colour>_toolbox` (16), `copper_backtank`, `netherite_backtank`, `copper_diving_helmet`,
`copper_diving_boots`, `netherite_diving_helmet`, `netherite_diving_boots`, `extendo_grip`,
`potato_cannon`, `wand_of_symmetry`, `handheld_worldshaper`, `linked_controller`, `sand_paper`,
`red_sand_paper`, `tree_fertilizer`, `minecart_contraption` (item).

### 2.26 Filters (U3)

`filter` (list filter, 9 slots, respect NBT/data, whitelist/blacklist), `attribute_filter` (attribute
list, whitelist-any/whitelist-all/blacklist).

## 3. Mechanics

### 3.1 Kinetic network (U2)

- A **kinetic block** declares, per block state: rotation axis, which faces carry a shaft
  connection, whether it has small or large cog teeth (and on which axis), and its stress impact or
  capacity (config, §5). Units register blocks with `kinetics::registerBlock(id, spec)`.
- **Propagation**: from each source, speed spreads through shaft connections (same speed, same
  direction), small cog ↔ small cog on parallel axes adjacent sideways (same speed, opposite
  direction), large ↔ small diagonal mesh (ratio 2:1 / 1:2, opposite direction), large ↔ large
  ⚠ (same speed opposite direction), encased chain drives (same speed), adjustable chain gearshift
  (×2 when powered ⚠ analog), gearbox (90° transfer with Create's direction rules), gearshift
  (reverses when powered), clutch (disconnects when powered), speed controller (sets target speed).
- **Speed limits**: max 256 RPM (config `maxRotationSpeed`), speeds above are clamped; Create's
  speed levels: slow < 30 ≤ medium < 100 ≤ fast ⚠ (used by tooltips/goggles/particles).
- **Conflicts**: two sources forcing different speeds/directions into one network → the component
  where they meet breaks (pops as item) as in Create; a network that would exceed max speed through
  ratios breaks the offending cog.
- **Stress**: the network sums capacity of sources at their speed and impact of consumers at their
  speed; when impact > capacity the whole network is **overstressed**: every component stops
  (speed 0 for processing), gauges show it, goggles show "Overstressed".
- **Networks** recompute on place, break, state change (powered, axis) and chunk load; results are
  cached per network id and persisted (speed per block in BlockData) so a reload resumes.
- **Client**: each kinetic block's rotating part is drawn rotating at its speed (degrees/tick =
  speed·6/20 ⚠), interpolated per frame, offset by Create's per-block initial angle so meshing cogs
  line up. Speed changes are sent to clients with the client mod in batches per chunk.
- **Particles/sounds**: rotation sound per network (cogs ticking) and overstress smoke, as Create.

### 3.2 Block data, capabilities, filters, value boxes (U3)

- `BlockData`: JSON per (dim, pos) owned by the block's unit, `get/set/erase`, iterate per chunk,
  automatically erased when the block is broken, moved by contraptions (U4 carries it).
- **Item capability**: `items::register(blockId, Handler)` with `insert(stack, face, simulate)`,
  `extract(filter, amount, face, simulate)`, `slots()`. Vanilla chests, barrels, hoppers, furnaces,
  shulker boxes, dispensers/droppers, composters, jukeboxes etc. are adapted (sided as vanilla hoppers).
- **Fluid capability**: same shape with `FluidStack {fluidId, amountMb}`; 1 bucket = 1000 mB;
  vanilla cauldrons (water, lava, powder snow) adapted.
- **Heat capability**: `heat::level(pos)` → none / passive (fire, campfire, lava, magma) / kindled /
  seething, registered by U14's blaze burner.
- **Filters**: `filter::matches(filterItem, stack)` with Create's list and attribute semantics
  (attributes: in tag, mod id, is food, is fuel, is enchanted, has enchant, is damaged, fluid
  contained, stackable, equipable, in item group, can be smelted/smoked/washed/blasted/haunted/
  crushed/milled/sandpapered, written book author/title, shulker contents, ...). Filter slots on
  blocks: right-click the slot with an item sets it; the slot renders the item (client mod) or a
  floating item display (vanilla).
- **Value boxes** (scroll values): a block face may expose a numeric value (e.g. creative motor
  speed, threshold) or an option list. Client mod: hold-scroll with the wrench/empty hand while
  looking at the box, as Create (Shift for fine steps); vanilla: right-click with the wrench opens a
  form. U3 provides the registry and both UIs.
- **Recipe loader**: `recipes::loadType("pressing", parse)` reads `assets/recipes/<type>/*.json`
  (merged into the mod folder at build time) with Create's ingredient/result/chance/fluid semantics.

### 3.3 Client protocol and screens (U3)

- Handshake on join: the client mod announces itself (protocol version); the server marks the
  player as "companion". Typed packets for kinetic updates, block data sync, screen open/close,
  screen actions, value box changes, overlays.
- Native screen framework (client): Create-style panels (background texture drawn by our texture
  scripts, slots, buttons, scroll inputs, labels, tooltips). Every screen has a **form fallback**
  (server forms) for vanilla clients covering the same actions.

### 3.4 Contraptions (U4)

- **Assembly**: an anchor (bearing, piston, pulley, gantry, cart assembler, train station) asks the
  engine to collect blocks: attached blocks per Create's rules (chassis ranges and sticky faces,
  super glue regions, blocks that hang/attach like torches, doors, beds, banners), up to
  `maxBlocksMoved` 2048 ⚠ and `maxChassisRange` 16 ⚠. Unmovable blocks (bedrock, obsidian ⚠ per Create,
  portals, ...) stop assembly with the reason shown.
- **Moving**: the structure becomes a server entity carrying block states + BlockData; it moves
  (translation / rotation about an axis / following a cart or train) each tick; entities standing
  on it are carried; players collide with its blocks (server collision boxes); seats can be ridden.
- **Actors**: blocks on a contraption may act while moving (`contraption::registerActor`): drills
  break, saws cut, harvesters harvest, ploughs plough, rollers pave, deployers use, PSIs transfer,
  controls toggle actors.
- **Storage**: inventories and tanks on a contraption are merged into contraption storage, usable by
  actors and the PSI.
- **Disassembly**: placing back aligned to the grid (rounding as Create), blocks that can't be placed
  drop as items.
- **Super glue**: drag-select a box (two clicks) with super glue to glue a region (max 24 ⚠ blocks
  in each axis); glued regions render as faint outlines with the client mod; removed by hitting with
  glue / breaking.
- **Chassis**: linear (sticky faces via slime ball, range by scroll value), secondary linear, radial
  (range radius); stickers stick the block in front when powered.
- **Client**: the client mod renders contraption blocks at their interpolated transform, including
  rotating kinetic parts and actors' animations.

Implementation (U4, `src/feature/contraption/Contraption.h` is the contract):

- **API for anchors**: `assemble(AssembleOptions{anchor, start, forced, axis, pivot, kind,
  includeAnchor}) -> AssembleResult{id | errorKey, errorText}`; `find(id)` → `Contraption&` with
  `setTransform(t)` (anchors that compute their position each tick: bearings, pistons),
  `setMotion({velocity, angularVelocity, follow, ticks})` (velocity per tick, follow an actor),
  `stop()`, `storage()`, `anchorData()`, `seat()`; `disassemble(id)`; `all()`. Ids are persisted:
  keep the id in your BlockData, never the pointer. Transform: local block coordinates relative to
  the anchor; `world = origin + offset + pivot + R(local - pivot)`, R = rotation by `angle` degrees
  about `axis` (right-hand rule). `/create contraption assemble|move|rotate|stop|disassemble|list|
  demo` is the test anchor.
- **Assembly rules** (Create's `Contraption#moveBlock`, pure code in `src/core/contraption/Assembly`):
  BFS from the start positions; a neighbour joins when both are in one glue region, when it is
  attached towards the block (torches, ladders, signs, banners, buttons, levers, lanterns, bells,
  rails, carpets, redstone, pressure plates, doors, beds, extended stickers), when the block is slime
  or honey and both can stick (not brittle, not glazed terracotta, supportive, slime ≠ honey), or in
  the forced direction. Chassis add their connected chassis (same kind and axis; radial: along the
  axis) and their included positions (linear: up to the range along each sticky end, stopping at
  air, brittle or non-supportive blocks; radial: a disc of radius `range + 0.5` from each sticky
  side). Doors, beds and double chests move whole. Blocks are moved when they are not air, not
  replaceable and have a collision shape or a block entity (brittle blocks and fence gates always).
  Unmovable: unbreakable blocks (destroy time −1: bedrock, barrier, command/structure blocks, end
  portal frame…) and `unmovableBlocks` (portals, piston parts, moving blocks); obsidian is movable as in
  Create. Errors (world untouched): `create.gui.assembly.exception.unmovableBlock` (a forced
  neighbour or a start block), `.chunkNotLoaded`, `.structureTooLarge` (> `maxBlocksMoved`).
- **Carried data**: block NBT (`name` + states) and block entity NBT (chests keep their pair);
  `registerDataCarrier({name, capture, restore, rotate})` moves other units' per-block data (U3's
  BlockData plugs in here; chassis ranges use it). Merged storage: vanilla chests, trapped chests,
  barrels, shulker boxes, hoppers, dispensers and droppers (`Storage::insertItem/extractItem/slot`);
  tanks through `registerFluidStorage(blockId, {read, write})` on the carried data.
- **Movement each tick**: the anchor's transform or the motion; actors (`registerActor(blockId,
  MovementBehaviour{startMoving, tick, visitNewPosition, stopMoving, isActive})`, Create's
  MovementBehaviour; an actor may `stall` the contraption); entities standing on it are carried
  (the point under their feet follows the transform, rotation included); players and mobs are pushed
  out of its blocks (also while it stands still: the blocks are gone from the world, the contraption
  is the floor); seated entities (`registerSeat(blockId, height)`, entities in a seat block at assembly
  or `seat()`) follow their seat, players leave by sneaking. Unloaded anchor chunk: frozen.
- **Disassembly**: angle rounded to the nearest quarter turn, offset to whole blocks; block states
  rotated (`facing_direction`, `minecraft:facing_direction`, `minecraft:block_face`,
  `minecraft:cardinal_direction`, `pillar_axis`, `direction`, `weirdo_direction`,
  `ground_sign_direction`, `torch_facing_direction`, `rail_direction`, `minecraft:vertical_half` /
  `upside_down_bit` on half turns, `levitate:facing`, `levitate:axis`, chassis sticky sides;
  `registerRotationHandler` for others); solid blocks first, then brittle ones bottom-up; a block whose
  place is occupied drops as an item (with its container's contents) and the world block stays; glue
  regions are placed back transformed. One log line per assembly / disassembly / dropped block.
- **Display**: each block is a vanilla falling-block entity (never falls, never lands; orphans from a
  saved world vanish) at the block's rotated center showing the block rotated by the nearest quarter
  turn; its own rotation carries the axis (pitch) and the remaining angle (yaw), so the client mod
  turns it by the exact angle about its center. Persistence: `lv:contraption:c:<id>` (blocks,
  transform, motion, glue, seats, anchor data), `lv:contraption:glue`, `lv:contraption:chassis`,
  `lv:contraption:stickers`.

### 3.5 Tracks (U5)

- Placing track on another track's end with the track item in hand connects them with a curve
  (bezier between the two endpoints, constraints as Create: max turn angle, min radius, slopes,
  `maxTrackPlacementLength` 32 ⚠); cost in tracks taken from inventory (survival); girders under
  curves optional (holding girders in offhand ⚠).
- Track graph: nodes at track ends/junctions, edges straight or curved, persisted in WorldDb, keyed
  by dimension; nether portals link graphs across dimensions.
- Query API: path finding (A* on the graph), edge points, positions along edges, for trains and
  signals.

### 3.6 Feature mechanics

Each wave-2 unit implements the full Create 6.0.10 behaviour of its blocks (§2), including: what
right-click/shift-right-click/wrench do, redstone input and output, comparator output, interaction
with belts/funnels/arms/deployers/pipes where Create has it, processing recipes and timings, sounds and
particles, goggles info, and the client rendering. The unit's PR adds a §3.<unit> subsection here with
the details and numbers it implemented. Short reminders of the less obvious ones:

- **Belts (U11)**: 2–20 ⚠ blocks long, connect two shafts in a line or slope (45°), items move at
  speed proportional to RPM, belt carries entities, funnels/tunnels/depots/processing work on belt
  items, belt casing, dye, wrench to adjust, breaking returns belts.
- **Fans (U13)**: push/pull entities and items along up to `fanPushDistance` 20 ⚠ blocks; items in the
  air stream through water → washing, fire/campfire/lava → smoking or blasting, soul fire → haunting.
- **Blaze burner (U14)**: capture blazes with an empty burner, feed fuel (normal: kindled), blaze
  cake (seething), creative cake; heat levels drive basin recipes and boilers.
- **Steam (U18)**: fluid tank boiler size/water/heat give boiler level 0–18 ⚠; each level drives
  engines at Create's rpm/capacity; whistles pitch by size and height.
- **Packages (U25/U26)**: Create 6.0 logistics: packagers box items with addresses, frogports and
  chain conveyors route them, postboxes receive via trains, stock tickers request by address.
- **Trains (U27/U28)**: assembly at a station from bogeys + casing, controls drive manually,
  schedules automate (instructions/conditions as Create), signals split the graph into sections,
  trains brake for red signals, derailment/collision behaviour as Create.
- **Schematics (U29)**: Bedrock `.mcstructure` and Create `.nbt` schematics in `schematics/`;
  placement tool overlays the ghost (client mod), schematicannon prints with gunpowder and materials
  from attached inventories.
- **Ponder (U30)**: hold W over an item to open Ponder (client mod only), index of all scenes,
  animated scenes built from our blocks.

## 4. Screens

Each screen: native (client mod) + form fallback (vanilla). Owner unit in brackets.

| Screen | Opened by | Owner |
|---|---|---|
| Filter / attribute filter | right-click filter item | U3 |
| Value box form (vanilla) | wrench right-click on a value box | U3 |
| Sequenced gearshift instructions | right-click | U6 |
| Toolbox | right-click / radial select (client mod) | U31 |
| Linked controller binding | right-click | U31 |
| Wand of symmetry / worldshaper | right-click | U31 |
| Schematic table, schematicannon, schematic & quill naming | right-click | U29 |
| Clipboard | right-click | U24 |
| Display link configuration | right-click | U24 |
| Redstone requester / factory gauge / stock ticker / shopping list | right-click | U26 |
| Packager address, frogport address, postbox | right-click | U25 |
| Train schedule editor | right-click with schedule | U27 |
| Station (name, assemble/disassemble, door control) | right-click station | U27 |
| Crafting blueprint | right-click | U29 |
| Ponder | W over item (client mod) | U30 |
| Threshold switch / smart observer bounds | right-click | U23 |

## 5. Configuration

One section per unit (`config/<unit>.json`, `LV_REGISTER_CONFIG`). Keys mirror Create's server
config (CKinetics, CStress, CLogistics, CFluids, CTrains, CSchematics, CEquipment, CRecipes, CWorldGen)
with the same defaults; each unit lists its keys here in its PR. The stress table (impact /
capacity per RPM, Create defaults ⚠) belongs to `config/kinetics.json` (U2) so all units read one
place:

| Block | Impact | Capacity | Generated RPM |
|---|---|---|---|
| hand_crank / valve handles | | 8 | 32 |
| water_wheel | | 32 | 8 |
| large_water_wheel | | 128 | 4 |
| windmill_bearing | | 512 ⚠ (per sail-count rules) | sails / 8 ⚠ |
| steam_engine | | 1024 ⚠ | boiler driven |
| creative_motor | | 16384 | value box (−256..256) |
| mechanical_press | 8 | | |
| mechanical_mixer | 4 | | |
| millstone | 4 | | |
| crushing_wheel | 8 | | |
| encased_fan | 2 | | |
| mechanical_drill / saw | 4 | | |
| deployer | 4 | | |
| mechanical_arm | 2 | | |
| mechanical_crafter | 2 | | |
| mechanical_bearing / clockwork / piston / pulleys / gantry | 4 | | |
| turntable | 4 | | |
| belt / depot / gearbox / shaft / cogs | 0 | | |
| speed controller / sequenced gearshift / chain drives | 0 / 0 / 0 ⚠ | | |
| mechanical_pump | 4 | | |
| hose_pulley | 4 | | |
| chain_conveyor | 1 ⚠ | | |

`config/contraption.json` (U4): `maxBlocksMoved` 2048, `maxChassisRange` 16 (Create's CKinetics
keys and defaults), `maxSuperGlueRange` 24, `glueOutlines` true, `unmovableBlocks` (extra unmovable ids).

## 6. Commands

| Command | Who | Owner |
|---|---|---|
| `/create reload`, `/create selftest ["glob"]` | op | foundation |
| `/create kinetics <pos>` (dump network: speed, stress, sources) | op | U2 |
| `/create killtrain <name>`, `/create trains` | op | U27 |
| `/create glue clear [radius]` (Create's `/create clearglue`; default 16, around the sender) | op | U4 |
| `/create contraption assemble <x y z> [axis]` / `move <id> <dx dy dz> [ticks]` / `rotate <id> <axis> <degrees> [ticks]` / `stop <id>` / `disassemble <id>` / `list` / `demo` / `chassis <x y z> <range>` (test anchor) | op | U4 |
| `/create ponder <id>` (client mod) | anyone | U30 |
| `/create highlight <pos>` | op | U3 |
| `/create overlay reset` | anyone | U8 |

## 7. Strings

Create's English wording and key names (`create.*`) for every string that exists in Create, e.g.
`create.gui.goggles.kinetic_stats`, `create.gui.stressometer.overstressed`, `create.tooltip.stressImpact`.
Levitate-only strings use `lv.<unit>.*`. Each unit owns the keys of its features (en_US + zh_CN; zh_CN
matches Create's official Chinese translation wording).

## 8. Not ported

| Create feature | Why | What Levitate does instead |
|---|---|---|
| Animated parts / contraptions for **vanilla** clients | Bedrock has no client-side rendering hooks without a client mod | static block models; moving contraptions shown as their blocks only when stopped plus a placeholder entity while moving; the client mod shows everything |
| Placeable flowing honey/chocolate in the world | Bedrock has no data-driven custom fluids | they exist in pipes, tanks, basins, buckets (as items); emptying a bucket in the world places a custom non-flowing source block ⚠ U19 decides |
| JEI/EMI recipe viewers, Create's config GUI | no such mods on Bedrock | recipes visible in Ponder/tooltips; JSON configs + `/create reload` |
| Create's mod-compat crushed ores for absent mods | no such mods | items exist (as Create registers them), with the vanilla-only recipes |
| Contraption blocks drawn as one rotating model on **vanilla** clients (U4) | a vanilla client draws a falling-block entity axis-aligned and can't be told to turn it | each block is its own falling-block entity at its exact rotated position, showing the block turned by the nearest quarter turn; the client mod turns each block by the remaining angle so the structure looks exact |
| Contraption collision computed by the client (U4) | Bedrock clients only collide with real blocks and a few entity types | the server pushes players out of the contraption and carries them (teleports); with lag this is less smooth than Create's client-side collision |
| Glue region outlines (U4) | no client-side line rendering without a client renderer for it | particles along the edges of nearby regions for players holding super glue (client mod and vanilla) |
| Redstone input of the sticker (U4) | custom blocks get no redstone neighbour update | the sticker polls the redstone power of its neighbours every 4 ticks (redstone blocks, wire, torches, levers, buttons, repeaters, comparators, observers, anything with `redstone_signal`) |

Units add rows here in their PRs when they find something else impossible.

## 9. Work plan

Unit U<n> owns selftest `testPos` dx range `100n..100n+99`, selftest prefix `<unit>.*`, config
section `<unit>`, lang files `<unit>.json`, e2e scenarios `<unit>.ps1`, texture script `<unit>.ps1`.

### Wave 1 (contracts; all of wave 2 depends on them)

| Unit | Name | Owns | Depends on |
|---|---|---|---|
| U1 | spec | this file | — |
| U2 | `kinetics` | §3.1, `src/{feature,client,core}/kinetics/`, `config/kinetics.json` (stress table) | — |
| U3 | `common` | §3.2–3.3, `src/feature/common/`, `src/feature/net/`, `src/client/screen/`, filters (§2.26) | — |
| U4 | `contraption` | §3.4, §2.17, `src/{feature,client}/contraption/` | — |
| U5 | `tracks` | §3.5, `track`, `src/{feature,client,core}/tracks/` | — |

### Wave 2 (features; depend only on wave 1)

| Unit | Name | Owns |
|---|---|---|
| U6 | `transmission` | §2.1 |
| U7 | `generators` | §2.2 |
| U8 | `goggles` | §2.3, goggles overlay, kinetic tooltips |
| U9 | `materials` | §2.4, world gen, decorative stone |
| U10 | `building` | §2.5 |
| U11 | `belts` | §2.6 |
| U12 | `funnels` | §2.7 |
| U13 | `bulk` | §2.8 |
| U14 | `basin` | §2.9, heat sources |
| U15 | `assembly` | §2.10, sequenced assembly |
| U16 | `crafter` | §2.11 |
| U17 | `pipes` | §2.12 |
| U18 | `steam` | §2.13 |
| U19 | `fluids` | §2.14 |
| U20 | `pistons` | §2.15 |
| U21 | `bearings` | §2.16 |
| U22 | `actors` | §2.18 |
| U23 | `redstone` | §2.19 |
| U24 | `display` | §2.20 |
| U25 | `packages` | §2.21 |
| U26 | `stock` | §2.22 |
| U27 | `trains` | §2.23 (station, bogeys, controls, schedules) |
| U28 | `signals` | §2.23 (signals, observers) |
| U29 | `schematics` | §2.24 |
| U30 | `ponder` | Ponder (client) |
| U31 | `equipment` | §2.25 |

Cross-unit integration (e.g. a funnel on a belt, a spout over a depot, a deployer on a contraption)
goes through wave-1 contracts (capabilities, actors, BlockData); a unit never includes another
wave-2 unit's headers. When both sides exist, the integration works without either unit naming the
other.
