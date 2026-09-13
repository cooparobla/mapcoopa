# Maps Module (`coopa::maps`)

The `maps` module generates whole world maps from a seed: Voronoi cells carrying an
elevation, a climate and a biome, threaded with rivers that run downhill to the sea and a
road network routed between the places worth travelling between, graded by the traffic
each stretch carries; nations divided into provinces whose borders settle on
ridgelines and coasts; settlements placed where people would actually live, each with a
population counted from its buildings and a name in its region's own invented language;
and landmarks read off the terrain itself. It is a port of Amit Patel's *Polygonal Map Generation*
([Red Blob Games](https://www.redblobgames.com/maps/mapgen2/)), reworked to fit this
library.

The module is its own repository rather than part of a graphics package because none of
it depends on Vulkan, GLFW or any sibling repository beyond libcoopa — it produces
**data**. The two PNG renderers are a debugging convenience, deliberately software-only;
the deliverable is a `MapGraph`, and `map_yaml.h` writes one to disk so a game can load a
world it did not generate itself.

Everything is header-only and reproducible: a `MapConfig` plus its `seed` fully
determines the output, byte for byte, including the settlements and the wobble on every
coastline.

---

## Maps Module Architecture

```text
┌──────────────────────────────────────────────────────────────────────────────┐
│                          MAPS MODULE DATA FLOW                                │
└──────────────────────────────────────────────────────────────────────────────┘

   ┌──────────────┐
   │  MapConfig   │  grid size, seed, noise field, per-pass knobs, town rules
   └──────┬───────┘
          │
          ▼
   ┌──────────────────────────────────────────────┐
   │              MapGenerator                     │
   │                                               │
   │  jittered grid ──► Delaunator ──► Voronoi dual │
   └──────┬───────────────────────────────────────┘
          │ builds, then annotates in place
          ▼
   ┌───────────────────────────────────────────────┐        ┌───────────────────┐
   │                 MapGraph                       │        │      passes/       │
   │                                                │◄───────│                    │
   │  centers[]  Voronoi cells  (biome, elevation)  │        │  1 water           │
   │  corners[]  Voronoi verts  (rivers, downslope) │        │  2 coast           │
   │  edges[]    Delaunay + Voronoi edge, shared    │        │  3 elevation       │
   │  roads[]    routed runs, graded by traffic     │        │  4 temperature     │
   │  towns[]    settlements + packed buildings     │        │  5 rivers          │
   │  regions[]  provinces, countries[] nations     │        │  6 moisture        │
   │  landmarks[] notable places                    │        │  7 biomes          │
   │                                                │        │  8 roads           │
   │  all adjacency by index, never by pointer      │        │  9 regions         │
   └──────┬───────────────────────────┬────────────┘        │ 10 towns           │
          │                            │                     │ 11 landmarks       │
          ▼                            ▼                     │ 12 noisy edges     │
   ┌─────────────────┐        ┌─────────────────┐           └───────────────────┘
   │  map_renderer.h  │        │   map_yaml.h     │
   │  BiomeRenderer   │        │  save_map()      │
   │  ElevationRender │        │  load_map()      │
   └────────┬────────┘        └─────────────────┘
            ▼
   ┌─────────────────┐
   │ image_writer.h   │  ──►  .png
   └─────────────────┘
```

Cells, corners and edges refer to each other by `CenterId` / `CornerId` / `EdgeId`
indices into `MapGraph`'s three arrays. That is a deliberate departure from the original,
which stored `shared_ptr`s in both directions and therefore leaked the whole graph on
every generation. The cost is one invariant every pass must respect: a record's slot is
its own index, so a pass that needs ranked order sorts an index array, never the storage.

---

## File Breakdown

### [`map_config.h`](./map_config.h)
`MapConfig` (grid density, seed, thresholds, nine pass toggles), `NoiseConfig` (typed
against FastNoiseLite's own enums rather than the raw `int`s the original cast at the
point of use), `TownConfig`, and `BiomePalette`. The palette is a flat array indexed by
`Biome`, which replaces the twenty-branch string comparison the renderer used to run per
cell.

### [`biome.h`](./biome.h)
The `Biome` enum (33 entries), its stable `snake_case` serialisation names, and
`classify_biome()` — a Whittaker table in three dimensions, branching on temperature
first, then elevation, then moisture. Temperature comes first because latitude is the
strongest control on what grows: the same height and rainfall give taiga at the pole and
rain forest at the equator. Names are append-only; they are the on-disk identity of every
saved map.

### [`name_generator.h`](./name_generator.h)
`Language`, a synthetic phonology, and `generate_name()`. Each country draws its own
inventory of onsets, vowels and codas, so its settlements share a sound and its neighbours
do not; regions get a dialect of their country's. `language_for()` and `dialect_for()`
rebuild a language from an id, so any pass can name a place in the right voice without a
language being passed around or serialised.

### [`landmark.h`](./landmark.h)
`LandmarkKind`, its names, the descriptive noun each takes, and `landmark_suits_biome()` —
the gate that stops a volcano appearing on ice or an oasis outside a desert.

### [`region.h`](./region.h)
Convenience include for `MapRegion` and `MapCountry`, which are declared in `map_data.h`
alongside the graph that owns them.

### [`map_data.h`](./map_data.h)
`MapPoint`, `MapCenter`, `MapCorner`, `MapEdge`, `MapTown`, `MapBuilding`, `MapRegion`,
`MapCountry`, `MapLandmark` and the `MapGraph` that owns them, plus the geometry a
consumer needs to place them:
`building_corners()` gives a footprint's four rotated corners — the authoritative shape,
since containment and non-overlap are guaranteed against those rather than an
axis-aligned box — with `point_in_polygon()` and a separating-axis `buildings_overlap()`
alongside. Also `MapGraph::cell_outline()`, which assembles a cell's
polygon in winding order by walking its corners and chaining the edge paths between them
— necessary once edges are subdivided, because a wobbled outline is not convex and
cannot have its winding recovered by sorting points about their centroid.

### [`noise.h`](./noise.h)
`Noise`, a FastNoiseLite instance configured once at construction. The original rebuilt
the generator and reapplied all eight settings on every sample, and the water pass built
a fresh one per corner.

### [`map_generator.h`](./map_generator.h)
`MapGenerator` — lays the jittered point lattice and its boundary ring, triangulates with
`delaunator`, reads the Voronoi dual off the half-edges, flags the border band, then runs
the enabled passes in order. Exposes the finished graph through `graph()`.

### [`passes/`](./passes/)
The nine annotation stages, one header each. See [`passes/README.md`](./passes/README.md).

### [`image.h`](./image.h) and [`image_writer.h`](./image_writer.h)
`Image` (an 8-bit interleaved buffer, not a texture — mapcoopa carries no graphics
dependency) plus half-space triangle fill, convex polygon fan fill and Bresenham strokes.
Also `CoverageMask`, a one-bit stencil: an overlay whose per-pixel effect is subtraction
rather than replacement has to be marked first and applied once, or it compounds wherever
its strokes overlap.
`image_writer.h` wraps stb_image_write; see its header comment for why the implementation
is pulled in with `STB_IMAGE_WRITE_STATIC`.

### [`map_renderer.h`](./map_renderer.h)
`BiomeRenderer` (coloured terrain tinted by region, then rivers, then the road network —
every casing first and every fill after, so a junction is not nicked by whichever road was
drawn later — then bridge parapets, settlements and landmark markers; the colour and hex
legend is in the [repository README](../../README.md#legend)) and
`ElevationRenderer` (greyscale heightmap, with rivers dimming the terrain beneath them by
a fixed amount — stencilled, so a confluence is no darker than the reaches feeding it).
Both are debugging aids: a consumer wanting a smooth heightfield should sample
`MapGraph::elevation_at()` rather than re-derive it from a lossy 8-bit image.

### [`map_yaml.h`](./map_yaml.h)
`save_map()` / `load_map()`, and the `map_to_node()` / `map_from_node()` pair beneath
them. Writes the entire graph — every cell, corner, edge, settlement and adjacency list —
so the document is a save of generator state rather than a derived export. Built on
`coopa::collections::YAMLMap`, with the bulk arrays assembled as `fkyaml::node` sequences
directly. Float fields are lossy to six significant digits; see the header comment.

---

## Usage Example

```cpp
#include <coopa/debug/logger.h>
#include <coopa/maps/image_writer.h>
#include <coopa/maps/map_generator.h>
#include <coopa/maps/map_renderer.h>
#include <coopa/maps/map_yaml.h>

coopa::maps::MapConfig config;
config.grid_size = 80;
config.seed = 251;
config.noise_island.seed = config.seed;  // one seed reproduces the whole map
config.towns.town_count = 28;
config.regions.country_count = 5;

coopa::debug::Logger logger("mapgen");
coopa::maps::MapGenerator generator(config, logger);
generator.generate();

const coopa::maps::MapGraph& map = generator.graph();

// Read the map as data.
for (const coopa::maps::MapCenter& cell : map.centers) {
    if (cell.ocean || cell.water) {
        continue;
    }
    spawn_terrain(cell.point.x, cell.point.y,
                  coopa::maps::biome_name(cell.biome),
                  cell.elevation);
}
for (const coopa::maps::MapTown& town : map.towns) {
    spawn_settlement(town.name, town.point.x, town.point.y, town.tier,
                     town.population, town.buildings);
}
for (const coopa::maps::MapCountry& country : map.countries) {
    register_nation(country.name, country.population, country.regions);
}
for (const coopa::maps::MapLandmark& landmark : map.landmarks) {
    spawn_landmark(landmark.name, landmark.kind, landmark.point.x, landmark.point.y);
}

// Persist it, and render a preview.
coopa::maps::save_map(map, config, "world.yaml");
coopa::maps::write_png("world_biomes.png",
                       coopa::maps::BiomeRenderer::render(map, config));

// Load it back in a later session.
coopa::maps::MapGraph loaded;
coopa::maps::MapConfig loaded_config;
coopa::maps::load_map("world.yaml", loaded, loaded_config);
```

A runnable version of the above ships as [`examples/map_generator.cpp`](../../examples/map_generator.cpp),
built as the `mapcoopa` target — the repo's bare project name, so `cplay` forwards its
arguments straight through to it:

```sh
cbuild                              # configure + build
cplay                               # random seed, writes ./map_out.*
cplay --seed=251 --out=/tmp/m       # reproducible
cplay --countries=8 --towns=40      # a busier world
cplay --help
```
