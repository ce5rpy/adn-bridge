# ysf2dmrcon

**Version 0.2.1**

Standalone voice bridge with three modes:

| Mode | Role |
|------|------|
| `ysf-dmr` (default) | YSF reflector ↔ DMR server (Homebrew peer + YSFP/DGID) |
| `echolink-dmr` | EchoLink ↔ DMR (GSM/RTP + remote AMBE vocoder) |
| `echolink-ysf` | EchoLink ↔ YSF (same EchoLink + vocoder path) |

Self-contained build — vendored code under `hbp/`, `mmdvm/`, and `vendor/`.
Upstream reference: [MMDVM_CM](https://github.com/juribeparada/MMDVM_CM).
EchoLink details: [docs/echolink-bridge.md](docs/echolink-bridge.md).

**Documentación en español:** [README.es.md](README.es.md)

## Build

### Dependencies (Debian / Ubuntu)

```bash
sudo apt-get update
sudo apt-get install -y build-essential libssl-dev curl libgsm1
```

| Package | Why |
|---------|-----|
| `build-essential` | `gcc`, `g++`, `make` |
| `libssl-dev` | OpenSSL (`-lcrypto`) for blake2b alias checksums |
| `curl` | Runtime download of subscriber / checksum JSON |
| `libgsm1` | EchoLink GSM audio (`libgsm.so.1`); headers under `vendor/gsm/` |

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

### YSF ↔ DMR (default)

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

### EchoLink modes

Requires a remote AMBE vocoder (DV3000 / AMBEServer UDP, e.g. md380-emu on
`127.0.0.1:2460`). Use a validated EchoLink `-L` / `-R` / conference callsign.

```bash
# EchoLink <-> DMR
cp ysf2dmrcon-echolink-dmr.example.ini ysf2dmrcon-echolink-dmr.ini
# edit passwords, bind_addr, DMR master, EchoLink host (node or *CONF*)
./ysf2dmrcon -c ysf2dmrcon-echolink-dmr.ini

# EchoLink <-> YSF
cp ysf2dmrcon-echolink-ysf.example.ini ysf2dmrcon-echolink-ysf.ini
# edit passwords, bind_addr, YSF reflector/DGID, EchoLink host
./ysf2dmrcon -c ysf2dmrcon-echolink-ysf.ini
```

Local `*.ini` files (with passwords) are gitignored; only `*.example.ini` is
committed. See [docs/echolink-bridge.md](docs/echolink-bridge.md).

## Multiple instances

Run **one process per bridge** (different DGID, DMR TG, and `dmrid`). Each
instance needs its own INI file:

```bash
./ysf2dmrcon -c ysf2dmrcon-tg71442.ini
./ysf2dmrcon -c ysf2dmrcon-tg71481.ini
```

Instances may share one subscriber database: set the same `[aliases] data_dir`
in each INI (default `./data`).

## Configuration

Mode is selected under `[bridge]`:

```ini
[bridge]
mode = ysf-dmr          ; default — YSF <-> DMR
mode = echolink-dmr     ; EchoLink <-> DMR
mode = echolink-ysf     ; EchoLink <-> YSF
```

Templates: `ysf2dmrcon.example.ini`, `ysf2dmrcon-echolink-dmr.example.ini`,
`ysf2dmrcon-echolink-ysf.example.ini`.

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

In **`echolink-ysf`** mode no DMR peer is opened: `host`/`port`/`password` are
unused (password may be a placeholder). `callsign` / `dmrid` still feed YSF
CSD/DCH RadioID. The YSF talker/gateway callsign on the wire is the **full**
`[echolink] callsign` (e.g. `CE5RPY-L`), using the same CSD/DCH layout as
DMR→YSF.

### `[echolink]` — EchoLink station (echolink-* modes)

Ports are **fixed in code** (do not put them in the INI): UDP **5198** RTP,
UDP **5199** RTCP, TCP **5200** directory.

| Key | Description |
|-----|-------------|
| `callsign` | Validated EchoLink station (`N0CALL-L`, `-R`, or `*CONF*`) |
| `password` | EchoLink directory password |
| `bind_addr` | Local IP to bind 5198/5199 |
| `host` | Node or conference **callsign** to connect (resolved via directory); dotted IPv4 still accepted for lab |
| `qth` / `email` | Optional directory metadata |
| `directory_servers` | Comma-separated directory hosts (defaults to the four `serverN.echolink.org`) |
| `login_interval` | Directory presence login period (default **360** s, tlb-compatible) |
| `station_list_interval` | Station-list refresh / peer IP update (default **600** s) |
| `log_level` | Optional channel level (`DEBUG`…`ERROR`); else inherits `[log]` |

Directory login/list runs on a **background thread** so TCP cannot stall audio.
EL→DMR/YSF starts on any inbound PCM (including key-down silence); hangtime
follows PCM presence (~700 ms). Full timing notes:
[docs/echolink-bridge.md](docs/echolink-bridge.md).

### `[vocoder]` — Remote AMBE (echolink-* modes)

| Key | Description |
|-----|-------------|
| `host` / `port` | DV3000 / AMBEServer UDP endpoint (lab: md380-emu `127.0.0.1:2460`) |
| `log_level` | Optional; prefer `WARNING` unless debugging encode/decode |

Uses RATET **34** (49-bit) with md380-emu `interleave49` on the wire; ModeConv
sees raw deinterleaved 49-bit frames.

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

Optional per-channel overrides (also accepted as `log_level=` under each
stanza, or as `dmr=` / `echolink=` / `ysf=` / `vocoder=` under `[log]`):

```text
2026-07-17 14:33:59,754 INFO/ysf: EL->YSF call start (src CE5RPY    )
```

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

### EchoLink → DMR / YSF

- Talker identity is the **local EchoLink** station base callsign
  (`[echolink] callsign`, strip after `-` / `/`), not the remote `host`.
- EL→YSF CSD/DCH framing matches DMR→YSF (DMR2YSF-compatible).

## Project layout

| Path | Role |
|------|------|
| `ysf2dmrcon.c` | Main loop, signals, config |
| `peer_dmr.c` / `peer_ysf.c` | UDP peers, reconnect, DGID/RPTO |
| `peer_echolink.c` | EchoLink directory, RTP/GSM, RTCP |
| `bridge.c` | YSF↔DMR voice bridge, identity, ModeConv pacing |
| `bridge_el.c` | EchoLink↔DMR / EchoLink↔YSF |
| `vocoder_remote.c` | DV3000 / AMBEServer UDP client |
| `aliases.c` | JSON alias load/download/lookup |
| `talker_alias.c` | DMRA decode (log only) |
| `ysf_fich.c` | YSF FICH codec + DGID rewrite |
| `hbp/dmr_hbp.c` | DMR HBP auth + LC/embedded codec |
| `mmdvm/` | ModeConv + Golay24128 (MMDVM_CM YSF2DMR) |
| `vendor/yyjson/` | JSON parser (MIT) |
| `docs/echolink-bridge.md` | EchoLink modes, ports, vocoder notes |

## License

**GPL v3** (or later). See [LICENSE](LICENSE).

| Component | License | Provenance |
|-----------|---------|------------|
| Application (`ysf2dmrcon`, peers, bridge, config, aliases, wrappers) | GPL-3.0-or-later | Copyright (C) 2026 Rodrigo Pérez, CE5RPY |
| `hbp/` | GPL-3.0-or-later | Doug McLain; Esteban Mackay HP3ICC; MMDVM_CM |
| `ysf_fich.c` | GPL-3.0-or-later | Doug McLain; Esteban Mackay HP3ICC |
| `mmdvm/` | GPL-2.0-or-later | Jonathan Naylor G4KLX; Andy Uribe CA6JAU; [MMDVM_CM](https://github.com/juribeparada/MMDVM_CM) |
| `vendor/yyjson/` | MIT | YaoYuan, [yyjson](https://github.com/ibireme/yyjson) |
