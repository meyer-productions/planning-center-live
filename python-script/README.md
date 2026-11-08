# Planning Center Live – OBS Python script

> The native plugin in the repository root is the recommended version. This script
> is kept for testing without OBS and as a fallback.

OBS script that reads **Planning Center Services LIVE** directly from the
Planning Center API (Personal Access Token) and shows it as switchable overlays
in OBS – no browser login, no expiring session cookies, no cookie banner.

## Views

| Key | View |
|---|---|
| `lowerthird` | **Lower third with countdown** (default): current item, remaining time, next item, progress bar. Turns amber below the warning threshold and red with `+m:ss` when the item runs over. Before the service it shows the countdown to the start; otherwise it is hidden. |
| `countdown` | Big countdown of the current item (stage / confidence monitor). |
| `list` | Run sheet like PCO LIVE: planned start times, keys, lengths, current item highlighted, auto-scroll. |
| `service` | Countdown to the service start, then elapsed service time and planned end. |
| `clock` | Small clock top right plus current item and remaining time. |
| `off` | Nothing (transparent). |

Views fade in/out. The **active view** can be switched by:

- the script settings (*Aktive Ansicht*),
- OBS hotkeys (*Settings → Hotkeys → "Planning Center: …"*, one per view plus *Next view*),
- HTTP, e.g. from Bitfocus Companion / Stream Deck: `GET http://127.0.0.1:8765/api/view/countdown`
  (`/api/view/next`, `/api/view` = current view, `/api/refresh` = reload data).

A browser source can also be locked to one view: `http://127.0.0.1:8765/overlay?view=list`.
That way you can e.g. have the lower third on the stream and the run sheet on a
multiview/confidence screen at the same time.

### URL parameters

| Parameter | Meaning |
|---|---|
| `view=` | `active` (default, follows the switch) or a fixed view key |
| `accent=%23e4002b` | accent colour override for this source |
| `bg=1` | opaque dark background (for monitors / a browser window on a second screen) |
| `scale=1` | fixed scale; by default the 1920×1080 layout fits the window |

## Installation

1. **Python for OBS** (Windows): install Python 3.12 64-bit from python.org, then in OBS
   *Tools → Scripts → Python Settings* set the install path
   (e.g. `C:\Users\<you>\AppData\Local\Programs\Python\Python312`). Only the standard library is used – no `pip install`.
2. Keep the repository layout (the script loads `../data/overlay/overlay.html`), or copy
   `overlay.html` next to the script.
3. OBS: *Tools → Scripts → +* → `planning_center_live.py`.
4. Create a Personal Access Token at
   <https://api.planningcenteronline.com/oauth/applications> → *Personal Access Tokens*
   and enter *Application ID* and *Secret*.
   Alternatively leave the fields empty and put `PCO_APP_ID`, `PCO_SECRET` and
   `PCO_SERVICE_TYPE` into a `.env` file next to the script or one folder up
   (see `.env.example`) – then the token is not stored in the scene collection.
5. Click **Service Types laden**, choose the service type.
6. Click **Browser-Quelle in aktuelle Szene einfügen** (1920×1080 browser source
   `Planning Center Live` pointing to `http://127.0.0.1:8765/overlay`).

The script automatically picks the plan: the most recent plan if its last service
time ended less than 3 hours ago, otherwise the next future plan. A plan ID can be
forced in the settings (the number in `services.planningcenteronline.com/plans/<ID>`).

### Settings

| Setting | Default | |
|---|---|---|
| Abfrage-Intervall | 2 s | how often LIVE is polled (one request per poll; items/plan times every 30 s). PCO allows 100 requests per 20 s. |
| Warnung ab Restzeit | 30 s | amber colour threshold |
| Countdown zum Beginn ab | 30 min | lower third / big countdown show "starts in" from this time on |
| Im Netzwerk freigeben | off | bind to `0.0.0.0` so Companion or another PC can reach the overlay/API |
| Demo-Modus | off | fake plan to design/test without Planning Center |

## Testing without OBS

```powershell
python planning_center_live.py --demo            # http://127.0.0.1:8765/overlay?bg=1
$env:PCO_APP_ID="…"; $env:PCO_SECRET="…"
python planning_center_live.py --list-service-types
python planning_center_live.py --service-type 123456 --view list
```

## How it works

- `GET /services/v2/service_types/{st}/plans?filter=past|future` – plan selection
- `GET …/plans/{id}/items` and `…/plan_times` – run sheet and service times
- `GET …/plans/{id}/live?include=current_item_time,next_item_time` – LIVE state
  (falls back to `…/live/{id}/current_item_time` and `next_item_time`)
- Remaining time = `live_start_at + length − now` of the current `ItemTime`; the
  overlay counts down locally between polls, synced to the script's clock.

Notes: credentials are stored in the OBS scene collection settings in plain text,
like every OBS script setting. The overlay server listens on `127.0.0.1` only
unless *Im Netzwerk freigeben* is enabled; it never exposes the credentials.

## License

MIT – meyer.productions
