# Settlements

A settlement is a cluster of cells, not a point: seven for a capital, three for a town, one
for a village (`TownTier::Capital`, `Town`, `Village`). The town pass is
[`passes/pass_towns.h`](../coopa/maps/passes/pass_towns.h); its keys are in the `towns:`
block of [`assets/config.yaml`](../assets/config.yaml).

Each `MapTown` carries a name in its region's dialect, a tier, a population counted from its
buildings, its `buildings`, its `streets`, and an optional `plaza`.

## Streets

The roads and rivers bordering each claimed cell become lanes running from the cell's site
out to those edges. A cell with neither gets fallback lanes toward its farthest corners.
Buildings are placed in pairs along each lane, and the remaining budget fills the interior.

The streets are stored as `MapTown::streets`, drawn on the structures layer and written to
the map file, so a consumer can lay them out too.

## Alignment

Interior buildings take the bearing of the nearest street rather than a random rotation.
Measured, that raised the share of buildings fronting a street from 51% to 83%. Positions
stay jittered, so the layout never becomes a grid.

## Clearance

Buildings are kept off the roadway. Every candidate footprint is tested against the
settlement's streets and against the `MapRoad` polylines through its cells, each at half its
width plus `street_clearance_m`. The test uses the whole rotated footprint and the same
separating-axis routine two buildings use. Before this, 37% of buildings stood on a lane and
18% on a road; both are now zero.

`building_corners()` in [`map_data.h`](../coopa/maps/map_data.h) gives a footprint's four
rotated corners. Containment and non-overlap are guaranteed against those corners.

## Squares and civic roles

A settlement with at least `plaza_min_cells` cells gets a `MapPlaza` (a market square) at
its primary site, trimmed to fit the cell, and kept clear of buildings. The buildings
nearest it take civic roles from one roster shared by every tier: well, hall, market,
smithy, temple, inn, granary, barracks, warehouse. A town is a prefix of a capital. Every
other building is a `Dwelling`. `BuildingRole` names are append-only, because they are the
on-disk identity of every saved building.

| Key | Shipped | Effect |
|---|---|---|
| `plaza_radius_m` | 26 | Radius of the market square, trimmed to fit the cell |
| `plaza_min_cells` | 2 | Cells needed before a settlement gets one |
| `capital_civic_count` / `town_civic_count` / `village_civic_count` | 7 / 4 / 2 | How far down the roster each tier goes |
| `street_width_m` | 4 | Width a street is drawn at |
| `street_clearance_m` | 1.5 | Clear ground between a building and any roadway, added to half its width |
