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
  { label: 'Oslo–Ski', from: 'Oslo S', to: 'Ski', big: false, bike: false }
];

// Trains that are easy to roll a bike on and off. Entur does not expose rolling
// stock, so this is keyed on line code (or train number, for one-off sets). On
// Oslo–Ski, R21/R22/R23 run Type 75 Flirts: step-free doors, open multi-purpose
// area. RE20 is mostly Type 74 (also step-free), but rush-hour extras can be
// Type 73 with steps and a narrow bike room, so it is not marked by default.
var DEFAULT_BIKE_LINES = 'R21, R22, R23';

var etClientName = DEFAULT_CLIENT;
var numDepartures = 5;
var bikeLines = parseBikeLines(DEFAULT_BIKE_LINES);

// Resolved journeys:
//   [{ label, big, bike, a:{id,name,lat,lon}, b:{id,name,lat,lon} }]
// `big` asks the watch for the large, glanceable board — readable while cycling,
// at the cost of fitting fewer departures on screen. `bike` marks departures on
// bike-friendly trains with a bike symbol.
var resolved = null;

// Cached GPS fix, persisted with the time it was taken. Deciding which of two
// stops is nearer tolerates a coarse fix, but not one from the other end of the
// journey: the saved fix is usually from the last time the app was open, which
// may well have been at the other stop.
var lastLoc = null;
var lastLocTs = 0;
var LOC_TIMEOUT = 6000;
// A fix younger than this is trusted as-is.
var LOC_FRESH_MS = 5 * 60 * 1000;
// A fix younger than this isn't worth asking the phone to replace.
var LOC_REUSE_MS = 30 * 1000;
// With only an older fix, opening a board waits this long for a fresh one
// before falling back to the old guess. A later fix still corrects the board.
var LOC_WAIT_MS = 3000;
var locPending = false;
var locWaiters = [];      // callbacks waiting on the fix in progress
var locForceQueued = false;

// What the watch is showing, so a direction change can be pushed to it.
var openIndex = -1;       // journey whose board was last requested
var sentMenuDirs = null;  // origin id per journey in the last menu sent

// Departure boards, keyed by journey index, kept as raw times so a cached entry
// can be re-rendered with correct "x min" countdowns instead of going stale.
var depCache = {};      // index -> { ts, dir, header, rows: [{iso, code, train, track}] }
var depInflight = {};   // index -> true while a fetch is running
var depWaiters = {};    // index -> [cb] waiting on the in-flight fetch
var DEP_SERVE_STALE_MS = 180000;

// "R21, r22 ,107" -> { R21: true, R22: true, '107': true }
function parseBikeLines(text) {
  var set = {};
  ('' + (text || '')).split(/[\s,;]+/).forEach(function (t) {
    if (t) set[t.toUpperCase()] = true;
  });
  return set;
}

function isBikeFriendly(row) {
  return !!((row.code && bikeLines[row.code.toUpperCase()]) ||
            (row.train && bikeLines[row.train]));
}

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

// Ask the phone for a fresh fix, updating the cache. `cb` is optional and runs
// exactly once, with the best fix we have (possibly the old one) — even if
// getCurrentPosition invokes neither of its callbacks. `force` asks for a
// brand-new, high-accuracy fix rather than one the phone has cached.
function refreshLocation(cb, force) {
  // Menu, boards and prefetch all ask at once on launch; one fix serves them.
  if (!force && !locPending && lastLoc && (Date.now() - lastLocTs) < LOC_REUSE_MS) {
    if (cb) cb(lastLoc);
    return;
  }
  if (cb) locWaiters.push(cb);
  if (locPending) {
    if (force) locForceQueued = true;
    return;
  }
  locPending = true;
  var done = false;
  function finish(loc) {
    if (done) return;
    done = true;
    locPending = false;
    if (loc) {
      lastLoc = loc;
      lastLocTs = Date.now();
      try {
        localStorage.setItem('lastLoc', JSON.stringify(loc));
        localStorage.setItem('lastLocTs', String(lastLocTs));
      } catch (e) {}
    }
    var ws = locWaiters;
    locWaiters = [];
    ws.forEach(function (w) { w(lastLoc); });
    if (loc) onLocationUpdated();
    if (locForceQueued) {
      locForceQueued = false;
      refreshLocation(null, true);
    }
  }
  setTimeout(function () { finish(null); }, LOC_TIMEOUT + 1000);
  navigator.geolocation.getCurrentPosition(
    function (pos) {
      finish({ lat: pos.coords.latitude, lon: pos.coords.longitude });
    },
    function () { finish(null); },
    { timeout: LOC_TIMEOUT, maximumAge: force ? 0 : 60000, enableHighAccuracy: !!force }
  );
}

function locIsFresh() {
  return !!lastLoc && (Date.now() - lastLocTs) < LOC_FRESH_MS;
}

// Answer straight away with whatever fix we have and refresh in the background.
// Only a completely cold start has to wait. Used for the menu, where a wrong
// guess is corrected as soon as the fresh fix lands (see onLocationUpdated).
function getLocation(cb) {
  refreshLocation(lastLoc ? null : cb);
  if (lastLoc) cb(lastLoc);
}

// Like getLocation, but an old fix is only used once a fresh one has had
// LOC_WAIT_MS to arrive. Used before building a board, so that opening the app
// in Oslo doesn't show Ski -> Oslo because the saved fix is from Ski this
// morning.
function getTrustedLocation(cb) {
  if (locIsFresh()) { cb(lastLoc); refreshLocation(); return; }
  var done = false;
  function once() { if (!done) { done = true; cb(lastLoc); } }
  refreshLocation(once);
  if (lastLoc) setTimeout(once, LOC_WAIT_MS);
}

function currentDir(index) {
  return pickDirection(resolved[index], lastLoc).origin.id;
}

// A new fix may flip the direction of journeys the watch is already showing.
// Push a corrected menu and board, and re-warm any other board that flipped.
function onLocationUpdated() {
  if (!resolved) return;
  var dirs = resolved.map(function (j, i) { return currentDir(i); });
  if (sentMenuDirs && sentMenuDirs.join() !== dirs.join()) sendMenu();
  resolved.forEach(function (j, i) {
    var c = depCache[i];
    if (i === openIndex) {
      if (!c || c.dir !== dirs[i]) sendDepartures(i, true);
    } else if (c && c.dir !== dirs[i]) {
      fetchDepartures(i);
    }
  });
}

function quayCode(place) {
  return (place && place.quay && place.quay.publicCode) || '';
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
        out.push({
          label: j.label || (a.name + '–' + b.name),
          big: !!j.big,
          bike: !!j.bike,
          a: a,
          b: b
        });
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
    localStorage.setItem('bikeLines', JSON.stringify(bikeLines));
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
    var bl = localStorage.getItem('bikeLines');
    if (bl) bikeLines = JSON.parse(bl);
    var l = localStorage.getItem('lastLoc');
    if (l) lastLoc = JSON.parse(l);
    lastLocTs = parseInt(localStorage.getItem('lastLocTs'), 10) || 0;
  } catch (e) { resolved = null; }
}

// Make sure `resolved` is populated (from storage, else seed defaults).
// Callers can overlap on launch — the menu send and the prefetch both need it —
// so a resolve already in progress collects waiters instead of starting again.
var resolveWaiters = null;

function ensureResolved(cb) {
  if (resolved) { cb(); return; }
  loadResolved();
  if (resolved) { cb(); return; }
  if (resolveWaiters) { resolveWaiters.push(cb); return; }
  resolveWaiters = [cb];
  resolveJourneys(DEFAULT_JOURNEYS, function (out) {
    resolved = out;
    saveResolved();
    var ws = resolveWaiters;
    resolveWaiters = null;
    ws.forEach(function (w) { w(); });
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

function sendToWatch(dict, isRetry) {
  Pebble.sendAppMessage(dict,
    function () {},
    function () {
      console.log('sendAppMessage failed' + (isRetry ? ' (giving up)' : ', retrying'));
      if (!isRetry) setTimeout(function () { sendToWatch(dict, true); }, 400);
    });
}

function sendError(index, msg) {
  sendToWatch({ MSG_TYPE: 2, JOURNEY_INDEX: index, PAYLOAD: msg });
}

// Build + send the journeys list (with current auto-direction) to the watch.
function sendMenu() {
  ensureResolved(function () {
    getLocation(function (loc) {
      sentMenuDirs = resolved.map(function (j) { return pickDirection(j, loc).origin.id; });
      var rows = resolved.map(function (j) {
        var d = pickDirection(j, loc);
        // The watch font has no two-way arrow, so say it in words.
        var arrow = d.known ? '→ ' : 'No GPS → ';
        return j.label + '\t' + arrow + shortName(d.dest.name) +
               '\t' + (j.big ? '1' : '0');
      });
      sendToWatch({
        MSG_TYPE: 0,
        COUNT: resolved.length,
        PAYLOAD: rows.join('\n')
      });
    });
  });
}

// Render a cached board into the watch payload. Times are stored as ISO, so the
// countdowns are recomputed on every send and a cached entry is never wrong —
// only progressively less complete as new departures appear.
var MAX_PAYLOAD = 460;   // stays under the watch's negotiated inbox buffer

function renderBoard(entry, bike) {
  var lines = [entry.header];
  entry.rows.forEach(function (r) {
    lines.push(isoLocalHHMM(r.iso) + '\t' + r.code + '\t' + etaText(r.iso) +
               '\t' + r.track + '\t' + (bike && isBikeFriendly(r) ? '1' : '0'));
  });
  // Drop from the end rather than risk an oversized message being dropped
  // whole — a silently lost payload is indistinguishable from a hang.
  while (lines.length > 2 && lines.join('\n').length > MAX_PAYLOAD) lines.pop();
  return lines.join('\n');
}

// Drop departures that have already gone, so a cached board doesn't lead with
// a train you can no longer catch.
function pruneBoard(entry) {
  var now = Date.now();
  entry.rows = entry.rows.filter(function (r) {
    return new Date(r.iso).getTime() > now - 60000;
  });
  return entry;
}

function sendBoard(index, entry) {
  sendToWatch({
    MSG_TYPE: 1,
    JOURNEY_INDEX: index,
    PAYLOAD: renderBoard(entry, resolved && resolved[index] && resolved[index].bike)
  });
}

// Query Entur for one journey and cache the result. `onDone` is optional.
function fetchDepartures(index, onDone) {
  if (onDone) {
    if (!depWaiters[index]) depWaiters[index] = [];
    depWaiters[index].push(onDone);
  }
  if (depInflight[index]) return;   // in flight; its reply serves every waiter

  var j = resolved && resolved[index];
  if (!j) { settleDepartures(index, 'No such journey'); return; }

  depInflight[index] = true;
  var d = pickDirection(j, lastLoc);
  var q =
    '{trip(from:{place:"' + d.origin.id + '"} to:{place:"' + d.dest.id +
    '"} numTripPatterns:' + numDepartures + '){tripPatterns{' +
    'expectedStartTime legs{mode expectedStartTime line{publicCode} ' +
    'serviceJourney{privateCode} ' +
    'fromPlace{quay{publicCode}} toPlace{quay{publicCode}}}}}}';

  graphQL(q, function (data) {
    depInflight[index] = false;
    // The fix changed while this was in flight: the answer runs the wrong way.
    // Ask again; the waiters stay queued for the corrected board.
    if (resolved[index] === j && currentDir(index) !== d.origin.id) {
      fetchDepartures(index);
      return;
    }
    var patterns = (data && data.trip && data.trip.tripPatterns) || [];
    var entry = {
      ts: Date.now(),
      dir: d.origin.id,
      header: shortName(d.origin.name) + ' → ' + shortName(d.dest.name),
      rows: []
    };
    patterns.forEach(function (p) {
      // First non-walking leg carries the real departure + line; the last one
      // carries the arrival platform at the destination.
      var leg = null, lastLeg = null;
      for (var k = 0; k < p.legs.length; k++) {
        if (p.legs[k].mode === 'foot') continue;
        if (!leg) leg = p.legs[k];
        lastLeg = p.legs[k];
      }
      var depQuay = leg ? quayCode(leg.fromPlace) : '';
      var arrQuay = lastLeg ? quayCode(lastLeg.toPlace) : '';
      var railish = leg && (leg.mode === 'rail' || leg.mode === 'metro');
      // "Spor 3 > 18": board at 3, arrive at 18. Knowing the arrival platform
      // is what lets you prefer one train over another.
      var track = depQuay ? (railish ? 'Spor ' : 'Pl. ') + depQuay : '';
      if (arrQuay) track = track ? track + ' > ' + arrQuay : 'Ank. ' + arrQuay;

      entry.rows.push({
        iso: (leg && leg.expectedStartTime) || p.expectedStartTime,
        code: (leg && leg.line && leg.line.publicCode) || '',
        train: (leg && leg.serviceJourney && leg.serviceJourney.privateCode) || '',
        track: track
      });
    });
    depCache[index] = entry;
    settleDepartures(index, null, entry);
  }, function (err) {
    depInflight[index] = false;
    settleDepartures(index, err);
  });
}

// Hand one fetch result to everyone waiting on it.
function settleDepartures(index, err, entry) {
  var ws = depWaiters[index] || [];
  depWaiters[index] = [];
  ws.forEach(function (w) { w(err, entry); });
}

// Answer the watch for one journey. A usable cached board goes out immediately
// and a refresh is pushed after it; only a cold cache has to wait for the
// network. The watch replaces the board whenever a newer one arrives.
// `fresh` skips the cache (manual refresh, or the direction just changed).
function sendDepartures(index, fresh) {
  openIndex = index;
  ensureResolved(function () {
    if (!resolved[index]) { sendError(index, 'No such journey'); return; }
    getTrustedLocation(function () { sendDeparturesNow(index, fresh); });
  });
}

function sendDeparturesNow(index, fresh) {
  if (!resolved[index]) return;
  // A cached board is only reusable if it runs the way we're travelling now —
  // a GPS fix may have flipped the direction since it was built.
  var wantDir = currentDir(index);
  var cached = depCache[index];
  var served = false;
  if (!fresh && cached && cached.dir === wantDir &&
        (Date.now() - cached.ts) < DEP_SERVE_STALE_MS) {
    pruneBoard(cached);
    if (cached.rows.length) { sendBoard(index, cached); served = true; }
  }

  fetchDepartures(index, function (err, entry) {
    if (err) { if (!served) sendError(index, err); return; }
    if (entry.rows.length || !served) sendBoard(index, entry);
  });
}

// Long-press SELECT on the watch: get a brand-new fix, then resend the menu and,
// when a board is open (index >= 0), that board fetched from scratch.
function manualRefresh(index) {
  openIndex = index;
  ensureResolved(function () {
    refreshLocation(function () {
      sendMenu();
      if (index >= 0) sendDeparturesNow(index, true);
    }, true);
  });
}

// Warm the cache for every journey as soon as the app opens — the phone-side JS
// only runs while the watchapp is in the foreground, so this is exactly the
// moment before the user picks one.
function prefetchAll() {
  if (!resolved) return;
  resolved.forEach(function (j, i) { fetchDepartures(i); });
}

// ---------------------------------------------------------------------------
// Clay settings -> raw journeys
// ---------------------------------------------------------------------------

// Clay toggles arrive as true/false (or "true"/"false" once round-tripped).
function boolVal(settings, key) {
  var v = settings[key];
  if (v && typeof v === 'object' && 'value' in v) v = v.value;
  return v === true || v === 'true' || v === 1 || v === '1';
}

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
  // Present-but-empty clears the list; absent (older saved settings) keeps it.
  if (settings.BIKE_LINES != null) bikeLines = parseBikeLines(val(settings, 'BIKE_LINES'));

  var raw = [];
  for (var i = 1; i <= 4; i++) {
    raw.push({
      label: val(settings, 'J' + i + '_LABEL'),
      from:  val(settings, 'J' + i + '_FROM'),
      to:    val(settings, 'J' + i + '_TO'),
      big:   boolVal(settings, 'J' + i + '_BIG'),
      bike:  boolVal(settings, 'J' + i + '_BIKE')
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
  ensureResolved(prefetchAll);   // ...and warm every board behind it
});

Pebble.addEventListener('appmessage', function (e) {
  var p = e.payload || {};
  if (!('REQUEST' in p)) return;
  if (p.REQUEST === 1) {
    sendMenu();
  } else if (p.REQUEST === 2) {
    sendDepartures(p.JOURNEY_INDEX || 0);
  } else if (p.REQUEST === 3) {
    manualRefresh('JOURNEY_INDEX' in p ? p.JOURNEY_INDEX : -1);
  } else if (p.REQUEST === 4) {
    sendDepartures(p.JOURNEY_INDEX || 0, true);
  }
});

Pebble.addEventListener('showConfiguration', function () {
  Pebble.openURL(clay.generateUrl());
});

Pebble.addEventListener('webviewclosed', function (e) {
  if (!e || !e.response) return;
  var settings = clay.getSettings(e.response, false);
  var raw = applySettings(settings);
  depCache = {};
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
