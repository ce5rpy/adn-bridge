# EchoLink bridge (adn-bridge)

How to run **`echolink-dmr`** (EchoLink ↔ DMR) and **`echolink-ysf`** (EchoLink ↔ YSF).

Build / install: [README.md](../README.md) · [README.es.md](../README.es.md).
**Español:** [echolink-bridge.es.md](echolink-bridge.es.md).
YSF ↔ DMR (no EchoLink): [ysf-dmr-bridge.md](ysf-dmr-bridge.md).

## Requirements

1. Validated EchoLink station (`CALL-L`, `CALL-R`, or conference callsign if you host).
2. Hardware AMBE vocoder reachable over UDP (DV3000 / AMBEServer protocol).
3. Network (pick one):
   - **Direct:** allow **UDP 5198–5199** inbound (and outbound **TCP 5200** to the directory), or
   - **EchoLink Proxy:** outbound **TCP** to the proxy (default port **8100**) only — no local UDP open.

## Quick start

```bash
mkdir -p config
# EchoLink <-> DMR
cp examples/adn-bridge-echolink-dmr.example.ini config/adn-bridge-echolink-dmr.ini
# edit: passwords, bind_addr (or proxy_*), DMR master, EchoLink host, vocoder
./adn-bridge -c config/adn-bridge-echolink-dmr.ini

# EchoLink <-> YSF
cp examples/adn-bridge-echolink-ysf.example.ini config/adn-bridge-echolink-ysf.ini
# edit: passwords, bind_addr (or proxy_*), YSF reflector/DGID, EchoLink host, vocoder
./adn-bridge -c config/adn-bridge-echolink-ysf.ini
```

Behind NAT: set `proxy_server` (and optional `proxy_port` / `proxy_password`) instead of opening UDP 5198/5199 — see [EchoLink Proxy](#echolink-proxy-behind-nat).

When the peer is resolved and linked you should see something like
`linked to … (RTCP SDES)`.

## Mode

```ini
[bridge]
mode = echolink-dmr     ; or echolink-ysf
```

## `[echolink]` — what to set

| Key | Required | Description |
|-----|----------|-------------|
| `callsign` | yes | Your EchoLink station |
| `password` | yes | Directory password |
| `bind_addr` | yes* | Local IP for UDP 5198/5199 (*not required if `proxy_server` is set) |
| `host` | yes | Node (`CALL-L` / `CALL-R`) or conference (`*NAME*`) to connect to |
| `qth` / `email` | no | Optional directory metadata |
| `directory_servers` | no | Defaults to the public `serverN.echolink.org` hosts |
| `login_interval` | no | Directory presence refresh (default **360** s) |
| `station_list_interval` | no | How often to refresh the peer IP from the station list (default **600** s) |
| `gain` | no | Audio scale **EchoLink → DMR/YSF** before AMBE. `1.0` = no change (default); suggested **0.5** for `echolink-ysf` and **1.0** for `echolink-dmr`; accepted range above **0** up to **4**. Does not affect DMR/YSF → EchoLink. |
| `proxy_server` | no | EchoLink Proxy host. If set, all EL traffic goes through the proxy |
| `proxy_port` | no | Proxy TCP port (default **8100** when `proxy_server` is set) |
| `proxy_password` | no | Proxy password (default **PUBLIC** for public proxies) |
| `log_level` | no | Else inherits `[log]` |

Ports **5198 / 5199 / 5200** are fixed in code for direct mode (not INI keys). With a proxy they are used only on the proxy host.

## EchoLink Proxy (behind NAT)

```ini
[echolink]
callsign = N0CALL-L
password = your-directory-password
host = *SOMECONF*
; omit bind_addr when using a proxy
proxy_server = your.proxy.example
proxy_port = 8100
proxy_password = PUBLIC
```

- Omit all three `proxy_*` keys → **direct** mode (unchanged).
- With proxy: look for `echolink: connected to proxy …` / `using proxy …` in the terminal log.
- Failures: `proxy bad password`, `proxy access denied`, `proxy connect …`.

## `[vocoder]`

```ini
[vocoder]
host = 127.0.0.1
port = 2460
```

Point `host`/`port` at your hardware vocoder. Without it there is no voice between EchoLink and DMR/YSF.

## DMR or YSF side

**`echolink-dmr`:** fill `[dmr]` like a Homebrew peer (`callsign`, `dmrid`, `host`, `port`, `tg`, `password`; optional `options` RPTO). TX is always TS2.

```ini
[dmr]
tg = 730170
; optional: drop leftover dynamic TGs for this peer before connect PTT
clear_dynamic_tg = 1
```

**`echolink-ysf`:** fill `[ysf]` (`host`, `port`, `dgid`). Omit `[dmr]` entirely (no DMR peer). YSF gateway callsign comes from `[echolink] callsign`.

Subscriber aliases (`[aliases]`) map callsigns ↔ DMR IDs the same way as YSF↔DMR — see [ysf-dmr-bridge.md](ysf-dmr-bridge.md) and the README.

## Logs (program output in the terminal)

Strings like `EL->DMR`, `RTP RX`, or `vocoder ENC` are **not INI keys**.
They appear in the **log** that `adn-bridge` prints while it runs.

1. Open a terminal on the server.
2. Start the bridge in the foreground (so you see the log immediately):

```bash
./adn-bridge -c adn-bridge-echolink-dmr.ini
```

3. Keep that window open — each event prints a line there.
4. Under systemd: `journalctl -u your-service-name -f`  
   Under nohup/redirect: open the file you sent output to (`>> bridge.log 2>&1`).

### Turn on more detail (in the INI, then restart)

```ini
[log]
level = INFO

[echolink]
log_level = DEBUG

[vocoder]
log_level = DEBUG

# echolink-dmr:
[dmr]
log_level = DEBUG

# echolink-ysf:
# [ysf]
# log_level = DEBUG
```

A real terminal line looks like:

```text
2026-07-17 14:33:59,754 INFO/echolink: echolink: linked to *REDCHILE* (RTCP SDES)
2026-07-17 14:34:10,120 INFO/dmr: EL->DMR call start (TG 730170, src CE5RPY    id 7300391)
```

- Timestamp + level (`INFO`/`DEBUG`/…) + channel (`echolink`, `dmr`, `ysf`, `vocoder`)
- After the colon: the message text to search for

### What should appear, in order (EchoLink → DMR or YSF)

| Step | Text to find **in the terminal log** | Meaning |
|------|----------------------------------------|---------|
| 0 | `echolink: connected to proxy …` / `using proxy …` | Proxy mode only — TCP to proxy OK |
| 1 | `echolink: directory login OK` | Directory login succeeded |
| 2 | `echolink: connecting to …` | `host` IP resolved |
| 3 | `echolink: linked to … (RTCP SDES)` | Linked (control only — **not audio yet**) |
| 4 | `echolink: RTP RX` (DEBUG) or `echolink: EL audio rms=` (INFO) | EchoLink audio (RTP) is arriving |
| 5 | `EL->DMR call start` or `EL->YSF call start` | Call started toward DMR/YSF |
| 6 | `vocoder ENC` (DEBUG) or at startup `vocoder ready at` | Hardware vocoder is converting |
| 7 | `EL->DMR call end` or `EL->YSF call end` | Call finished |

Reverse path: in the same log look for `DMR->EL call start` or `YSF->EL call start`, then `vocoder DEC`.

**Important:** `linked to …` without `RTP RX` / `EL audio rms` / `call start` means linked but silent (nobody transmitting on that node/conference, or your EchoLink app is on another room).

### If there is no audio

| Missing from the log | What to check |
|----------------------|---------------|
| `connected to proxy` / `using proxy` | `proxy_server` / port / password, outbound TCP 8100 |
| `directory login OK` | `callsign`/`password`, TCP 5200 or proxy path, `directory_servers` |
| IP / `connecting to` / `station list: … not found` | Wrong `host` spelling or not in the directory |
| `linked to …` | Direct: UDP 5198/5199 / NAT. Proxy: proxy up / peer SDES |
| `RTP RX` / `EL audio rms` (while `linked`) | Nobody talking on that conference/node |
| After `call start`: `vocoder ENC timeout` or `PRODID probe failed` | `[vocoder] host`/`port` and the AMBE hardware |
| `call start` but silence on DMR master / YSF reflector | `[dmr]` TG/password or `[ysf]` host/DGID |

Filter while it runs:

```bash
./adn-bridge -c your.ini 2>&1 | grep -E 'linked|RTP|EL audio|EL->|YSF->EL|DMR->EL|vocoder|call start|call end'
```

## Notes

- One active call direction at a time (half-duplex).
- This project ships **source only**; AMBE encoding/decoding is done by the hardware vocoder.
