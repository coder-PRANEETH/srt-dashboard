#!/usr/bin/env python3
"""
Cache map tiles around SASTRA Deemed University for offline use.

The dashboard runs with no internet in the vehicle, so every tile Leaflet may
ask for has to be on disk before we leave. Tiles are written as

    public/tiles/{z}/{x}/{y}.png

which is exactly the layout the Leaflet tileLayer URL in App.jsx expects.

YOU MUST SUPPLY A TILE SOURCE (--url) THAT PERMITS OFFLINE CACHING.

There is deliberately no default. tile.openstreetmap.org CANNOT be used: the
OSMF tile policy forbids "pre-emptive fetching of tiles other than those a
user is actively viewing" and states plainly that "offline use is not
permitted". Their servers enforce it by returning an HTTP 200 whose body is a
403 "Access blocked" PNG, so a naive downloader fills the disk with identical
placeholder images that only show up when the map is already in the car.
The same reasoning rules out other volunteer-run services (OpenTopoMap etc.).

Legitimate sources, in rough order of effort:

  1. A provider whose terms allow caching, using your own API key. Stadia
     Maps, Thunderforest, MapTiler and Jawg all have free tiers - check the
     current terms for offline/caching rights before pointing this at them.
       --url 'https://tile.example.com/{z}/{x}/{y}.png?key=YOUR_KEY'

  2. Geofabrik sell ready-made tile packages (ZIP/MBTiles) for a region.
     No download needed - unpack straight into public/tiles.

  3. Self-host from the India extract (download.geofabrik.de/asia/india.html)
     with switch2osm.org's guide, then point this script at localhost.
       --url 'http://localhost:8080/styles/basic/{z}/{x}/{y}.png'

Usage:
    python3 tools/download-tiles.py --url '...' --dry-run
    python3 tools/download-tiles.py --url '...'              # 2 km, z13-17
    python3 tools/download-tiles.py --url '...' --radius 5   # wider area
"""

import argparse
import hashlib
import math
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

# Main campus, Thirumalaisamudram. Verified against OpenStreetMap - note that
# Wikipedia's "78.68 E" for Thirumalaisamudram is wrong by ~37 km.
CAMPUS_LAT = 10.7280
CAMPUS_LON = 79.0195

# Hosts known to prohibit offline/bulk caching. Refused outright so this
# script cannot quietly fill the disk with "Access blocked" placeholders.
BLOCKED_HOSTS = (
    'tile.openstreetmap.org',
    'tile.osm.org',
    'a.tile.openstreetmap.org',
    'b.tile.openstreetmap.org',
    'c.tile.openstreetmap.org',
    'tile.opentopomap.org',
)

USER_AGENT = ('srt-dashboard-offline-tiles/1.0 '
              '(student project; contact: praneeth.m1000@gmail.com)')
REQUEST_DELAY = 0.12


def deg2tile(lat, lon, zoom):
    """Slippy-map tile x/y containing the given coordinate."""
    n = 2 ** zoom
    lat_rad = math.radians(lat)
    x = int((lon + 180.0) / 360.0 * n)
    y = int((1.0 - math.asinh(math.tan(lat_rad)) / math.pi) / 2.0 * n)
    return x, y


def bounding_box(lat, lon, radius_km):
    """Square bounding box covering radius_km around a point."""
    dlat = radius_km / 111.32
    dlon = radius_km / (111.32 * math.cos(math.radians(lat)))
    return lat + dlat, lat - dlat, lon - dlon, lon + dlon


def tile_list(lat, lon, radius_km, min_zoom, max_zoom):
    north, south, west, east = bounding_box(lat, lon, radius_km)
    tiles = []
    for zoom in range(min_zoom, max_zoom + 1):
        x_left, y_top = deg2tile(north, west, zoom)
        x_right, y_bottom = deg2tile(south, east, zoom)
        for x in range(min(x_left, x_right), max(x_left, x_right) + 1):
            for y in range(min(y_top, y_bottom), max(y_top, y_bottom) + 1):
                tiles.append((zoom, x, y))
    return tiles


def looks_like_png(data):
    """A real tile is a PNG. Blocked/error responses often arrive as HTTP 200
    carrying HTML or a placeholder image, so check the magic bytes."""
    return bool(data) and data[:8] == b'\x89PNG\r\n\x1a\n'


def fetch(url, retries=3):
    request = urllib.request.Request(url, headers={'User-Agent': USER_AGENT})
    for attempt in range(retries):
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                return response.read()
        except urllib.error.HTTPError as error:
            if error.code == 404:
                return None
            # 401/403 mean the key or the plan is wrong. Retrying cannot fix
            # that, and these services answer with a *valid* PNG saying so -
            # fail loudly instead of caching hundreds of error images.
            if error.code in (401, 403):
                raise SystemExit(
                    f'\nHTTP {error.code} from the tile server.\n'
                    'The API key is missing, wrong, or not entitled to these '
                    'tiles.\nNothing was written. Check the key and the style '
                    'name in your --url.'
                )
            if attempt == retries - 1:
                raise
            time.sleep(2 ** attempt)
        except urllib.error.URLError:
            if attempt == retries - 1:
                raise
            time.sleep(2 ** attempt)
    return None


def main():
    parser = argparse.ArgumentParser(
        description='Download offline OSM tiles around SASTRA.'
    )
    parser.add_argument('--lat', type=float, default=CAMPUS_LAT)
    parser.add_argument('--lon', type=float, default=CAMPUS_LON)
    parser.add_argument('--radius', type=float, default=2.0,
                        help='radius in km (default: 2)')
    parser.add_argument('--min-zoom', type=int, default=13)
    parser.add_argument('--max-zoom', type=int, default=17)
    parser.add_argument('--url', required=True,
                        help='tile URL template with {z}/{x}/{y}; must be a '
                             'source whose terms permit offline caching')
    parser.add_argument('--out', default=None,
                        help='output dir (default: public/tiles)')
    parser.add_argument('--dry-run', action='store_true',
                        help='only report how many tiles would be fetched')
    args = parser.parse_args()

    out_dir = args.out or os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        'public', 'tiles',
    )

    host = urllib.parse.urlparse(args.url).hostname or ''

    if host.lower() in BLOCKED_HOSTS:
        print(
            f'\nRefusing to bulk-download from {host}.\n\n'
            'That service prohibits offline/pre-emptive tile caching and '
            'enforces it by returning\n'
            'an HTTP 200 whose body is a 403 "Access blocked" image - you '
            'would end up with a\n'
            'directory of identical placeholders and a blank map in the '
            'vehicle.\n\n'
            'Use a provider whose terms allow caching (see this file\'s '
            'docstring).',
            file=sys.stderr,
        )
        return 1

    tiles = tile_list(args.lat, args.lon, args.radius,
                      args.min_zoom, args.max_zoom)

    print(f'Source     : {args.url}')
    print(f'Centre     : {args.lat}, {args.lon}')
    print(f'Radius     : {args.radius} km')
    print(f'Zoom       : {args.min_zoom}-{args.max_zoom}')
    print(f'Tiles      : {len(tiles):,}  (~{len(tiles) * 18 / 1024:.1f} MB)')
    print(f'Output     : {out_dir}')

    if args.dry_run:
        return 0

    if len(tiles) > 20000:
        print('\nRefusing: >20000 tiles is abusive to the public OSM tile '
              'servers. Reduce --radius or --max-zoom.', file=sys.stderr)
        return 1

    downloaded = skipped = failed = 0

    # Providers often answer an auth/quota failure with HTTP 200 and a valid
    # PNG that reads "unauthorized". Those are byte-identical across every
    # coordinate, so a repeated digest is a useful signal.
    #
    # But blank tiles repeat legitimately: open farmland around Thanjavur has
    # no mapped features, and a style renders every such tile as the same flat
    # background. Those are tiny (a few hundred bytes), whereas an error image
    # carries rendered text and is far larger. Only treat repeats above that
    # size as junk.
    BLANK_TILE_MAX_BYTES = 2048
    seen_digests = {}

    for index, (zoom, x, y) in enumerate(tiles, start=1):
        path = os.path.join(out_dir, str(zoom), str(x), f'{y}.png')

        if os.path.exists(path) and os.path.getsize(path) > 0:
            skipped += 1
            continue

        os.makedirs(os.path.dirname(path), exist_ok=True)
        url = (args.url.replace('{z}', str(zoom))
               .replace('{x}', str(x))
               .replace('{y}', str(y)))

        try:
            data = fetch(url)
        except Exception as error:                      # noqa: BLE001
            print(f'\n  failed {zoom}/{x}/{y}: {error}', file=sys.stderr)
            failed += 1
            continue

        if data is None:
            failed += 1
            continue

        if len(data) > BLANK_TILE_MAX_BYTES:
            digest = hashlib.md5(data).hexdigest()
            seen_digests.setdefault(digest, 0)
            seen_digests[digest] += 1

            if seen_digests[digest] == 4:
                print(f'\n  Aborting: four non-blank tiles at different '
                      'coordinates came back\n  byte-identical. That is an '
                      'error/placeholder image (bad API key,\n  quota, or '
                      'wrong style), not map data. '
                      f'{downloaded} tile(s) written so far -\n  delete '
                      f'{out_dir} before retrying.',
                      file=sys.stderr)
                return 1

        if not looks_like_png(data):
            print('\n  Aborting: the server returned a non-PNG response for '
                  f'{zoom}/{x}/{y}.\n  This usually means the source is '
                  'refusing bulk downloads. Nothing further was written.',
                  file=sys.stderr)
            return 1

        with open(path, 'wb') as handle:
            handle.write(data)

        downloaded += 1
        time.sleep(REQUEST_DELAY)

        if index % 25 == 0 or index == len(tiles):
            print(f'\r  {index}/{len(tiles)} '
                  f'({downloaded} new, {skipped} cached, {failed} failed)',
                  end='', flush=True)

    print(f'\nDone: {downloaded} downloaded, {skipped} cached, '
          f'{failed} failed.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
