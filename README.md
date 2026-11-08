# Planning Center Live for OBS

An OBS Studio plugin that shows **Planning Center Services LIVE** directly in
OBS – a countdown lower third, a big countdown, the run sheet, a service
countdown and a clock.

It reads the Planning Center API with a Personal Access Token. No browser, no
login session that expires, no cookie banner.

## Features

- **Source "Planning Center Live"** with switchable views:

  | View | |
  |---|---|
  | **Lower third – countdown** (default) | Slim bar at the bottom edge: current item, next item, remaining time, progress bar and a clock on the right. Amber below the warning threshold, red `+m:ss` when the item runs over. Before the service it shows "starts in", otherwise it hides itself. |
  | **Stage banner** | Large banner at the bottom: current item and next item, a big remaining-time timer (readable from the stage) and an equally large clock. Optional white background. Before the service it shows "starts in", otherwise plan + clock. |
  | **Big countdown** | Remaining time of the current item, large (stage / confidence monitor). |
  | **Run sheet** | Like PCO LIVE: planned start times, keys, lengths, current item highlighted, auto-scroll. |
  | **Service countdown** | Countdown to the service start, then elapsed time and planned end. |
  | **Clock** | Clock plus current item and remaining time. |
  | **Current item + timer + clock** | Like "Clock", but swapped: the remaining time is the big number (warning/over-time colours, "starts in" before the service), current item and clock beside it. The clock can be hidden. |
  | **Hidden** | Nothing. |

- Lower third: width, height, distance from bottom/sides, clock on/off.
- Stage banner: height, white background, clock with/without seconds
  (shares the distance from bottom/sides with the lower third).
- Long item titles shrink (down to 60 %) before they are cut off.
- Appearance: accent colour, warning threshold, next item, progress bar,
  "starts in" window, opaque background, German/English labels, canvas size.
- Several sources at the same time (e.g. lower third on the program, run sheet
  on the multiview) – they share one connection.
- Hotkeys per source for every view and "next view", plus "stage banner
  white/dark".
- Update check: on start the plugin looks for a newer GitHub release (log +
  Tools → Planning Center Live settings, hint in the source properties).
  "Download update" opens the matching file (Windows ZIP / macOS pkg, Linux:
  release page) – installation stays manual, nothing is replaced automatically.
- Remote switching via obs-websocket (e.g. Bitfocus Companion / Stream Deck).

All views are laid out for 16:9 (1920×1080 by default).

## Installation

Download the package for your system from
[Releases](https://github.com/meyer-productions/planning-center-live/releases),
install it and restart OBS. Requires **OBS Studio 31 or newer** (64-bit;
on Linux the version the package was built for, currently 32+).

### Windows

Extract `planning-center-live-…-windows-x64.zip` into
`C:\ProgramData\obs-studio\plugins\` (the result is
`C:\ProgramData\obs-studio\plugins\planning-center-live\bin` and `\data`).

### macOS

Open `planning-center-live-…-macos-universal.pkg`. The package is not signed
by Apple: if macOS blocks it, allow it under *System Settings → Privacy &
Security → Open Anyway*.

### Linux – OBS from the official PPA

```bash
sudo apt install ./planning-center-live-…-x86_64-linux-gnu.deb
```

### Linux – OBS as Flatpak

The Flatpak loads plugins from its own config folder:

```bash
dpkg-deb -x planning-center-live-…-x86_64-linux-gnu.deb /tmp/pcl
P=~/.var/app/com.obsproject.Studio/config/obs-studio/plugins/planning-center-live
mkdir -p "$P/bin/64bit" "$P/data"
cp /tmp/pcl/usr/lib/x86_64-linux-gnu/obs-plugins/planning-center-live.so "$P/bin/64bit/"
cp -r /tmp/pcl/usr/share/obs/obs-plugins/planning-center-live/. "$P/data/"
```

Tested with the Flatpak OBS 33 (together with DistroAV).

## Setup

1. Create a **Personal Access Token** at
   <https://api.planningcenteronline.com/oauth/applications> → *Personal Access Tokens*.
2. In OBS: **Tools → Planning Center Live settings**
   - enter *Application ID* and *Secret*,
   - click *Load service types* and choose your service type,
   - optional: fixed plan ID, poll interval (default 2 s), demo mode.
3. Add the source **Planning Center Live** to a scene and choose the view in its properties.

The plugin picks the plan automatically: the latest plan stays selected for
3 hours after its last service time, otherwise the next upcoming plan.

## Switching views remotely

Every view is a source setting, so obs-websocket can switch it, e.g. from
Bitfocus Companion with a custom request:

```json
{ "requestType": "SetInputSettings",
  "requestData": { "inputName": "Planning Center Live", "inputSettings": { "view": "countdown" } } }
```

View values: `lowerthird`, `stage`, `countdown`, `list`, `service`, `clock`, `timer`, `off`.

## How it works

- `GET /services/v2/service_types/{id}/plans` – plan selection
- `GET …/plans/{id}/items` and `…/plan_times` – run sheet and service times (every 30 s)
- `GET …/plans/{id}/live?include=current_item_time,next_item_time` – LIVE state
  (every poll; Planning Center allows 100 requests per 20 s)
- Remaining time = start of the current item in LIVE + item length − now;
  between polls the overlay counts down locally.
- Pre-service items are scheduled backwards from the service start, like in Planning Center.

The source renders a private OBS browser source with the bundled
`data/overlay/overlay.html` and pushes the data into it – no web server.

## Building

Based on the official [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate).
GitHub Actions builds Windows, macOS and Ubuntu packages on every push to `main`;
pushing a version tag (e.g. `1.0.1`) creates a draft release with all packages.

No extra dependencies: HTTP via WinHTTP (Windows) / libcurl (macOS, Linux),
JSON via libobs.

## License

GPL-2.0 – © meyer.productions
