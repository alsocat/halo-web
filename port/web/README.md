# Browser

`ninja web` compiles the game with Emscripten for 32-bit WebAssembly. The
result is a site, `build/web/site`: the page, its scripts and `halo.wasm`.
The game draws with WebGL 2. It plays sound and reads the keyboard, the mouse
and gamepads through SDL3. System link plays between the browsers of a site.

The browser build uses the platform layer of the Linux build
(`port/linux/src`), as the Android build does: OpenGL ES 3 (`HALO_GLES`), 32-bit
code that is not x86. `port/web/src` has the pieces a browser does without:

| File | What |
| --- | --- |
| `web_platform.c` | The settings the page decides, the files, the canvas, and the frames. |
| `web_maps_backend.cpp` | The server's maps at `/data/maps`, read in HTTP ranges. |
| `posix_net_web.c` | Sockets between the pages of a site (the game's system link). |
| `posix_bridge.c` | The rings `shell/net.js` carries to the other pages over WebRTC. |
| `memory_watch_web.c` | Texture write tracking without page protection. |
| `web_gl.c` | The OpenGL ES helpers the Android host gives, on WebGL. |
| `web_stubs.c` | The desktop's updater and disc image import, which a page has no use for. |

## Requirements

- The tools of the Linux build (Python, ninja).
- Emscripten (`emsdk`, in `EMSDK` or `~/emsdk`). `configure.py` adds the
  browser build when it finds `emcc`.

## Build

1. Enter `source ~/emsdk/emsdk_env.sh`.
2. Enter `python configure.py --release`.
3. Enter `ninja web`.

## The site

The game needs `maps/` from an Xbox disc image, on the server. The page reads
the maps in pieces as the game asks (HTTP ranges); nothing is downloaded whole.

1. Copy `maps/` to the server, to `/srv/halo/maps`.
2. Enter `port/web/deploy.sh`. It copies the site to `/srv/halo/site` and
   writes `maps/index.json`, which lists the maps.
3. Install `port/web/nginx.conf` as an nginx site. It serves the site on port
   8080 with the headers the game's threads need (cross-origin isolation).
4. Install mosquitto with a WebSocket listener on 127.0.0.1:9001 (the pages
   find each other through it, at `/mqtt`).

The page must be served over HTTPS (a reverse proxy in front of port 8080),
or from `localhost`: browsers give threads only to secure pages.

`shell/site-config.js` names the site's room: the pages with the same room
play system link together.

## Settings

The settings of `config.toml` are in the browser's storage, with the saved
games. The page's address sets any setting by its environment name
(`port/linux/src/port_config.c`), for example `?HALO_MAX_FPS=60`.
`?debug` shows the game's log on screen. The log is always in the browser's
console.

For tests: `?HALO_COOP_TEST=a10:4` starts a campaign level (`a10`, `b30`,
...) at once in co-op with that many local players (two to four), each on
the controller of their number; `?HALO_WEB_DUMP_UI=1` logs every menu widget
(`game/web_menus.c`, which changes the menus).

## Co-op

COOPERATIVE PLAY takes two to four players on one screen: they join on the
four-way screen split screen uses (press A or START on each controller, and
pick a profile), and once one presses A again, the campaign's level list.
Each player needs a controller of their own; the keyboard and mouse play as
the first controller's player.

## Differences from the native builds

- Lens flares do not show: WebGL gives a visibility test's result only
  after the game's thread returns to the browser, which it never does.
- Internet play is off (it needs UDP). System link plays between the pages
  of the site instead.
- There is no Bink video.
