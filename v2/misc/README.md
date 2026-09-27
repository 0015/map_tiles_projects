# v2 screenshots — what goes in this folder

The v2 README references these images from here. Until a file exists its tag is
an HTML comment rather than a broken thumbnail, so drop the file in and delete
the comment marker around the `<img>` line.

The board photo on the front page is not in here: it lives at
[`../../misc/01.Navigation_ESP32-S3-Touch-AMOLED-1.75.jpeg`](../../misc/01.Navigation_ESP32-S3-Touch-AMOLED-1.75.jpeg),
next to the v1 demo GIF.

| File | Referenced by | What to shoot |
|---|---|---|
| `nav_screen.jpg` | [`../README.md` → On screen](../README.md#on-screen) | The 466×466 panel straight on, mid-route: turn banner with a distance and a street name, blue road ahead, grey behind, summary panel at the bottom. It sits next to the ASCII layout diagram, so a head-on shot reads best. |
| `nav_off_route.jpg` | [`../README.md` → Leaving the route](../README.md#leaving-the-route) | The off-route state: red marker away from the road, the dashed tether running back to the route, the banner showing the gap and the direction. Press **⚠** in the demo transport bar to produce it without going anywhere. |

## A few practical notes

* **Round panels photograph badly on a bright desk.** The bezel picks up
  reflections that read as smudges on the map. Shoot in soft, indirect light,
  slightly off-axis, and crop to the circle.
* **Use demo mode.** `NAV_GPS_SOURCE_SIMULATOR` puts the device mid-route on
  demand, and the **⏭** button in the transport bar jumps to 250 m before the
  next turn — which is exactly the frame worth photographing. No driving
  required.
* **Keep them under about 600 KB each.** JPEG at quality 80 and no more than
  1600 px on the long edge is plenty for a README, and keeps the repository
  clone small. `sips -Z 1600 -s formatOptions 80 shot.jpg` does both on macOS.
* **Filenames are load-bearing.** The README references these exact names;
  rename a file and the image disappears.
