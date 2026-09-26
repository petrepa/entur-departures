// Clay configuration for Entur Departures.
//
// Each journey is a fixed pair of stops. Type a stop NAME (e.g. "Oslo S",
// "Ski", "Nydalen") — the phone geocodes it against Entur's register on save.
// You can also paste an exact "NSR:StopPlace:XXXXX" id if a name is ambiguous.
// The travel DIRECTION is chosen automatically from your GPS location, so the
// order you enter From/To only matters as a fallback when location is unknown.

function journeySection(n) {
  return {
    "type": "section",
    "items": [
      { "type": "heading", "defaultValue": "Journey " + n },
      {
        "type": "input",
        "messageKey": "J" + n + "_LABEL",
        "label": "Name",
        "description": "Shown in the list, e.g. \"Oslo–Ski\"",
        "attributes": { "placeholder": "Oslo–Ski", "limit": 28 }
      },
      {
        "type": "input",
        "messageKey": "J" + n + "_FROM",
        "label": "Stop A",
        "attributes": { "placeholder": "Oslo S", "limit": 60 }
      },
      {
        "type": "input",
        "messageKey": "J" + n + "_TO",
        "label": "Stop B",
        "attributes": { "placeholder": "Ski", "limit": 60 }
      },
      {
        "type": "toggle",
        "messageKey": "J" + n + "_BIG",
        "label": "Big text",
        "description": "Large, glanceable departures — readable while cycling. " +
          "Fewer fit on screen at once.",
        "defaultValue": false
      },
      {
        "type": "toggle",
        "messageKey": "J" + n + "_BIKE",
        "label": "Mark bike-friendly trains",
        "description": "Show a bike symbol on departures whose line or train " +
          "number is in the bike-friendly list below.",
        "defaultValue": false
      }
    ]
  };
}

module.exports = [
  { "type": "heading", "defaultValue": "Entur Departures" },
  {
    "type": "text",
    "defaultValue": "Define up to 4 journeys. Direction is picked automatically " +
      "from your location — near Stop A you'll see A→B, near Stop B you'll see B→A."
  },
  journeySection(1),
  journeySection(2),
  journeySection(3),
  journeySection(4),
  {
    "type": "section",
    "items": [
      { "type": "heading", "defaultValue": "Options" },
      {
        "type": "slider",
        "messageKey": "NUM_DEPARTURES",
        "label": "Departures to show",
        "defaultValue": 5,
        "min": 2,
        "max": 10,
        "step": 1
      },
      {
        "type": "input",
        "messageKey": "BIKE_LINES",
        "label": "Bike-friendly trains",
        "description": "Line codes and/or train numbers, comma separated. " +
          "Default R21, R22, R23: on Oslo–Ski these run Type 75 (Flirt) with " +
          "step-free doors and a multi-purpose area. RE20 is left out because " +
          "rush-hour departures can be Type 73 with steps and a cramped bike " +
          "room; add its step-free Type 74 trains by number (e.g. 107) if you " +
          "know them.",
        "defaultValue": "R21, R22, R23",
        "attributes": { "limit": 120 }
      },
      {
        "type": "input",
        "messageKey": "ET_CLIENT_NAME",
        "label": "Entur client name",
        "description": "Identifies the app to Entur (company-application).",
        "defaultValue": "peter-pebble-departures",
        "attributes": { "limit": 60 }
      }
    ]
  },
  { "type": "submit", "defaultValue": "Save" }
];
