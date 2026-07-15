# ysf2dmrcon

Standalone **YSF reflector ↔ DMR server** voice bridge. Registers as a Homebrew
DMR peer (MMDVMHost-style login) and as a YSF client (YSFP + DGID room).

Self-contained build — vendored code under `hbp/`, `mmdvm/`, and `vendor/`.
Upstream reference: [MMDVM_CM](https://github.com/juribeparada/MMDVM_CM).

**Spanish documentation:** [README.es.md](README.es.md)

## Build

```bash
cd /opt/ysf2dmr
make
```

Produces `ysf2dmrcon`. Optional install:

```bash
make install PREFIX=/usr/local
```

## Quick start

1. Copy the template and edit your settings:

   ```bash
   cp ysf2dmrcon.example.ini ysf2dmrcon.ini
   ```

2. Set YSF reflector, DGID, DMR server, password, and talkgroup in `OPTIONS`.

3. Run from the directory that contains the INI (or pass `-c`):

   ```bash
   ./ysf2dmrcon
   # or
   ./ysf2dmrcon -c /path/to/ysf2dmrcon.ini
   ```

4. For production, set `[log] level = INFO` (or `WARNING`).

## Multiple instances

Run **one process per bridge** (different DGID, DMR TG, and `dmrid`). Each
instance needs its own INI file:

```bash
./ysf2dmrcon -c ysf2dmrcon-tg71442.ini
./ysf2dmrcon -c ysf2dmrcon-tg71481.ini
```

Instances may share one subscriber database: set the same `[aliases] data_dir`
in each INI (default `./data`).

## Configuration (`ysf2dmrcon.ini`)

### `[ysf]` — YSF reflector

| Key | Description |
|-----|-------------|
| `host` | YSF reflector hostname or IP |
| `port` | YSF UDP port (commonly `42000`) |
| `dgid` | DGID room to join on connect/reconnect |
| `radio_id` | Yaesu model label for **DMR→YSF** CSD/DCH (see below) |
| `radio_model` | Alias for `radio_id` (same meaning) |

#### YSF radio model (`radio_id` / `radio_model`)

On **DMR→YSF**, bytes 5–9 of the YSF CSD/DCH “Radio ID” field are filled from
this setting (the talker **callsign** comes from `subscriber_ids.json`, not
from here). Use the model that best matches what YSF listeners expect to see
for bridged DMR traffic.

**INI keys:** `radio_id` or `radio_model` (equivalent).

**Resolution order:**

1. Match a **known name** below (case-insensitive) → use the fixed 5-character wire code.
2. Value **≤ 5 characters** → copied as-is (space-padded to 5 bytes).
3. **Longer name** → remove `-` and spaces, take the first 5 letters/digits, uppercased.
4. Empty or `*****` → default `FT-5D`.

| INI name(s) | Wire code (5 bytes) | Typical Yaesu radio |
|-------------|---------------------|---------------------|
| `FT-70D` | `FT-70` | FT-70D |
| `FT-3D` | `FT-3D` | FT3D |
| `FT-991` | `FT991` | FT-991 |
| `FT-1XD` | `FT-1X` | FT1XD |
| `FT-2D` | `FT-2D` | FT2D |
| `FT-5D` | `FT-5D` | FT5D (**default**) |
| `FT7250` | `FT725` | FT-7250 |
| `FT3207` | `FT320` | FT3D family |
| `FTM100`, `FTM-100` | `FTM10` | FTM-100D |
| `FTM200`, `FTM-200` | `FTM20` | FTM-200 |
| `FTM300`, `FTM-300` | `FTM30` | FTM-300D |
| `FTM310`, `FTM-310` | `FTM31` | FTM-310 |
| `FTM3200`, `FTM-3200` | `FTM32` | FTM-3200D |
| `FTM400`, `FTM-400` | `FTM40` | FTM-400D / FTM-400X |
| `FTM500`, `FTM-500` | `FTM50` | FTM-500D |

Example:

```ini
[ysf]
radio_id = FT-70D
# same as: radio_model = FT-70D
```

**YSF→DMR (incoming):** many handhelds append a suffix after `-` or `/` in the
wire source field (e.g. `HP3ICC-FT3`, `CE5RPY/FT3`). The bridge strips that
suffix and uses only the base callsign for JSON lookup. The suffix is **not**
required to match `radio_id` above.

### `[dmr]` — Homebrew peer identity

| Key | Description |
|-----|-------------|
| `callsign` | Bridge callsign (monitor “Bridges”, RPTC identity) |
| `dmrid` | Bridge DMR ID — must be unique per instance |
| `location` | Shown on monitor Linked Systems (max 20 chars) |
| `description` | Short bridge label (max 19 chars) |
| `host` / `port` | ADN DMR server |
| `options` | RPTO string, e.g. `TS2=7302;SINGLE=0;TIMER=60;` — voice TG is parsed from `TS1=` / `TS2=` |
| `password` | Homebrew peer password |

**Monitor note:** RX/TX frequency zero in RPTC → monitor shows N/A frequencies
(correct for a software bridge, not a hotspot).

`default_ysf_dmrid` is accepted for backward compatibility but **not used**;
unknown YSF talkers fall back to this section’s `callsign` + `dmrid`.

### `[aliases]` — Subscriber database

Same layout as `ALIASES` in new-adn-server. Files live under `data_dir`
(default `./data`).

| Key | Description |
|-----|-------------|
| `try_download` | `1` = download on start if missing or stale |
| `stale_minutes` | Re-download when file age exceeds this (`0` = always on start) |
| `data_dir` | Directory for JSON files (default `./data`) |
| `subscriber_file` / `subscriber_url` | Main ID ↔ callsign database |
| `local_subscriber_file` | Optional local overlay |
| `checksum_file` / `checksum_url` | Optional checksum manifest |

### `[log]`

`level = DEBUG | INFO | WARNING | ERROR`

## Talker identity

Voice always crosses; only the displayed/transmitted identity changes.

### DMR → YSF

- Source: **DMR radio ID → callsign** from `subscriber_ids.json` only.
- DMRA (talker alias) is decoded and logged at DEBUG only — **never** used for YSF source (users may set arbitrary text).
- If the ID is missing from the JSON, the numeric ID is sent (many YSF radios do not display it as a callsign).

### YSF → DMR

1. Strip suffix after the first `-` or `/` (`HP3ICC-FT3` → `HP3ICC`).
2. If the base callsign exists in JSON → use that callsign and the **first**
   DMR ID listed for that callsign in the file.
3. If unknown → use bridge identity from `[dmr]` (`callsign` + `dmrid`).

## Project layout

| Path | Role |
|------|------|
| `ysf2dmrcon.c` | Main loop, signals, config |
| `peer_dmr.c` / `peer_ysf.c` | UDP peers, reconnect, DGID/RPTO |
| `bridge.c` | Voice bridge, identity, ModeConv pacing |
| `aliases.c` | JSON alias load/download/lookup |
| `talker_alias.c` | DMRA decode (log only) |
| `ysf_fich.c` | YSF FICH codec + DGID rewrite |
| `hbp/dmr_hbp.c` | DMR HBP auth + LC/embedded codec |
| `mmdvm/` | ModeConv + Golay24128 (MMDVM_CM YSF2DMR) |
| `vendor/yyjson/` | JSON parser (MIT) |

## License

**GPL v3** (or later). See [LICENSE](LICENSE).

| Component | License | Provenance |
|-----------|---------|------------|
| Application (`ysf2dmrcon`, peers, bridge, config, aliases, wrappers) | GPL-3.0-or-later | Copyright (C) 2026 Rodrigo Pérez, CE5RPY |
| `hbp/` | GPL-3.0-or-later | Doug McLain; Esteban Mackay HP3ICC; MMDVM_CM |
| `ysf_fich.c` | GPL-3.0-or-later | Doug McLain; Esteban Mackay HP3ICC |
| `mmdvm/` | GPL-2.0-or-later | Jonathan Naylor G4KLX; Andy Uribe CA6JAU; [MMDVM_CM](https://github.com/juribeparada/MMDVM_CM) |
| `vendor/yyjson/` | MIT | YaoYuan, [yyjson](https://github.com/ibireme/yyjson) |
