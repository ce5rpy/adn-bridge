# ysf2dmrcon

Standalone **YSF reflector ↔ DMR server** voice bridge at `/opt/ysf2dmr`.
Registers as a Homebrew DMR peer (MMDVMHost-style login) and as a YSF client
(YSFP + DGID room).

Self-contained build — vendored code under `hbp/`, `mmdvm/`, and `vendor/`.
Upstream reference: [MMDVM_CM](https://github.com/juribeparada/MMDVM_CM).

## Build

```bash
cd /opt/ysf2dmr
make
```

Produces `ysf2dmrcon`.

## Run

Copy `ysf2dmrcon.example.ini` to `ysf2dmrcon.ini` (same directory as the binary
or current working directory), edit YSF/DMR settings, then:

```bash
cd /opt/ysf2dmr
./ysf2dmrcon
```

Optional: `./ysf2dmrcon -c /path/to/config.ini`

## Layout

| Path | Role |
|------|------|
| `ysf2dmrcon.c` | Main loop, config |
| `peer_dmr.c` / `peer_ysf.c` | UDP peers, reconnect, DGID/RPTO |
| `bridge.c` | Voice bridge (ModeConv pacing) |
| `ysf_fich.c` | YSF FICH codec + DGID rewrite |
| `hbp/dmr_hbp.c` | DMR HBP auth + LC/embedded codec (from dmrcon) |
| `mmdvm/` | ModeConv + Golay24128 (from MMDVM_CM YSF2DMR) |
| `vendor/yyjson/` | JSON parser for alias files (MIT) |

## License

**GPL v3** (or later). See [LICENSE](LICENSE).

| Component | License | Provenance |
|-----------|---------|------------|
| Application (`ysf2dmrcon`, peers, bridge, config, aliases, wrappers) | GPL-3.0-or-later | Copyright (C) 2026 Rodrigo Pérez, CE5RPY |
| `hbp/` | GPL-3.0-or-later | Doug McLain / HP3ICC dmrcon; MMDVM_CM |
| `ysf_fich.c` | GPL-3.0-or-later | Doug McLain / HP3ICC dgidcon |
| `mmdvm/` | GPL-2.0-or-later | Jonathan Naylor G4KLX / Andy Uribe CA6JAU, [MMDVM_CM](https://github.com/juribeparada/MMDVM_CM) |
| `vendor/yyjson/` | MIT | YaoYuan, [yyjson](https://github.com/ibireme/yyjson) |

