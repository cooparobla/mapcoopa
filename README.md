# mapcoopa

`mapcoopa` is a header-only C++ world generator: a seed and a `MapConfig` in, a whole
`MapGraph` out — Voronoi cells carrying an elevation, a climate and one of 33 biomes,
threaded with rivers that run downhill to the sea and roads that follow the contours;
nations divided into provinces whose borders settle on ridgelines and coasts; settlements
placed where people would actually live, each with a population counted from its buildings
and a name in its region's own invented language; and landmarks read off the terrain
itself. It is a port of Amit Patel's *Polygonal Map Generation*
([Red Blob Games](https://www.redblobgames.com/maps/mapgen2/)).

It produces **data**, not pixels: nothing here touches Vulkan, GLFW or any sibling repo
other than [`libcoopa`](../libcoopa), which it uses for exactly two headers
(`coopa/collections/yaml_map.h` and `coopa/debug/logger.h`). The two PNG renderers are a
software-only debugging convenience; the deliverable is the `MapGraph`, and
`coopa/maps/map_yaml.h` writes one to disk so a game can load a world it did not generate.

Everything is reproducible: a `MapConfig` plus its `seed` fully determines the output,
byte for byte, including the settlements and the wobble on every coastline.

Full module documentation, the data-flow diagram and a usage example live in
[**`coopa/maps/README.md`**](./coopa/maps/README.md); the twelve generation stages are
documented in [`coopa/maps/passes/README.md`](./coopa/maps/passes/README.md).

---

## Generating a map

`cbuild` configures and builds; `cplay` runs `./build/mapcoopa` — the generator — and
forwards its arguments verbatim.

```bash
cd libs/mapcoopa
cbuild
cplay --seed=42 --out=world
```

That writes `world_biomes.png`, `world_elevation.png` and `world.yaml` into the current
directory. With no `--seed` the generator draws one from system entropy and prints it, so
every run differs but stays reproducible afterwards.

`cbuild` wipes `build/` and reconfigures every time. For a fast iteration loop after the
first build:

```bash
cmake --build build && ./build/mapcoopa --out=world
```

### Options

| Flag | Default | Effect |
|---|---|---|
| `--seed=N` | random, printed | Master seed; reproduces a map exactly |
| `--grid-size=N` | 80 | Cells per axis. The island noise frequency scales with this, so a bigger map resolves more finely instead of fragmenting |
| `--image-size=N` | 2048 | Render size in pixels, square |
| `--rivers=N` | 55 | River sources to attempt |
| `--towns=N` | 28 | Settlements to place |
| `--countries=N` | 5 | Nations to carve out |
| `--regions=N` | 3 | Provinces per nation |
| `--no-regions` | — | Skip political geography entirely |
| `--no-landmarks` | — | Skip notable places |
| `--no-subdivide` | — | Straight cell boundaries instead of wobbled ones |
| `--out=PATH` | `map_out` | Output prefix |

At the default `--grid-size=80` the YAML lands around 15 MB.

## Running the tests

The suite is a separate target, since the bare project name belongs to the generator:

```bash
cbuild
./build/mapcoopa_tests      # or: ctest --test-dir build
```

30 cases covering determinism, the graph invariants, every pass, both renderers and the
YAML round trip.

## Consuming it from another repo

`add_subdirectory()` this repo and link `coopa::maps`; the standalone guard means you get
the interface target and nothing else — no extra binaries, no ctest entries.

```cmake
set(MAPCOOPA_DIR "${ROOT_DIR_PARENT}/mapcoopa")
if(NOT TARGET coopa::maps)
    add_subdirectory(${MAPCOOPA_DIR} ${CMAKE_CURRENT_BINARY_DIR}/mapcoopa-build)
endif()
target_link_libraries(your_target PRIVATE coopa::maps)
```

`coopa::maps` pulls in `coopa::lib` transitively, so glm and fkYAML come with it. The
delaunator, FastNoiseLite and stb_image_write headers this repo vendors under `includes/`
are on its interface include path too.
