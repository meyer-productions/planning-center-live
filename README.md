# Planning Center Live for OBS

Native OBS plugin that shows **Planning Center Services LIVE** directly in OBS.
It talks to the Planning Center API with a Personal Access Token – no browser
login, no expiring session cookies, no cookie banner.

Add the source **Planning Center Live** to a scene and pick the view in its
properties:

| View | |
|---|---|
| **Lower third – countdown** (default) | Slim single-line bar at the very bottom: current item, next item, remaining time, progress. Amber below the warning threshold, red `+m:ss` when the item runs over. Before the service it shows "starts in"; otherwise it hides itself. |
| **Big countdown** | Remaining time of the current item, large (stage / confidence monitor). |
| **Run sheet** | Like PCO LIVE: planned start times, keys, lengths, current item highlighted, auto-scroll. |
| **Service countdown** | Countdown to the service start, then elapsed time and planned end. |
| **Clock** | Small clock plus current item and remaining time. |
| **Hidden** | Nothing. |

All views are laid out for 16:9 (1920×1080 by default, size configurable).

## Source properties

- **View** – the views above.
- **Appearance** – accent colour, warning threshold, show next item / progress
  bar, "starts in" window, lower third distance from bottom and sides, opaque
  background (for a confidence monitor), overlay language (German / English),
  width/height.
- **Planning Center connection** – Application ID + Secret, service type
  (button *Load service types*), optional fixed plan ID, poll interval, demo
  mode, status. The connection is remembered: new sources start with the
  last used token and service type.

### Switching views

- per source in the properties,
- **hotkeys** per source (*Settings → Hotkeys → [source name]*: one per view + *next view*),
- remotely via obs-websocket (e.g. Bitfocus Companion / Stream Deck):
  `SetInputSettings` with `{"inputName": "Planning Center Live", "inputSettings": {"view": "countdown"}}`.

Valid view values: `lowerthird`, `countdown`, `list`, `service`, `clock`, `off`.

Several sources can run at the same time (e.g. lower third on the program
feed, run sheet on a multiview). Sources with the same connection share one
set of API requests.

## Installation

Download the package for Windows (`.exe`/`.zip`), macOS (`.pkg`) or Ubuntu/Debian (`.deb`) from the
[Releases](https://github.com/meyer-productions/planning-center-live/releases)
or from the build artifacts of the latest *Push* workflow run, install it and
restart OBS (OBS 31 or newer).

Create a Personal Access Token at
<https://api.planningcenteronline.com/oauth/applications> → *Personal Access Tokens*.

## How it works

- `GET /services/v2/service_types/{id}/plans?filter=past|future` – picks the plan
  (the latest plan stays selected for 3 hours after its last service time)
- `GET …/plans/{id}/items` + `…/plan_times` – run sheet and service times (every 30 s)
- `GET …/plans/{id}/live?include=current_item_time,next_item_time` – LIVE state
  (every poll, default 2 s; PCO allows 100 requests per 20 s)
- Remaining time = `live_start_at` of the current item + item length − now.
- The source renders a private browser source with the bundled
  `data/overlay/overlay.html` and pushes the state into it with the browser
  source's `javascript_event` procedure. Between polls the page counts down locally.

Pre-service items (`service_position = pre`) are scheduled backwards from the
service start, like in Planning Center.

## Building

The plugin uses the official [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate).
GitHub Actions builds Windows, macOS and Ubuntu packages on every push to `main`; a tag
like `1.0.0` creates a release. Local build: see the template documentation
(Visual Studio 2022 + CMake 3.28 on Windows, Xcode on macOS).

HTTP uses WinHTTP on Windows and the system libcurl on macOS/Linux; JSON is handled
with libobs' `obs_data`, so there are no extra dependencies.

## Other folders

- [`python-script/`](python-script/) – the earlier OBS Python script version
  (overlay via a local web server, also usable without OBS for testing).
- [`legacy/`](legacy/) – the original Linux script that opened PCO LIVE in Firefox kiosk mode.

## License

GPL-2.0 (required for OBS plugins) – meyer.productions
