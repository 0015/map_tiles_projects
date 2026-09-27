# Map Tiles Projects — Example Repository

Complete, buildable ESP-IDF projects for the **0015__map_tiles** component
([ESP Registry](https://components.espressif.com/components/0015/map_tiles)) —
offline maps on ESP32 over LVGL 9.x, with tiles read from an SD card and no
network, cloud or API key involved.

The repository is split by which generation of the component API a project uses.

## v1 and v2

v1 and v2 are two APIs in **one component**, not two versions of it. v2.0.0
*added* the navigation layer and changed nothing in the old one: every v1
project in this repository builds untouched against v2.0.0, and the major
version number marks the new surface, not a break.

| | **[v2](v2)** — offline navigation | **[v1](v1)** — map display |
|---|---|---|
| Headers | `map_view.h`, `map_route.h`, `map_tile_cache.h`, `map_geo.h` | `map_tiles.h` |
| Needs | `0015/map_tiles` **≥ 2.0.0** | `0015/map_tiles` ≥ 1.0 |
| The map is | a single widget that draws every tile itself | a grid of `lv_image` widgets, one per tile |
| Centred on | a floating-point world-pixel coordinate | an integer tile index |
| Panning | continuous, with fling momentum | 256 px jumps when the grid re-anchors |
| Tile loading | per tile, LRU cached, on a background task | the whole grid, synchronously, on the UI thread |
| Where the position comes from | a GPS fix, snapped onto a route | you place a marker |
| Routes | `route.bin` from the card: geometry, turns, distance, ETA | — |
| Leaving the route | detected, with the distance and direction back | — |
| Projects here | 1 | 2 |

**Pick v2** when something is moving and the map has to keep up with it.
**Pick v1** when you want a map to look at: a viewer, a tracker, a position
plotted on a screen.

Using v1 today costs nothing when v2 exists — the two share the component, the
tile format and the SD card layout, so a v1 project can adopt the navigation
layer later without re-downloading anything.

## Repository structure

```
map_tiles_projects/
├── v1/                                             # map display — map_tiles.h
│   ├── 01.Simple_Map/                              # interactive map viewer, 3 boards
│   │   ├── espressif_esp32_p4_function_ev_board/
│   │   ├── waveshare_esp32_p4_wifi6_touch_lcd_xc/
│   │   ├── waveshare_esp32_s3_touch_amoled_1_75/
│   │   └── shared_components/
│   └── 02.ESP32-S3_Map_LoRa_GPS/                   # GPS tracking over LoRa
└── v2/                                             # navigation — map_tiles v2.0.0
    └── 01.Navigation_ESP32-S3-Touch-AMOLED-1.75/   # 466×466 round, 1.75 inch, battery
```

---

# v2 — Offline navigation

Turn-by-turn navigation with no network and no API key: a route drawn on the
map, a marker that follows you along it, the next turn with its distance and
street name, and honest handling of what happens when you leave the route.
Everything comes off the SD card.

Built on the navigation layer added in `map_tiles` **v2.0.0**, which replaces
the fixed tile grid with a continuously panning map view, an LRU tile cache and
a background loader — so the map keeps up with a moving vehicle instead of
stalling at every tile boundary.

<div align="center">

<img src="misc/01.Navigation_ESP32-S3-Touch-AMOLED-1.75.jpeg" width="720" alt="Turn-by-turn navigation running on the ESP32-S3 round AMOLED">

</div>

A route simulator is built in, so the whole guidance path — including leaving
the route — can be exercised at a desk without a GPS fix.

**[Read the v2 navigation guide →](v2/)**

### v2/01.Navigation_ESP32-S3-Touch-AMOLED-1.75

A watch-sized board: 466×466 round QSPI AMOLED, 1.75 inch, battery powered with
the charge on screen, a gyroscope compass, and controls that stay hidden until
you tap the map. GPS on UART2.

**[View project →](v2/01.Navigation_ESP32-S3-Touch-AMOLED-1.75/)**

### Where the tiles and routes come from

The v2 project reads tiles, and routes from `routes/`, off the card. Build both
with the navigation packager in
[OfflineMapDownloader](https://github.com/0015/OfflineMapDownloader) — click
waypoints, plan, build, and unzip the result onto the card. It downloads only a
corridor around the route and converts the tiles to RGB565 on the way out.

The route is optional. Switch the packager to **Area · tiles only** for a
rectangle of map with no route, and the device opens it as a map on its own.

---

# v1 — Map display

Projects built on the original `map_tiles.h` API: a grid of tiles anchored to an
integer tile coordinate, panned by hand, with markers placed on top.

<div align="center">

[![Offline map display on ESP32](misc/demo.gif)](https://youtu.be/ldWUl3tLEAc)

</div>

### v1/01.Simple_Map

Interactive map display with touch controls, real-time GPS coordinate tracking,
zoom and manual coordinate entry. Three ESP32 development boards with different
display configurations, sharing one component.

<div align="center">

<img src="v1/01.Simple_Map/misc/espressif_esp32_p4_function_ev_board_demo.gif" width="480" alt="Panning and zooming an offline map on the ESP32-P4 Function EV board">

</div>

**[View project →](v1/01.Simple_Map/)** | **[Watch demo →](https://youtu.be/Kyjf24e-Poo)**

### v1/02.ESP32-S3_Map_LoRa_GPS

Map display joined to LoRa wireless communication and live GPS tracking on
ESP32-S3: two devices exchange positions over long range and plot each other on
a local map.

**[View project →](v1/02.ESP32-S3_Map_LoRa_GPS/)** | **[Watch demo →](https://youtu.be/KDNXQfUJcSY)**

---

## About the map_tiles component

[**0015__map_tiles**](https://github.com/0015/map_tiles) provides:

- Efficient map tile loading from SD card
- Standard tile geometry (256×256, RGB565 with a 12-byte LVGL v9 header)
- Integration with LVGL 9.x
- Memory-optimised tile management
- Configurable zoom levels and tile providers
- **v2.0.0:** smooth-panning map view, asynchronous tile cache, offline route
  rendering, map matching and turn instructions

## Requirements

- ESP-IDF 5.4 or later (the v2 project is verified on 5.5.4)
- LVGL 9.3 or later
- An ESP32 device with display support
- An SD card for map tile storage

## License

See [LICENSE](LICENSE) file for details.

## Contributing

More example projects will be added to demonstrate additional use cases and
features of the map tiles library.
