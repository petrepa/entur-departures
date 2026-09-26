# Appstore listing

Everything the Pebble appstore asks for, in one place. `pebble publish` (pebble-tool
5.0.30+) prompts for the name, short description, source URL, category and icons
the first time an app is created; the long description, banner and extra
screenshots go in through the web dashboard. The name, category and icons can be
changed there later.

**Not published.** Before publishing, work through the checklist at the bottom.

## App name

Entur Departures

## Short description

Next departures for your regular journeys in Norway, with the direction picked automatically from where you are.

## Category

`daily`

## Description

(about 1,500 of 1,600 characters)

```
Glance at your wrist, see your next train, and see which way it's going.

Entur Departures shows live public transport departures for up to four journeys you set up on your phone, such as Oslo S – Ski. You don't choose a direction: the app checks which end of the journey you're nearer and shows departures from there. Near Oslo S you get trains to Ski, and near Ski you get trains back.

Each departure shows:
• The realtime departure time and a countdown in minutes
• The line (R21, RE20, bus 31, ...)
• The boarding track and the arrival track ("Spor 12 > 1"), so you can choose between two trains leaving at nearly the same time

Per-journey options:
• Big text: the countdown is huge and the track is bold, so you can read it at a glance while walking or cycling
• Bike-friendly trains: a bike symbol on departures with step-free doors and room for a bike. You choose which lines or train numbers count, and it defaults to the Type 75 lines on Oslo – Ski.

Works for trains, metro, trams, buses and ferries anywhere in Norway. In settings, type a stop name like "Nydalen" or "Bergen stasjon", or paste an NSR:StopPlace id.

Boards are fetched in the background as soon as the app opens, so they're usually ready before you pick a journey.

Privacy: your location is only used on your phone to decide the direction. It's never sent anywhere. Entur only receives the stops you set up.

Unofficial app, not made or endorsed by Entur. Departure data comes from Entur's open APIs (NLOD licence).

Built for Pebble Time 2.
```

## Release notes (1.1.0)

```
First public release.
• Up to four journeys, with the direction picked automatically by GPS
• Live departures with boarding and arrival track
• Big text mode for reading at a glance
• Bike symbol on bike-friendly trains
```

## Keywords

entur, train, tog, departures, avganger, ruter, vy, norway, norge, oslo, public transport, kollektiv, bike, sykkel

## Links

| Field | Value |
|-------|-------|
| Source URL | https://github.com/petrepa/entur-departures (**the repo is private**, so make it public or leave this empty) |
| Website | The same repo URL, or empty |
| Support email | Defaults to the developer account's email |

## Artwork

| Asset | File | Size |
|-------|------|------|
| Large icon | `../icon_144x144.png` | 144×144 |
| Small icon | `../icon_80x80.png` | 80×80 |
| Marketing banner | `banner_720x320.png` | 720×320 |
| Screenshot 1: journeys | `screenshots/emery-1-journeys.png` | 200×228 |
| Screenshot 2: departure board with bikes | `screenshots/emery-2-board.png` | 200×228 |
| Screenshot 3: big-text board | `screenshots/emery-3-big-text.png` | 200×228 |
| Screenshot 4: another journey | `screenshots/emery-4-auto-direction.png` | 200×228 |

`python3 store/make_assets.py` regenerates the icons and banner. The banner
uses screenshots 2 and 3.

The screenshots were taken from the emery emulator on **SDK 4.33.1** against live
Entur data, with demo journeys and a location fixed at Oslo S. The older
SDK 4.9.77 emulator firmware has no `→` glyph and draws it as a box. To retake
them, temporarily set `DEFAULT_JOURNEYS` and `lastLoc` in `src/pkjs/index.js`,
then run:

```
pebble install --emulator emery
pebble screenshot --no-open --emulator emery store/screenshots/<name>.png
```

## To publish

```
pebble login
pebble publish --screenshots store/screenshots/*.png \
  --icon-small icon_80x80.png --icon-large icon_144x144.png
```

Leave out `--is-published` for the first upload. The release then stays hidden
until you publish it from the dashboard.

## Checklist before publishing

- [ ] Merge the bike-friendly trains PR. This listing and the screenshots describe it.
- [ ] Decide on the name. "Entur" is another organisation's brand. The
      description says the app is unofficial, but a name like "Departures (Entur
      data)" is safer. Alternatively, ask Entur.
- [ ] Set a unique `ET-Client-Name` for the public build. Entur asks for
      `<company>-<app>`, and the current default `peter-pebble-departures` is
      acceptable.
- [ ] Make the repo public, or drop the source URL.
- [ ] Consider changing `author` in `package.json` (currently "Peter"). It is
      shown as the developer name.
- [ ] Test the Clay settings page on a real phone.
- [ ] Upgrade pebble-tool (`uv tool upgrade pebble-tool`). 5.0.16 has no
      `publish` command.
