# ysf2dmrcon

**Version 0.1.0**

Standalone **YSF reflector ↔ DMR server** voice bridge. Registers as a Homebrew
DMR peer (MMDVMHost-style login) and as a YSF client (YSFP + DGID room).

Self-contained build — vendored code under `hbp/`, `mmdvm/`, and `vendor/`.
Upstream reference: [MMDVM_CM](https://github.com/juribeparada/MMDVM_CM).

**Documentación en español:** [README.es.md](README.es.md)

## Build

### Dependencies (Debian / Ubuntu)

```bash
sudo apt-get update
sudo apt-get install -y build-essential libssl-dev curl
```

| Package | Why |
|---------|-----|
| `build-essential` | `gcc`, `g++`, `make` |
| `libssl-dev` | OpenSSL (`-lcrypto`) for blake2b alias checksums |
| `curl` | Runtime download of subscriber / checksum JSON |

yyjson and MMDVM ModeConv sources are vendored; no extra apt packages for those.

### Compile

```bash
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

**DMR→YSF RadioID** in CSD/DCH is hardcoded to `*****` (DMR2YSF/YSF2DMR
default). It is not configurable.

**YSF→DMR (incoming):** many handhelds append a suffix after `-` or `/` in the
wire source field (e.g. `HP3ICC-FT3`, `CE5RPY/FT3`). The bridge strips that
suffix and uses only the base callsign for JSON lookup.

### `[dmr]` — Homebrew peer identity

| Key | Description |
|-----|-------------|
| `callsign` | Bridge callsign (monitor “Bridges”, RPTC identity) |
| `dmrid` | Bridge DMR ID — must be unique per instance |
| `location` | Shown on monitor Linked Systems (max 20 chars) |
| `description` | Short bridge label (max 19 chars) |
| `host` / `port` | ADN DMR server |
| `tg` | Mandatory voice talkgroup (DMRD + 1s connect PTT); TX always TS2 |
| `options` | Optional RPTO string — omit/empty = no RPTO; if set, sent as-is |
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
| `stale_minutes` | Re-download when file age exceeds this; also checked while running (default `1440` = 24 h; `0` = always on start only) |
| `reload_minutes` | How often to check if on-disk JSON is newer than RAM and rebuild; missing file forces download (`0` = off; default `15`) |
| `data_dir` | Directory for JSON files (default `./data`) |
| `subscriber_file` / `subscriber_url` | Main ID ↔ callsign database |
| `local_subscriber_file` | Optional local overlay |
| `checksum_file` / `checksum_url` | Optional; if absent, a valid subscriber JSON is accepted |

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
   DMR ID listed for that callsign in the file. In-memory index is two
   contiguous open-addressing tables (`id→callsign` and `callsign→primary id`);
   every ID is kept so DMR→YSF resolves e.g. both `7300391` and `7300392` to
   `CE5RPY`.
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
