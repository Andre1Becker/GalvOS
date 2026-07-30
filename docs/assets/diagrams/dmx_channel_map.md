<!-- Paste into docs/08-api-reference.md § DMX & Art-Net -->
<!-- Source of truth: include/config.h DmxChannel enum + DMX_CHANNEL_NAMES[25]; BPM default verified in src/bpm_clock.cpp (s_dmx_channel = 237) -->

| CH  | Name                | Range  | Notes                                                                 |
| --- | ------------------- | ------ | ---------------------------------------------------------------------- |
| 1   | Master Dimmer       | 0-255  | 0 = off, 255 = full; overridden by WebUI dimmer when `ui_override` active |
| 2   | Color Preset        | 0-255  | Selects built-in color palette entry                                   |
| 3   | Color Speed         | 0-255  | Color animation speed                                                  |
| 4   | Pattern Group       | 0-255  | 0=Geometry, 1=Waves, 2=3D, 3=Scenes, ...                                |
| 5   | Pattern Select      | 0-255  | Pattern index within group                                             |
| 6   | Effect Mode         | 0-255  | Dynamic effect (Rotation, Pulse, ...)                                  |
| 7   | Effect Speed        | 0-255  | Effect speed                                                           |
| 8   | Size                | 0-255  | Pattern scale; 255 = full scan range                                   |
| 9   | Auto-Scale          | 0-255  | 0 = off, >0 = auto-scale enabled                                       |
| 10  | Rotation            | 0-255  | Maps to 0-360deg                                                       |
| 11  | H-Flip              | 0-255  | 0 = normal, >=128 = flip                                               |
| 12  | V-Flip              | 0-255  | 0 = normal, >=128 = flip                                               |
| 13  | H-Position          | 0-255  | Horizontal offset                                                      |
| 14  | V-Position          | 0-255  | Vertical offset                                                        |
| 15  | Wave Amplitude      | 0-255  | Wave presets only                                                      |
| 16  | Wave Frequency      | 0-255  | Wave presets only                                                      |
| 17  | ILDA File           | 0-255  | 0=off, 1-40=file index, 255=last                                       |
| 18  | ILDA Speed          | 0-255  | Playback speed                                                         |
| 19  | ILDA Size           | 0-255  | 128 = original size                                                    |
| 20  | ILDA Loop           | 0-255  | 0=once, >=1=loop                                                       |
| 21  | ILDA Brightness     | 0-255  | 255 = follow master dimmer                                             |
| 22  | ILDA Frame Repeat   | 0-255  | 0=normal, >=1=slower                                                   |
| 23  | Color Anim Type     | 0-255  | 0=off, 1=gradient, 2=chase, 3=strobe, 4=pulse, 5=twinkle, 6=flip        |
| 24  | Color Anim Sequence | 0-9    | Palette sequence index                                                 |
| 25  | Color Anim Speed    | 0-255  | Animation speed                                                        |
| --  | BPM Clock           | 0-255  | Absolute CH 237 by default (configurable via `cfg-bpm-dmx-channel` / `bpm_clock::setDmxChannel()`); independent of the DMX start address — 0-255 maps to 20-300 BPM |

**Source arbitration among DMX-shaped inputs** (see [control_priority.mmd](control_priority.mmd)): WebUI Override / OSC > Art-Net > DMX-512 > sACN/E1.31 > Internal defaults, resolved per-frame in `readDmx()` (`src/patterns/pattern_engine.cpp`).
