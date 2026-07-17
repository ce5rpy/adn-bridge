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

Log channels (each can be `DEBUG|INFO|WARNING|ERROR`):

- `[log] level=` — default for all channels (also `app`: main/aliases)
- `[dmr] log_level=`, `[echolink] log_level=`, `[ysf] log_level=`, `[vocoder] log_level=`
- Or under `[log]`: `dmr=`, `echolink=`, `ysf=`, `vocoder=`

Lines look like `2026-07-17 10:44:12,350 DEBUG/dmr: …`. For DMR path noise, prefer `[dmr] log_level=DEBUG` with `[log] level=INFO` and quieter `echolink`/`vocoder`. Look for `vocoder ENC`/`DEC` (`raw=` vs `wire=`, `pcm_rms=`) and `DMR->EL ambe` / `EL->DMR ambe` or `EL->YSF` / `YSF->EL`.

EL→DMR/YSF call end uses **PCM energy hangtime** (~700 ms below speech RMS), not RTP idle alone — EchoLink conferences often keep sending comfort-noise RTP after unkey. Idle residual PCM is dropped so it cannot open a new DMR stream. EL→DMR UDP TX is paced at ~55 ms/frame (same as YSF↔DMR) to avoid burst `RATE DROP` on the master. EL→DMR sends a **single** VHEAD (not three identical ones): adn-server PacketControl treats duplicate VHEAD CRC/`lastData` as loss.

## EchoLink ↔ YSF path

Same EchoLink peer + remote AMBE vocoder as `echolink-dmr`. Voice crosses ModeConv as AMBE7:

| Direction | Flow |
|-----------|------|
| EL → YSF | GSM PCM → encode → `put_ambe7_ysf` ×5 → YSFD HEADER / VD2 VOICE / EOT |
| YSF → EL | YSFD → `put_ysf*` → `get_dmr` → `dmr33_to_ambe` → decode → EL PCM |

Half-duplex: one active call at a time (`call_active` 1 = EL→YSF, 2 = YSF→EL). EL→YSF ends after ~2 s RTP silence (`last_rtp_rx`). YSF framing matches the existing YSF↔DMR bridge (sync, FICH, DCH slots, HP3ICC `ysf_modeconv_chunk` repack).

`[dmr] callsign` / `dmrid` are still required for YSF wire identity and CSD/DCH; no DMR UDP peer is opened in this mode (`password` may be a placeholder).

## Lab topology (ADN)

| Role | Callsign | IP |
|------|----------|-----|
| thelinkbox | CA5RPY-L | 44.31.61.72 |
| ysf2dmrcon bridge | CE5RPY-L | 44.31.61.70 |
| AMBEServer | — | 127.0.0.1:2460 |
| YSF reflector (lab) | — | see local `ysf2dmrcon-echolink-ysf.ini` |

```bash
# EchoLink <-> DMR
./ysf2dmrcon -c ysf2dmrcon-echolink.ini

# EchoLink <-> YSF
./ysf2dmrcon -c ysf2dmrcon-echolink-ysf.ini
```

Connectivity: set `host=CA5RPY-L` (or a conference like `*REDCHILE*`). The bridge logs into the directory, looks up that callsign in the station list, then sends RTCP SDES to the resolved IP.

## Sources only

This project ships source code. AMBE codecs run in an external process reached by UDP.
