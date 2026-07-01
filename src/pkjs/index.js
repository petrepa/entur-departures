/**
 * Entur Departures — PebbleKit JS (phone side)
 *
 * Does all the network + geo work:
 *   - Reads journeys from Clay settings (stop names), geocodes them to
 *     NSR:StopPlace ids + coordinates via Entur Geocoder (once, then cached).
 *   - On each departures request, gets GPS, picks travel direction by choosing
 *     the nearer of the journey's two stops as the origin, and queries Entur
 *     JourneyPlanner v3 `trip` for the next departures in that direction.
 *   - Sends compact display strings to the watch over AppMessage.
 *
 * Entur APIs are open (no key) but require an ET-Client-Name header.
 */

var Clay = require('@rebble/clay');
var clayConfig = require('./config');
// Manual event handling so we can geocode before pushing anything to the watch.
var clay = new Clay(clayConfig, null, { autoHandleEvents: false });

// ---------------------------------------------------------------------------
// Config / state
// ---------------------------------------------------------------------------

var GRAPHQL_URL = 'https://api.entur.io/journey-planner/v3/graphql';
var GEOCODER_URL = 'https://api.entur.io/geocoder/v1/autocomplete';

var DEFAULT_CLIENT = 'peter-pebble-departures';

// Seed journey (used until the user configures their own in phone settings).
var DEFAULT_JOURNEYS = [
  { label: 'Oslo–Ski', from: 'Oslo S', to: 'Ski' }
];

var etClientName = DEFAULT_CLIENT;
var numDepartures = 5;

// Resolved journeys: [{ label, a:{id,name,lat,lon}, b:{id,name,lat,lon} }]
var resolved = null;

// Cached GPS fix.
var lastLoc = null;
var lastLocTs = 0;
var LOC_MAX_AGE = 180000; // 3 min

// ---------------------------------------------------------------------------
// HTTP helpers (always send ET-Client-Name)
// ---------------------------------------------------------------------------

function httpGet(url, onOk, onErr) {
  var xhr = new XMLHttpRequest();
  xhr.open('GET', url);
  xhr.setRequestHeader('ET-Client-Name', etClientName);
  xhr.onload = function () {
    if (xhr.status >= 200 && xhr.status < 300) onOk(xhr.responseText);
    else onErr('HTTP ' + xhr.status);
  };
  xhr.onerror = function () { onErr('network'); };
  xhr.ontimeout = function () { onErr('timeout'); };
  xhr.timeout = 15000;
  xhr.send();
}

function graphQL(query, onOk, onErr) {
  var xhr = new XMLHttpRequest();
  xhr.open('POST', GRAPHQL_URL);
  xhr.setRequestHeader('Content-Type', 'application/json');
  xhr.setRequestHeader('ET-Client-Name', etClientName);
  xhr.onload = function () {
    if (xhr.status >= 200 && xhr.status < 300) {
      try {
        var body = JSON.parse(xhr.responseText);
        if (body.errors && body.errors.length) { onErr('api error'); return; }
        onOk(body.data);
      } catch (e) { onErr('bad json'); }
    } else { onErr('HTTP ' + xhr.status); }
  };
  xhr.onerror = function () { onErr('network'); };
  xhr.ontimeout = function () { onErr('timeout'); };
  xhr.timeout = 15000;
  xhr.send(JSON.stringify({ query: query }));
}

// ---------------------------------------------------------------------------
// Geo helpers
// ---------------------------------------------------------------------------

// Haversine distance in metres (fine for "which stop is nearer").
function distance(lat1, lon1, lat2, lon2) {
  var toRad = Math.PI / 180;
  var dLat = (lat2 - lat1) * toRad;
  var dLon = (lon2 - lon1) * toRad;
  var a = Math.sin(dLat / 2) * Math.sin(dLat / 2) +
          Math.cos(lat1 * toRad) * Math.cos(lat2 * toRad) *
          Math.sin(dLon / 2) * Math.sin(dLon / 2);
  return 6371000 * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
}

function getLocation(cb) {
  var now = Date.now();
  if (lastLoc && (now - lastLocTs) < LOC_MAX_AGE) { cb(lastLoc); return; }
  navigator.geolocation.getCurrentPosition(
    function (pos) {
      lastLoc = { lat: pos.coords.latitude, lon: pos.coords.longitude };
      lastLocTs = Date.now();
      cb(lastLoc);
    },
    function () { cb(lastLoc); },   // fall back to stale fix, or null
    { timeout: 15000, maximumAge: 120000 }
  );
}

// Origin = nearer stop; destination = the other. Falls back to A->B if no fix.
function pickDirection(journey, loc) {
  if (loc) {
    var da = distance(loc.lat, loc.lon, journey.a.lat, journey.a.lon);
    var db = distance(loc.lat, loc.lon, journey.b.lat, journey.b.lon);
    if (db < da) return { origin: journey.b, dest: journey.a, known: true };
    return { origin: journey.a, dest: journey.b, known: true };
  }
  return { origin: journey.a, dest: journey.b, known: false };
}

// ---------------------------------------------------------------------------
// Resolving stop names -> {id, name, lat, lon}
// ---------------------------------------------------------------------------

function resolvePlace(text, cb) { // cb(err, place)
  text = (text || '').trim();
  if (!text) { cb('empty'); return; }

  if (/^NSR:StopPlace:/i.test(text)) {
    var q = '{stopPlace(id:"' + text + '"){id name latitude longitude}}';
    graphQL(q, function (data) {
      var sp = data && data.stopPlace;
      if (!sp || sp.latitude == null) { cb('not found: ' + text); return; }
      cb(null, { id: sp.id, name: sp.name, lat: sp.latitude, lon: sp.longitude });
    }, cb);
    return;
  }

  var url = GEOCODER_URL + '?size=1&layers=venue&lang=no&text=' +
            encodeURIComponent(text);
  httpGet(url, function (resp) {
    var j;
    try { j = JSON.parse(resp); } catch (e) { cb('bad json'); return; }
    if (!j.features || !j.features.length) { cb('no stop: ' + text); return; }
    var f = j.features[0];
    cb(null, {
      id: f.properties.id,
      name: f.properties.name || f.properties.label,
      lat: f.geometry.coordinates[1],   // GeoJSON is [lon, lat]
      lon: f.geometry.coordinates[0]
    });
  }, cb);
}

// Resolve a list of {label, from, to} sequentially into `resolved` journeys.
function resolveJourneys(raw, done) {
  var out = [];
  var i = 0;
  function next() {
    if (i >= raw.length) { done(out); return; }
    var j = raw[i++];
    if (!j.from || !j.to) { next(); return; }   // skip incomplete slots
    resolvePlace(j.from, function (eA, a) {
      if (eA) { console.log('resolve A failed: ' + eA); next(); return; }
      resolvePlace(j.to, function (eB, b) {
        if (eB) { console.log('resolve B failed: ' + eB); next(); return; }
        out.push({ label: j.label || (a.name + '–' + b.name), a: a, b: b });
        next();
      });
    });
  }
  next();
}

// ---------------------------------------------------------------------------
// Persistence of resolved journeys
// ---------------------------------------------------------------------------

function saveResolved() {
  try {
    localStorage.setItem('resolved', JSON.stringify(resolved));
    localStorage.setItem('etClientName', etClientName);
    localStorage.setItem('numDepartures', String(numDepartures));
  } catch (e) { /* ignore quota errors */ }
}

function loadResolved() {
  try {
    var r = localStorage.getItem('resolved');
    if (r) resolved = JSON.parse(r);
    var c = localStorage.getItem('etClientName');
    if (c) etClientName = c;
    var n = localStorage.getItem('numDepartures');
    if (n) numDepartures = parseInt(n, 10) || 5;
  } catch (e) { resolved = null; }
}

// Make sure `resolved` is populated (from storage, else seed defaults).
function ensureResolved(cb) {
  if (resolved) { cb(); return; }
  loadResolved();
  if (resolved) { cb(); return; }
  resolveJourneys(DEFAULT_JOURNEYS, function (out) {
    resolved = out;
    saveResolved();
    cb();
  });
}

// ---------------------------------------------------------------------------
// Formatting + AppMessage out
// ---------------------------------------------------------------------------

function shortName(name) {
  // Trim the ", <city>" suffix geocoder adds, keep it compact for the watch.
  var c = name.indexOf(',');
  return c > 0 ? name.substring(0, c) : name;
}

function isoLocalHHMM(iso) {
  // iso is "...T17:45:44+02:00" already in Oslo local time.
  var t = iso.indexOf('T');
  return t >= 0 ? iso.substr(t + 1, 5) : iso;
}

function etaText(iso) {
  var mins = Math.round((new Date(iso).getTime() - Date.now()) / 60000);
  if (mins <= 0) return 'now';
  if (mins === 1) return '1 min';
  if (mins < 60) return mins + ' min';
  return Math.floor(mins / 60) + 'h ' + (mins % 60) + 'm';
}

function sendToWatch(dict) {
  Pebble.sendAppMessage(dict,
    function () {},
    function (e) { console.log('sendAppMessage failed'); });
}

function sendError(index, msg) {
  sendToWatch({ MSG_TYPE: 2, JOURNEY_INDEX: index, PAYLOAD: msg });
}

// Build + send the journeys list (with current auto-direction) to the watch.
function sendMenu() {
  ensureResolved(function () {
    getLocation(function (loc) {
      var rows = resolved.map(function (j) {
        var d = pickDirection(j, loc);
        var arrow = d.known ? '→ ' : '⇄ ';
        return j.label + '\t' + arrow + shortName(d.dest.name);
      });
      sendToWatch({
        MSG_TYPE: 0,
        COUNT: resolved.length,
        PAYLOAD: rows.join('\n')
      });
    });
  });
}

// Fetch + send the departure board for one journey.
function sendDepartures(index) {
  ensureResolved(function () {
    var j = resolved[index];
    if (!j) { sendError(index, 'No such journey'); return; }
    getLocation(function (loc) {
      var d = pickDirection(j, loc);
      var q =
        '{trip(from:{place:"' + d.origin.id + '"} to:{place:"' + d.dest.id +
        '"} numTripPatterns:' + numDepartures + '){tripPatterns{' +
        'expectedStartTime legs{mode expectedStartTime line{publicCode} ' +
        'fromPlace{quay{publicCode}}}}}}';
      graphQL(q, function (data) {
        var patterns = data && data.trip && data.trip.tripPatterns;
        var header = shortName(d.origin.name) + ' → ' + shortName(d.dest.name);
        if (!patterns || !patterns.length) {
          sendToWatch({ MSG_TYPE: 1, JOURNEY_INDEX: index, PAYLOAD: header });
          return;
        }
        var lines = [header];
        patterns.forEach(function (p) {
          // First non-walking leg carries the real departure + line.
          var leg = null;
          for (var k = 0; k < p.legs.length; k++) {
            if (p.legs[k].mode !== 'foot') { leg = p.legs[k]; break; }
          }
          var iso = (leg && leg.expectedStartTime) || p.expectedStartTime;
          var code = (leg && leg.line && leg.line.publicCode) || '';
          // Boarding track/platform = the quay of the first non-walking leg.
          var track = '';
          if (leg && leg.fromPlace && leg.fromPlace.quay &&
              leg.fromPlace.quay.publicCode) {
            var railish = (leg.mode === 'rail' || leg.mode === 'metro');
            track = (railish ? 'Spor ' : 'Pl. ') + leg.fromPlace.quay.publicCode;
          }
          lines.push(isoLocalHHMM(iso) + '\t' + code + '\t' + etaText(iso) +
                     '\t' + track);
        });
        sendToWatch({
          MSG_TYPE: 1,
          JOURNEY_INDEX: index,
          PAYLOAD: lines.join('\n')
        });
      }, function (err) {
        sendError(index, err);
      });
    });
  });
}

// ---------------------------------------------------------------------------
// Clay settings -> raw journeys
// ---------------------------------------------------------------------------

function val(settings, key) {
  var v = settings[key];
  if (v && typeof v === 'object' && 'value' in v) v = v.value;
  return (v == null) ? '' : ('' + v).trim();
}

function applySettings(settings) {
  var client = val(settings, 'ET_CLIENT_NAME');
  if (client) etClientName = client;
  var n = parseInt(val(settings, 'NUM_DEPARTURES'), 10);
  if (n >= 2 && n <= 10) numDepartures = n;

  var raw = [];
  for (var i = 1; i <= 4; i++) {
    raw.push({
      label: val(settings, 'J' + i + '_LABEL'),
      from:  val(settings, 'J' + i + '_FROM'),
      to:    val(settings, 'J' + i + '_TO')
    });
  }
  return raw;
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

Pebble.addEventListener('ready', function () {
  console.log('Entur Departures JS ready');
  loadResolved();
  sendMenu();   // proactively populate the watch list on launch
});

Pebble.addEventListener('appmessage', function (e) {
  var p = e.payload || {};
  if (!('REQUEST' in p)) return;
  if (p.REQUEST === 1) {
    sendMenu();
  } else if (p.REQUEST === 2) {
    sendDepartures(p.JOURNEY_INDEX || 0);
  }
});

Pebble.addEventListener('showConfiguration', function () {
  Pebble.openURL(clay.generateUrl());
});

Pebble.addEventListener('webviewclosed', function (e) {
  if (!e || !e.response) return;
  var settings = clay.getSettings(e.response, false);
  var raw = applySettings(settings);
  resolveJourneys(raw, function (out) {
    if (out.length) {
      resolved = out;
    } else {
      // Nothing configured — keep defaults so the app still works.
      resolveJourneys(DEFAULT_JOURNEYS, function (def) { resolved = def; saveResolved(); sendMenu(); });
      return;
    }
    saveResolved();
    sendMenu();
  });
});
