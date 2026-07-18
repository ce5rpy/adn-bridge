# ysf2dmrcon

**Version 0.3.0**

Standalone voice bridge with three modes:

| Mode | Role |
|------|------|
| `ysf-dmr` (default) | YSF reflector ↔ DMR server (Homebrew peer + YSFP/DGID) |
| `echolink-dmr` | EchoLink ↔ DMR (GSM/RTP + hardware AMBE vocoder) |
| `echolink-ysf` | EchoLink ↔ YSF (same EchoLink + hardware vocoder) |

Self-contained build — vendored code under `hbp/`, `mmdvm/`, and `vendor/`.
Upstream reference: [MMDVM_CM](https://github.com/juribeparada/MMDVM_CM).
YSF↔DMR details: [docs/ysf-dmr-bridge.md](docs/ysf-dmr-bridge.md)
([ES](docs/ysf-dmr-bridge.es.md)).
EchoLink details: [docs/echolink-bridge.md](docs/echolink-bridge.md)
([ES](docs/echolink-bridge.es.md)).

**Documentación en español:** [README.es.md](README.es.md)

## Build

### Dependencies (Debian / Ubuntu)

```bash
sudo apt-get update
sudo apt-get install -y build-essential libssl-dev zlib1g-dev libgsm1-dev curl
```

| Package | Why |
|---------|-----|
| `build-essential` | `gcc`, `g++`, `make` |
| `libssl-dev` | OpenSSL (`-lcrypto`) for blake2b alias checksums |
| `zlib1g-dev` | zlib (`-lz`) for EchoLink station-list decompress |
| `libgsm1-dev` | EchoLink GSM (`-lgsm`); runtime `libgsm1` pulled in; headers also under `vendor/gsm/` |
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

### YSF ↔ DMR (default)

1. Copy the template and edit your settings:

   ```bash
   cp ysf2dmrcon.example.ini ysf2dmrcon.ini
   # same template under an explicit name:
   # cp ysf2dmrcon-ysf-dmr.example.ini ysf2dmrcon-ysf-dmr.ini
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

Requires a hardware AMBE vocoder (UDP DV3000 / AMBEServer protocol). Use a
validated EchoLink `-L` / `-R` / conference callsign.

```bash
# EchoLink <-> DMR
cp ysf2dmrcon-echolink-dmr.example.ini ysf2dmrcon-echolink-dmr.ini
# edit passwords, bind_addr (or proxy_*), DMR master, EchoLink host (node or *CONF*)
./ysf2dmrcon -c ysf2dmrcon-echolink-dmr.ini

# EchoLink <-> YSF
cp ysf2dmrcon-echolink-ysf.example.ini ysf2dmrcon-echolink-ysf.ini
# edit passwords, bind_addr (or proxy_*), YSF reflector/DGID, EchoLink host
./ysf2dmrcon -c ysf2dmrcon-echolink-ysf.ini
```

Local `*.ini` files (with passwords) are gitignored; only `*.example.ini` is
committed. See [docs/echolink-bridge.md](docs/echolink-bridge.md)
([ES](docs/echolink-bridge.es.md)).

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

Templates: `ysf2dmrcon.example.ini` / `ysf2dmrcon-ysf-dmr.example.ini` (YSF↔DMR),
`ysf2dmrcon-echolink-dmr.example.ini`, `ysf2dmrcon-echolink-ysf.example.ini`.

### `[ysf]` / `[dmr]` — YSF ↔ DMR

Minimal keys: YSF `host`/`port`/`dgid`; DMR `callsign`/`dmrid`/`host`/`port`/
`tg`/`password` (optional `options` RPTO). TX always TS2.
Optional `clear_dynamic_tg = 1`: on DMR login, silence-PTT **TG 4000** first
(drop dynamic TGs), then the connect PTT to `tg`.

Setup guide:

→ **[docs/ysf-dmr-bridge.md](docs/ysf-dmr-bridge.md)**
([ES](docs/ysf-dmr-bridge.es.md))

In **`echolink-ysf`** mode no DMR peer is opened: `host`/`port`/`password` are
unused (password may be a placeholder). `callsign` / `dmrid` still feed YSF
CSD/DCH RadioID — see [docs/echolink-bridge.md](docs/echolink-bridge.md).

### `[echolink]` / `[vocoder]` — EchoLink modes

Minimal keys: `callsign`, `password`, `bind_addr`, `host` (node or `*CONF*`),
and `[vocoder] host`/`port`. Ports **5198/5199/5200** are fixed in direct mode.
Optional EchoLink Proxy: `proxy_server` / `proxy_port` / `proxy_password`
(default `PUBLIC`) — then `bind_addr` is not required.

Setup guide (ports, proxy, vocoder, INI, how to check audio):

→ **[docs/echolink-bridge.md](docs/echolink-bridge.md)**
([ES](docs/echolink-bridge.es.md))

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

- **YSF ↔ DMR:** [docs/ysf-dmr-bridge.md](docs/ysf-dmr-bridge.md)
  ([ES](docs/ysf-dmr-bridge.es.md))
- **EchoLink → DMR / YSF:** inbound RTCP SDES; see
  [docs/echolink-bridge.md](docs/echolink-bridge.md)
  ([ES](docs/echolink-bridge.es.md))

## Project layout

| Path | Role |
|------|------|
| `ysf2dmrcon.c` | Main loop, signals, config |
| `peer_dmr.c` / `peer_ysf.c` | UDP peers, reconnect, DGID/RPTO |
| `peer_echolink.c` / `el_proxy.c` | EchoLink directory, RTP/GSM, RTCP, optional proxy |
| `bridge.c` | YSF↔DMR voice bridge, identity, ModeConv pacing |
| `bridge_el.c` | EchoLink↔DMR / EchoLink↔YSF |
| `vocoder_remote.c` | DV3000 / AMBEServer UDP client |
| `aliases.c` | JSON alias load/download/lookup |
| `talker_alias.c` | DMRA decode (log only) |
| `ysf_fich.c` | YSF FICH codec + DGID rewrite |
| `hbp/dmr_hbp.c` | DMR HBP auth + LC/embedded codec |
| `mmdvm/` | ModeConv + Golay24128 (MMDVM_CM YSF2DMR) |
| `vendor/yyjson/` | JSON parser (MIT) |
| `docs/ysf-dmr-bridge.md` / `.es.md` | YSF↔DMR setup guide |
| `docs/echolink-bridge.md` / `.es.md` | EchoLink setup guide |

## License

**GPL v3** (or later). See [LICENSE](LICENSE).

| Component | License | Provenance |
|-----------|---------|------------|
| Application (`ysf2dmrcon`, peers, bridge, config, aliases, wrappers) | GPL-3.0-or-later | Copyright (C) 2026 Rodrigo Pérez, CE5RPY |
| `hbp/` | GPL-3.0-or-later | Doug McLain; Esteban Mackay HP3ICC; MMDVM_CM |
| `ysf_fich.c` | GPL-3.0-or-later | Doug McLain; Esteban Mackay HP3ICC |
| `mmdvm/` | GPL-2.0-or-later | Jonathan Naylor G4KLX; Andy Uribe CA6JAU; [MMDVM_CM](https://github.com/juribeparada/MMDVM_CM) |
| `vendor/yyjson/` | MIT | YaoYuan, [yyjson](https://github.com/ibireme/yyjson) |
