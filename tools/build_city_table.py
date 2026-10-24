#!/usr/bin/env python3
"""Audit searchable cities and generate the compact firmware city table."""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import sys
import urllib.parse
import urllib.request
from pathlib import Path

HOSTS = (
    "http://de1.api.radio-browser.info",
    "http://de2.api.radio-browser.info",
)
SOURCE_FIELDS = ("initials", "pinyin", "city", "province", "priority")
AUDIT_FIELDS = (
    "city",
    "province",
    "available",
    "station_count",
    "checked_at",
    "source",
)


def read_cities(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as source:
        rows = list(csv.DictReader(source))
    if not rows:
        raise SystemExit(f"{path}: no cities")
    if tuple(rows[0].keys()) != SOURCE_FIELDS:
        raise SystemExit(
            f"{path}: expected columns {','.join(SOURCE_FIELDS)}"
        )
    seen: set[tuple[str, str]] = set()
    for line, row in enumerate(rows, start=2):
        key = (row["city"].strip(), row["province"].strip())
        if not all(row[field].strip() for field in SOURCE_FIELDS):
            raise SystemExit(f"{path}:{line}: empty required field")
        if key in seen:
            raise SystemExit(f"{path}:{line}: duplicate city/province {key}")
        seen.add(key)
    return rows


def fetch_stations(retries: int = 2) -> list[dict[str, object]]:
    parameters = urllib.parse.urlencode(
        {"countrycode": "CN", "hidebroken": "true", "limit": 100000}
    )
    last_error: Exception | None = None
    for _ in range(retries):
        for host in HOSTS:
            url = f"{host}/json/stations/search?{parameters}"
            request = urllib.request.Request(
                url,
                headers={
                    "Accept": "application/json",
                    "User-Agent": "Octo-Radio-City-Audit/1.0",
                },
            )
            try:
                with urllib.request.urlopen(request, timeout=30) as response:
                    stations = json.load(response)
                if not isinstance(stations, list):
                    raise ValueError("response is not a JSON array")
                return stations
            except (OSError, ValueError, json.JSONDecodeError) as error:
                last_error = error
    raise SystemExit(f"Radio Browser audit failed: {last_error}")


def app_playable(station: dict[str, object]) -> bool:
    url = str(station.get("url_resolved") or station.get("url") or "")
    if ".mp3" not in url.casefold():
        return False
    if str(station.get("codec") or "").casefold() != "mp3":
        return False
    last_check = station.get("lastcheckok")
    if isinstance(last_check, (int, float)) and last_check != 1:
        return False
    hls = station.get("hls")
    if isinstance(hls, (int, float)) and hls != 0:
        return False
    return True


def audit_cities(
    cities: list[dict[str, str]], stations: list[dict[str, object]]
) -> list[dict[str, object]]:
    usable = [station for station in stations if app_playable(station)]
    checked_at = dt.date.today().isoformat()
    result: list[dict[str, object]] = []
    for city in cities:
        name = city["city"].casefold()
        matches = [
            station
            for station in usable
            if name in str(station.get("name") or "").casefold()
        ]
        result.append(
            {
                "city": city["city"],
                "province": city["province"],
                "available": 1 if matches else 0,
                "station_count": len(matches),
                "checked_at": checked_at,
                "source": "radio-browser playlist-match",
            }
        )
    return result


def write_audit(path: Path, rows: list[dict[str, object]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as target:
        writer = csv.DictWriter(target, fieldnames=AUDIT_FIELDS)
        writer.writeheader()
        writer.writerows(rows)


def read_audit(
    path: Path, cities: list[dict[str, str]]
) -> list[dict[str, object]]:
    with path.open(newline="", encoding="utf-8") as source:
        rows = list(csv.DictReader(source))
    if tuple(rows[0].keys() if rows else ()) != AUDIT_FIELDS:
        raise SystemExit(f"{path}: invalid audit columns")
    indexed: dict[tuple[str, str], dict[str, object]] = {}
    for row in rows:
        key = (row["city"], row["province"])
        if key in indexed:
            raise SystemExit(f"{path}: duplicate audit row {key}")
        indexed[key] = row
    expected = {(row["city"], row["province"]) for row in cities}
    actual = set(indexed)
    if actual != expected:
        missing = sorted(expected - actual)
        extra = sorted(actual - expected)
        raise SystemExit(
            f"{path}: audit/source mismatch; missing={missing}, extra={extra}"
        )
    return rows


def cpp_string(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"')


def write_header(
    path: Path,
    cities: list[dict[str, str]],
    audit: list[dict[str, object]],
    audit_date: str,
) -> int:
    available = {
        (str(row["city"]), str(row["province"]))
        for row in audit
        if str(row["available"]) == "1" and int(row["station_count"]) > 0
    }
    selected = [
        city
        for city in cities
        if (city["city"], city["province"]) in available
    ]
    lines = [
        "// Generated by tools/build_city_table.py. Do not edit.",
        f"// Radio Browser audit: {audit_date}.",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "struct RadioCityRecord {",
        "    const char *initials;",
        "    const char *pinyin;",
        "    const char *name;",
        "    const char *province;",
        "    uint8_t priority;",
        "};",
        "",
        "inline constexpr RadioCityRecord kRadioCities[] = {",
    ]
    for city in selected:
        lines.append(
            '    {"%s", "%s", "%s", "%s", %s},'
            % (
                cpp_string(city["initials"]),
                cpp_string(city["pinyin"]),
                cpp_string(city["city"]),
                cpp_string(city["province"]),
                city["priority"],
            )
        )
    lines.extend(
        [
            "};",
            "",
            f"inline constexpr unsigned kRadioCityCount = {len(selected)};",
            "",
        ]
    )
    path.write_text("\n".join(lines), encoding="utf-8")
    return len(selected)


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source", default="data/radio_cities.csv", help="all administrative cities"
    )
    parser.add_argument(
        "--audit",
        default="data/radio_city_availability.csv",
        help="checked-in Radio Browser audit",
    )
    parser.add_argument(
        "--output", default="main/radio_cities.h", help="generated C++ header"
    )
    parser.add_argument(
        "--refresh",
        action="store_true",
        help="query Radio Browser and rewrite the audit file",
    )
    args = parser.parse_args()

    source_path = root / args.source
    audit_path = root / args.audit
    output_path = root / args.output
    cities = read_cities(source_path)
    if args.refresh:
        stations = fetch_stations()
        audit = audit_cities(cities, stations)
        write_audit(audit_path, audit)
        checked = {str(row["checked_at"]) for row in audit}
        print(
            f"audited {len(cities)} cities against {len(stations)} Radio Browser "
            f"stations ({', '.join(sorted(checked))})"
        )
    else:
        audit = read_audit(audit_path, cities)

    available = sum(
        1
        for row in audit
        if str(row["available"]) == "1" and int(row["station_count"]) > 0
    )
    removed = len(cities) - available
    audit_date = max(str(row["checked_at"]) for row in audit)
    written = write_header(output_path, cities, audit, audit_date)
    if written != available:
        raise SystemExit("internal error: generated city count mismatch")
    print(
        f"Generated {output_path} with {available} searchable cities "
        f"({removed} without playable Radio Browser results)"
    )


if __name__ == "__main__":
    main()
