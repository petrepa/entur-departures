# Entur Departures

A Pebble smartwatch app that shows the next public-transport departures for a set
of predefined **journeys** — where the travel **direction is chosen automatically
from your GPS location**. Standing near Oslo S shows *Oslo S → Ski* trains;
standing near Ski shows *Ski → Oslo S*. Powered by [Entur](https://entur.no)'s
open real-time APIs for Norway.

Built for **Pebble Time 2 (emery)**.

![journeys list](docs/screenshot-list.png) ![departure board](docs/screenshot-board.png)

## Features

- **Predefined journeys** — each is a fixed pair of stops (e.g. Oslo S ↔ Ski).
- **Automatic direction** — the phone's location picks the nearer stop as the
  origin, so you always see departures the way you're actually travelling.
- **Live departures** with realtime-adjusted times and a minutes-until countdown.
- **Boarding and arrival track** for each departure — `Spor 3 > 18` means board
  at track 3 and arrive at track 18 (`Pl.` instead of `Spor` for bus/tram). Seeing
  the arrival track is what lets you pick between two trains leaving at nearly
  the same time when some platforms are easier to reach than others.
- **Phone-configurable** via a Clay settings page — no rebuild to change journeys.

## How it works

All network and geo work happens on the phone in PebbleKit JS; the watch is a thin
display layer driven by AppMessage.

```
Watch (C, MenuLayer)  ⇄  AppMessage  ⇄  Phone (PebbleKit JS)  ⇄  Entur APIs
  journeys list                            geocode stop names → NSR ids
  departure board                          GPS → nearest stop = origin
                                           JourneyPlanner v3 `trip` query
```

- **`src/c/main.c`** — two windows: a `MenuLayer` of journeys and a departure
  board. Parses compact strings from the phone; persists the last list for an
  instant cold-launch.
- **`src/pkjs/index.js`** — reads Clay settings, geocodes stop names to
  `NSR:StopPlace` ids (cached in `localStorage`), gets GPS, picks direction by
  haversine distance, and queries the Entur JourneyPlanner v3 `trip` endpoint.
- **`src/pkjs/config.js`** — the Clay configuration page (4 journey slots,
  departures-to-show, Entur client name).

### Entur APIs

Open under [NLOD](https://data.norge.no/nlod/en/2.0) — **no API key**. The only
requirement is an `ET-Client-Name: <company>-<application>` header on every request.

- JourneyPlanner v3 GraphQL — `https://api.entur.io/journey-planner/v3/graphql`
- Geocoder — `https://api.entur.io/geocoder/v1/autocomplete`

Set your own client name in the app's phone settings.

## Build & install

Requires the modern Pebble toolchain (`pebble-tool` + SDK). See
[developer.repebble.com](https://developer.repebble.com/sdk/).

```bash
pebble build

# emulator
pebble install --emulator emery

# real watch (phone developer connection)
pebble install --phone <phone-ip>
```

## Configure

Open the Pebble phone app → **Entur Departures** → settings (gear), then fill in
up to four journeys. Type a stop **name** (e.g. `Oslo S`, `Nydalen`) — it's
geocoded on save — or paste an exact `NSR:StopPlace:XXXXX` id if a name is
ambiguous. Ships with a default **Oslo–Ski** journey so it works out of the box.

## Navigation

| Screen | UP / DOWN | SELECT | BACK |
|--------|-----------|--------|------|
| Journeys list | scroll | open board | exit |
| Departure board | scroll | refresh | back to list |

## License

MIT
