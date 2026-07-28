# adn-bridge

**Version 0.3.1**

Standalone voice bridge — configure two `[peer.*]` endpoints:

| Layout | Peers |
|--------|-------|
| YSF ↔ DMR | `[peer.*]` type `ysf` + `dmr` |
| EchoLink ↔ DMR | `echolink` + `dmr` (`vocoder_host`/`port` on `[peer.el]`) |
| EchoLink ↔ YSF | `echolink` + `ysf` (`vocoder_host`/`port` on `[peer.el]`) |

Self-contained build — vendored code under `hbp/`, `mmdvm/`, and `vendor/`.
Upstream reference: [MMDVM_CM](https://github.com/juribeparada/MMDVM_CM).
Full guide: [docs/bridge.md](docs/bridge.md) ([ES](docs/bridge.es.md)).

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

Produces `adn-bridge`. Optional install:

```bash
sudo make install
# → /opt/adn-bridge/adn-bridge
# → /opt/adn-bridge/config/*.example.ini
# → /opt/adn-bridge/data/
```

## Quick start

1. Copy a template into `config/` (install default: `/opt/adn-bridge/config/`):

   ```bash
   mkdir -p config
   # Master template (YSF+DMR peers; EchoLink peers commented)
   cp examples/adn-bridge.example.ini config/adn-bridge.ini

   # Or a ready-to-run layout:
   # cp examples/adn-bridge-ysf-dmr.example.ini config/adn-bridge.ini
   # cp examples/adn-bridge-echolink-dmr.example.ini config/adn-bridge.ini
   # cp examples/adn-bridge-echolink-ysf.example.ini config/adn-bridge.ini
   ```

| Template | Layout |
|----------|--------|
| `examples/adn-bridge.example.ini` | Master — YSF+DMR peers (EchoLink commented) |
| `examples/adn-bridge-ysf-dmr.example.ini` | YSF ↔ DMR |
| `examples/adn-bridge-echolink-dmr.example.ini` | EchoLink ↔ DMR |
| `examples/adn-bridge-echolink-ysf.example.ini` | EchoLink ↔ YSF |

2. Run with an INI path:

   ```bash
   ./adn-bridge -c config/adn-bridge.ini
   # installed: /opt/adn-bridge/adn-bridge -c /opt/adn-bridge/config/adn-bridge.ini
   ```

EchoLink modes need a hardware AMBE vocoder and a validated `-L` / `-R` /
conference callsign — [docs/bridge.md](docs/bridge.md)
([ES](docs/bridge.es.md)).

Local INIs under `config/` are gitignored; templates live under
`examples/*.example.ini`.

## Multiple instances

Run **one process per bridge** (different DGID, DMR TG, and `dmrid`). Each
instance needs its own INI under `config/`:

```bash
./adn-bridge -c config/adn-bridge-tg71442.ini
./adn-bridge -c config/adn-bridge-tg71481.ini
```

Instances may share one subscriber database: set the same `[aliases] data_dir`
in each INI (default `./data`, or `/opt/adn-bridge/data` when installed).

### Interactive setup + systemd

```bash
./examples/generate-config.sh          # or: --lang es
# writes adn-bridge-<instance>.ini (+ optional .service)
```

Enter accepts the suggested default (ADN alias URLs, ports, etc.).

Systemd examples under `examples/`:

| File | Use |
|------|-----|
| `adn-bridge.service` | Single instance (edit paths) |
| `adn-bridge@.service` | Template: `systemctl enable --now adn-bridge@redchile` → INI `config/adn-bridge-redchile.ini` |

```bash
sudo make install
# INI: /opt/adn-bridge/config/adn-bridge-redchile.ini
sudo cp examples/adn-bridge@.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now adn-bridge@redchile.service
```

## Configuration

Define **two enabled** `[peer.<name>]` stanzas. Global sections: `[aliases]`,
`[log]`. EchoLink peers require `vocoder_host` and `vocoder_port`.

```ini
[peer.fusion]
type = ysf
enabled = true
host = reflector.example.net
port = 42000
callsign = N0CALL
dgid = 1

[peer.master]
type = dmr
enabled = true
callsign = N0CALL
dmrid = 1234567
host = master.example.net
port = 62031
tg = 1234
password = change-me
options = TS2=1234;SINGLE=0;
```

Supported mixes: `dmr+ysf`, `echolink+dmr`, `echolink+ysf` (exactly one of each
type, both enabled).

Templates: `examples/adn-bridge.example.ini` plus one file per layout under
`examples/adn-bridge-*.example.ini`.

### YSF ↔ DMR peers

`[peer.*]` with `type = ysf` or `type = dmr`. YSF keys: `host`/`port`/`callsign`/
`dgid`. DMR keys: `callsign`/`dmrid`/`host`/`port`/`tg`/`password` (optional
`options` RPTO). TX always TS2. Optional `clear_dynamic_tg = 1`: on DMR login,
silence-PTT **TG 4000** first, then connect PTT to `tg`.

Setup guide: **[docs/bridge.md](docs/bridge.md)** ([ES](docs/bridge.es.md))
— sections *YSF ↔ DMR* and *Peer reference*.

### EchoLink peers

`type = echolink` plus `callsign`, `password`, `bind_addr` (or `proxy_*`), `host`.
`vocoder_host` / `vocoder_port` on each `[peer.el]` (EchoLink layouts).

Setup guide: **[docs/bridge.md](docs/bridge.md)** ([ES](docs/bridge.es.md))
— sections *EchoLink* and *Troubleshooting*.

### `[aliases]` — Subscriber database


Same layout as `ALIASES` in adn-server. Files live under `data_dir`
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

See [docs/bridge.md](docs/bridge.md) ([ES](docs/bridge.es.md)) — *Subscriber aliases* and *Identity rules*.

## Project layout

| Path | Role |
|------|------|
| `adn_bridge.c` | Main loop, signals, config |
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
| `docs/bridge.md` / `bridge.es.md` | Configuration and operation (all layouts) |
| `examples/` | `*.example.ini`, systemd units, `generate-config.sh` |
| `config/` | Local instance INIs (gitignored; `make install` → `/opt/adn-bridge/config/`) |
| `data/` | Alias cache (gitignored; `make install` → `/opt/adn-bridge/data/`) |

## Acknowledgments

Thanks to **[Esteban Mackay, HP3ICC](https://gitlab.com/hp3icc)**, for ideas, help, testing, and corrections throughout development.

## License

**GPL v3** (or later). See [LICENSE](LICENSE).

| Component | License | Provenance |
|-----------|---------|------------|
| Application (`adn-bridge`, peers, bridge, config, aliases, wrappers) | GPL-3.0-or-later | Copyright (C) 2026 Rodrigo Pérez, CE5RPY |
| `hbp/` | GPL-3.0-or-later | Doug McLain; Esteban Mackay HP3ICC; MMDVM_CM |
| `ysf_fich.c` | GPL-3.0-or-later | Doug McLain; Esteban Mackay HP3ICC |
| `mmdvm/` | GPL-2.0-or-later | Jonathan Naylor G4KLX; Andy Uribe CA6JAU; [MMDVM_CM](https://github.com/juribeparada/MMDVM_CM) |
| `vendor/yyjson/` | MIT | YaoYuan, [yyjson](https://github.com/ibireme/yyjson) |
