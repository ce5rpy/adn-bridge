# adn-bridge — configuration and operation

Complete guide for all bridge layouts: **YSF ↔ DMR**, **EchoLink ↔ DMR**, and
**EchoLink ↔ YSF**.

Build / install: [README.md](../README.md) · [README.es.md](../README.es.md).
**Español:** [bridge.es.md](bridge.es.md).

## Overview

Configure **exactly two enabled** `[peer.<name>]` stanzas. Global sections:
`[aliases]`, `[log]`. EchoLink peers set `vocoder_host` / `vocoder_port` inline.

| Layout | Peers | Vocoder |
|--------|-------|---------|
| YSF ↔ DMR | `ysf` + `dmr` | not required |
| EchoLink ↔ DMR | `echolink` + `dmr` | required |
| EchoLink ↔ YSF | `echolink` + `ysf` | required |

Templates under `examples/`:

| File | Layout |
|------|--------|
| `adn-bridge-ysf-dmr.example.ini` | YSF ↔ DMR |
| `adn-bridge-echolink-dmr.example.ini` | EchoLink ↔ DMR |
| `adn-bridge-echolink-ysf.example.ini` | EchoLink ↔ YSF |
| `adn-bridge.example.ini` | Master (YSF+DMR; EchoLink commented) |

Interactive generator: `./examples/generate-config.sh` (`--lang es`).

## Quick start

```bash
mkdir -p config
cp examples/adn-bridge-ysf-dmr.example.ini config/adn-bridge.ini
# edit peers, then:
./adn-bridge -c config/adn-bridge.ini
# installed: /opt/adn-bridge/adn-bridge -c /opt/adn-bridge/config/adn-bridge.ini
```

EchoLink layouts:

```bash
cp examples/adn-bridge-echolink-dmr.example.ini config/adn-bridge-el-dmr.ini
cp examples/adn-bridge-echolink-ysf.example.ini config/adn-bridge-el-ysf.ini
```

Several instances: one process per bridge (different DGID, TG, `dmrid`), each
with its own INI. They may share one `[aliases] data_dir`.

---

## YSF ↔ DMR

### Requirements

1. YSF reflector (host, port, DGID).
2. DMR master with Homebrew peer password and talkgroup.
3. Unique bridge `callsign` + `dmrid` per instance.
4. No hardware AMBE vocoder.

### Example

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
options = TS2=1234;SINGLE=0;TIMER=60;
```

### Logging

Useful lines: `YSF->DMR` / `DMR->YSF` call start/end.

---

## EchoLink ↔ DMR / EchoLink ↔ YSF

### Requirements

1. Validated EchoLink station (`CALL-L`, `CALL-R`, or conference callsign).
2. Hardware AMBE vocoder over UDP (DV3000 / AMBEServer).
3. Network — **direct:** UDP **5198–5199** inbound + TCP **5200** to directory;
   **proxy:** outbound TCP to EchoLink Proxy (default port **8100**), no local UDP.

When linked you should see `linked to … (RTCP SDES)` in the log.

### EchoLink proxy (behind NAT)

```ini
[peer.el]
type = echolink
enabled = true
callsign = N0CALL-L
password = your-directory-password
host = *SOMECONF*
proxy_server = your.proxy.example
proxy_port = 8100
proxy_password = PUBLIC
```

Omit all `proxy_*` keys for direct mode (`bind_addr` required). With proxy,
look for `echolink: connected to proxy …` / `using proxy …`.

### Vocoder (on `[peer.el]`)

Hardware AMBE (DV3000 / AMBEServer) — configured on the peer that needs it
(EchoLink today; D-Star etc. later). Required keys:

```ini
[peer.el]
type = echolink
...
vocoder_host = 127.0.0.1
vocoder_port = 2460
# vocoder_log_level = INFO   # optional; overrides [log] for vocoder channel
```

`vocoder_port` defaults to **2460** if omitted, but `vocoder_host` is required.

Without a reachable vocoder there is no voice between EchoLink and DMR/YSF.

### Log checklist (EchoLink → DMR/YSF)

| Step | Log text | Meaning |
|------|----------|---------|
| 0 | `connected to proxy` / `using proxy` | Proxy mode — TCP OK |
| 1 | `directory login OK` | Directory login OK |
| 2 | `connecting to …` | `host` IP resolved |
| 3 | `linked to … (RTCP SDES)` | Linked (control only) |
| 4 | `RTP RX` or `EL audio rms=` | EchoLink audio arriving |
| 5 | `EL->DMR call start` / `EL->YSF call start` | Call toward radio side |
| 6 | `vocoder ENC` / `vocoder ready at` | Hardware vocoder active |
| 7 | `EL->DMR call end` / `EL->YSF call end` | Call finished |

Reverse: `DMR->EL call start` / `YSF->EL call start`, then `vocoder DEC`.

`linked to …` without `RTP RX` / `call start` = linked but silent (nobody TX on
that node/conference).

Filter while running:

```bash
./adn-bridge -c your.ini 2>&1 | grep -E 'linked|RTP|EL audio|EL->|YSF->EL|DMR->EL|vocoder|call start|call end'
```

---

## Peer reference

### Common keys (all `[peer.*]`)

| Key | Required | Description |
|-----|----------|-------------|
| `type` | yes | `dmr`, `ysf`, or `echolink` (alias `el`) |
| `enabled` | no | Default **true** when stanza is present |
| `log_level` | no | Overrides `[log] level` for this peer's channel |

### `type = ysf`

| Key | Required | Description |
|-----|----------|-------------|
| `host` / `port` | yes | Reflector (usually port `42000`) |
| `callsign` | yes | Gateway callsign on the reflector |
| `dgid` | yes | Room 0–99 |

### `type = dmr`

| Key | Required | Description |
|-----|----------|-------------|
| `callsign` | yes | Bridge callsign (monitor) |
| `dmrid` | yes | Unique DMR ID per process |
| `host` / `port` | yes | DMR master |
| `tg` | yes | Voice talkgroup (TX always **TS2**) |
| `password` | yes | Homebrew peer password |
| `options` | no | RPTO string; omit/empty = no RPTO (typical on XLX) |
| `location` / `description` | no | Monitor display text |
| `clear_dynamic_tg` | no | See below |

**`clear_dynamic_tg`** — optional, **default off** (omit the key or `= 0`).

- `1` — on DMR login: silence-PTT **TG 4000** first (drops leftover dynamic TGs
  for this peer), then connect PTT to `tg`.
- Recommended on ADN masters with dynamic TGs; omit on XLX and masters without
  OPTIONS/RPTO.

RX/TX frequencies are zero in RPTC → monitor shows N/A (normal for software
bridges).

### `type = echolink`

| Key | Required | Description |
|-----|----------|-------------|
| `callsign` | yes | Your EchoLink station |
| `password` | yes | Directory password |
| `bind_addr` | yes* | Local IP for UDP 5198/5199 (*not if `proxy_server` set) |
| `host` | yes | Node or `*CONF*` to connect to |
| `qth` / `email` | no | Directory metadata |
| `directory_servers` | no | Default: public `serverN.echolink.org` |
| `login_interval` | no | Default **360** s |
| `station_list_interval` | no | Default **600** s |
| `gain` | no | EchoLink → DMR/YSF PCM scale before AMBE; default **1.0**;
  suggested **0.5** for EchoLink↔YSF, **1.0** for EchoLink↔DMR; range **0**–**4** |
| `vocoder_host` | yes | AMBE server host (DV3000 / AMBEServer) |
| `vocoder_port` | yes* | Usually `2460` (*default **2460** if key omitted) |
| `vocoder_log_level` | no | Overrides `[log]` for vocoder channel |
| `proxy_server` / `proxy_port` / `proxy_password` | no | EchoLink Proxy (port default **8100**, password **PUBLIC**) |

Ports **5198 / 5199 / 5200** are fixed in code (not INI keys).

---

## `[aliases]`

Optional; recommended for correct talker callsign / DMR ID display.

- Files under `data_dir` (default `./data`), same model as adn-server `ALIASES`.
- Multiple bridge processes may share one `data_dir`.

**Identity rules:**

- **DMR → YSF:** lookup radio ID in `subscriber_ids.json`; if missing, send the number.
- **YSF → DMR:** strip suffix after `-` or `/`, lookup; if unknown, bridge `callsign` + `dmrid`.
- **EchoLink → DMR/YSF:** inbound RTCP SDES talker name; DMR ID via alias lookup.

---

## `[log]`

`level = INFO` for normal use. Per-peer `log_level = DEBUG` when troubleshooting.

Example (EchoLink ↔ DMR):

```ini
[log]
level = INFO

[peer.el]
log_level = DEBUG

[peer.master]
log_level = DEBUG
```

Log line format — always `LEVEL/channel: message`, with an optional leading
timestamp:

```text
INFO/echolink: echolink: linked to *REDCHILE* (RTCP SDES)
2026-07-17 14:33:59,754 INFO/echolink: echolink: linked to *REDCHILE* (RTCP SDES)
```

### Output sinks (`handlers=`)

`[log]` also takes a `handlers=` list (comma-separated) choosing where lines
go and whether they carry a timestamp:

| Token | Destination | Timestamp |
|-------|-------------|-----------|
| `console` | stderr | no |
| `console-timed` | stderr | yes |
| `file` | `file=` path | no |
| `file-timed` | `file=` path | yes |
| `null` | — | (silences both sinks) |

```ini
[log]
level = INFO
handlers = console,file-timed
file = /var/log/adn-bridge/adn-bridge.log
```

**If `handlers=` is omitted**, adn-bridge auto-detects: no timestamp when
`stderr` is systemd's journal (`JOURNAL_STREAM` set — journalctl already
stamps every line, so the app's own timestamp would just be duplicated), a
timestamp when `stderr` is an interactive terminal. Running under systemd
this needs no config at all; running manually (`./adn-bridge -c config.ini`)
you still get timestamps. If you redirect stderr to a file yourself without a
terminal (`./adn-bridge ... >> out.log 2>&1`), that's neither a journal nor a
TTY, so auto-detect picks no-timestamp — set `handlers = console-timed`
explicitly if you want one in that case.

**Log rotation**: with a `file`/`file-timed` handler configured, send
`SIGHUP` to reopen the file at the same path (no restart, no dropped log
lines) — this is what a logrotate `postrotate` script should do:

```text
/var/log/adn-bridge/*.log {
    weekly
    rotate 4
    compress
    delaycompress
    missingok
    notifempty
    postrotate
        systemctl kill -s HUP adn-bridge@redchile.service
    endscript
}
```

Under systemd: `journalctl -u adn-bridge@instance -f`.

---

## Troubleshooting

### YSF ↔ DMR

| Symptom | Check |
|---------|-------|
| No audio either way | DMR connected? YSF linked? `call_active` in DEBUG logs |
| Wrong talker ID | `[aliases]` data_dir and `subscriber_ids.json` |
| DMR connect PTT only, no bridge audio | `tg`, `password`, `options` / master type |

### EchoLink layouts

| Missing from log | Check |
|------------------|-------|
| `connected to proxy` | `proxy_server`, port, password, outbound TCP |
| `directory login OK` | `callsign`/`password`, TCP 5200 or proxy |
| `station list: … not found` | `host` spelling / directory presence |
| `linked to …` only | UDP/NAT or proxy; nobody TX on conference |
| `vocoder ENC timeout` | `[peer.el] vocoder_host`/`vocoder_port`, AMBE hardware |
| `call start` but silence on DMR/YSF | peer `tg`/password or YSF host/DGID |

---

## Notes

- Half-duplex: one active ingress direction at a time.
- Source-only distribution; EchoLink paths use external hardware AMBE.
