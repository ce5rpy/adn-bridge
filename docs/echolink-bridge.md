# EchoLink bridge modes (ysf2dmrcon)

## Modes

Set in INI:

```ini
[bridge]
mode = ysf-dmr          ; default — existing YSF <-> DMR
mode = echolink-dmr     ; EchoLink <-> DMR
mode = echolink-ysf     ; EchoLink <-> YSF
```

## EchoLink ports (fixed in code)

| Port | Role |
|------|------|
| UDP 5198 | RTP audio (GSM) |
| UDP 5199 | RTCP control / SDES |
| TCP 5200 | Directory login (to `directory_servers`) |

Do **not** put these ports in the INI. Configure only:

- `bind_addr` — local IP for 5198/5199
- `callsign` / `password` — EchoLink station credentials
- `host` — node or conference **callsign** to connect (e.g. `CA5RPY-L`, `*REDCHILE*`); resolved via the EchoLink directory station list. A dotted IPv4 is still accepted as a lab escape hatch.
- `directory_servers` — comma-separated directory hostnames
- `login_interval` — tlb `LoginInterval` (default **360** s); directory presence login
- `station_list_interval` — tlb `StationListInterval` (default **600** s); full station-list refresh and peer IP update

Directory timing matches thelinkbox `RTCP_Handler`:

1. Startup: `LOGIN_AND_LIST`, next login at +60 s, next list at +`station_list_interval`
2. When login is due and list is also due → login + station list
3. When only login is due → login only
4. When only list is due → station list only (re-resolve `host`, update IP if it changed)

## Vocoder

```ini
[vocoder]
host = 127.0.0.1
port = 2460
```

Wire protocol is DV3000 / AMBEServer style (UDP). Lab uses **md380-emu** (`emu-ambe` docker).

| Layer | Format |
|-------|--------|
| ModeConv / bridge API | 7-byte **raw** (deinterleaved) 49-bit AMBE |
| UDP to md380-emu @ RATET 34 | same 49 bits **interleaved** (`interleave49`) |
| Analog_Bridge `useEmulator` | often **AMBE72** (RATET 33 / RATEP 3600x2450, 9-byte FEC) |

With `log_level=DEBUG`, look for `vocoder ENC` / `vocoder DEC` lines (`raw=` vs `wire=`, `pcm_rms=`) and `DMR->EL ambe` / `EL->DMR ambe` in the bridge.

## Lab topology (ADN)

| Role | Callsign | IP |
|------|----------|-----|
| thelinkbox | CA5RPY-L | 44.31.61.72 |
| ysf2dmrcon bridge | CE5RPY-L | 44.31.61.70 |
| AMBEServer | — | 127.0.0.1:2460 |

```bash
./ysf2dmrcon -c ysf2dmrcon-echolink.ini
```

Connectivity: set `host=CA5RPY-L` (or a conference like `*REDCHILE*`). The bridge logs into the directory, looks up that callsign in the station list, then sends RTCP SDES to the resolved IP.

## Sources only

This project ships source code. AMBE codecs run in an external process reached by UDP.
