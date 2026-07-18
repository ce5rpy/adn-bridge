# YSF ↔ DMR bridge (ysf2dmrcon)

How to run the default mode **`ysf-dmr`** (YSF reflector ↔ DMR Homebrew peer).

Build / install: [README.md](../README.md) · [README.es.md](../README.es.md).
**Español:** [ysf-dmr-bridge.es.md](ysf-dmr-bridge.es.md).
EchoLink modes: [echolink-bridge.md](echolink-bridge.md).

## Requirements

1. YSF reflector (host, port, DGID).
2. DMR master with a Homebrew peer password and talkgroup.
3. Unique bridge `callsign` + `dmrid` per instance.
4. No hardware AMBE vocoder needed for this mode.

## Quick start

```bash
cp ysf2dmrcon.example.ini ysf2dmrcon.ini
# or: cp ysf2dmrcon-ysf-dmr.example.ini ysf2dmrcon-ysf-dmr.ini
# edit YSF reflector/DGID, DMR host/password/tg, callsign/dmrid
./ysf2dmrcon
# or: ./ysf2dmrcon -c /path/to/your.ini
```

## Mode

```ini
[bridge]
mode = ysf-dmr
```

## `[ysf]`

| Key | Required | Description |
|-----|----------|-------------|
| `host` | yes | Reflector hostname or IP |
| `port` | yes | Usually `42000` |
| `dgid` | yes | Room to join (0–99) |

## `[dmr]`

| Key | Required | Description |
|-----|----------|-------------|
| `callsign` | yes | Bridge callsign (shown on the monitor) |
| `dmrid` | yes | Bridge DMR ID — unique per process |
| `host` / `port` | yes | DMR master |
| `tg` | yes | Voice talkgroup (TX always **TS2**) |
| `password` | yes | Homebrew peer password |
| `options` | no | RPTO string (e.g. `TS2=1234;SINGLE=0;TIMER=60;`); omit/empty = no RPTO |
| `location` / `description` | no | Monitor display text |

RX/TX frequencies are zero in RPTC → the monitor shows N/A (normal for a software bridge).

## Subscriber aliases (`[aliases]`)

Optional but recommended so talkers show the right callsign / DMR ID.

- Files under `data_dir` (default `./data`), same idea as adn-server `ALIASES`.
- See the README for `subscriber_url`, `stale_minutes`, etc.
- Several bridge processes may share one `data_dir`.

**Identity (what users see):**

- DMR → YSF: look up radio ID in `subscriber_ids.json`; if missing, send the number.
- YSF → DMR: strip suffix after `-` or `/` (`HP3ICC-FT3` → `HP3ICC`), then look up; if unknown, use the bridge `callsign` + `dmrid`.

## Several bridges

One process per bridge (different DGID, TG, and `dmrid`), each with its own INI:

```bash
./ysf2dmrcon -c ysf2dmrcon-tg71442.ini
./ysf2dmrcon -c ysf2dmrcon-tg71481.ini
```

## Logging

`[log] level=INFO` for normal use; `DEBUG` on `[dmr]` / `[ysf]` when troubleshooting.
Useful lines: `YSF->DMR` / `DMR->YSF` call start/end.

## Notes

- This project ships **source only**.
