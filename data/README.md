# Runtime Data

## City Search Data

`radio_cities.csv` is the complete source list of 337 Chinese prefecture-level
divisions. `radio_city_availability.csv` is the checked-in Radio Browser audit;
it records every source city and its number of playable MP3 matches.

The firmware only embeds cities with at least one match. Refresh the audit and
regenerate the compact table with:

```bash
python3 tools/build_city_table.py --refresh
```

The audit requires network access and uses the same filters as the station
directory. The current result retains 203 cities and excludes 134 that have no
playable Radio Browser result, including Baicheng.

## Offline FM Frequency Data

`fm_frequencies.csv` is the single source of truth for city-specific FM
frequencies. The device embeds a compact hash/index table generated from it; it
never parses the CSV at runtime.

## Columns

| Column | Description |
| --- | --- |
| `city` | Exact Chinese city name from `main/radio_cities.h` |
| `station` | Directory station name or a stable alias |
| `fm_mhz` | FM frequency in MHz, from 87.0 through 108.0 |
| `confidence` | `verified`, `community`, or `declared` |
| `source` | Human-readable source description |
| `checked_at` | Last review date in `YYYY-MM-DD` format |

Rules:

- Add separate rows for aliases.
- Do not add a frequency inferred only from a marketing number such as `886`
  or `991`.
- Include the city when the same programme uses different transmitters.
- A conflicting duplicate key is rejected by the generator.
- Keep `source` and `checked_at` so entries can be audited later.

Regenerate and test:

```bash
python3 tools/build_frequency_table.py
IDF_PATH=/path/to/esp-idf ./tools/tests/run_tests.sh
```

The generated record stores only a 64-bit city/name hash and a 16-bit
frequency. Source metadata and human-readable aliases stay in the CSV.
The checked-in table currently contains 87 reviewed entries across 25 cities.

Online FM values are runtime metadata, not part of this source table. They are
stored in the separate `fmcache1` NVS namespace by Qingting channel ID, so
changing a display name does not invalidate the cache. Negative lookup results
are kept only in RAM for the current boot.
