# ysf2dmrcon

**Versión 0.3.1**

Puente de voz con tres modos:

| Modo | Función |
|------|---------|
| `ysf-dmr` (por defecto) | Reflector YSF ↔ servidor DMR (peer Homebrew + YSFP/DGID) |
| `echolink-dmr` | EchoLink ↔ DMR (GSM/RTP + vocoder AMBE por hardware) |
| `echolink-ysf` | EchoLink ↔ YSF (mismo camino EchoLink + vocoder por hardware) |

Compilación autocontenida — código en `hbp/`, `mmdvm/` y `vendor/`.
Referencia upstream: [MMDVM_CM](https://github.com/juribeparada/MMDVM_CM).
Detalle YSF↔DMR: [docs/ysf-dmr-bridge.es.md](docs/ysf-dmr-bridge.es.md)
([EN](docs/ysf-dmr-bridge.md)).
Detalle EchoLink: [docs/echolink-bridge.es.md](docs/echolink-bridge.es.md)
([EN](docs/echolink-bridge.md)).

**Documentación en inglés:** [README.md](README.md)

## Compilación

### Dependencias (Debian / Ubuntu)

```bash
sudo apt-get update
sudo apt-get install -y build-essential libssl-dev zlib1g-dev libgsm1-dev curl
```

| Paquete | Para qué |
|---------|----------|
| `build-essential` | `gcc`, `g++`, `make` |
| `libssl-dev` | OpenSSL (`-lcrypto`) para checksums blake2b de aliases |
| `zlib1g-dev` | zlib (`-lz`) para descomprimir la station-list EchoLink |
| `libgsm1-dev` | GSM EchoLink (`-lgsm`); trae `libgsm1` en runtime; cabeceras también en `vendor/gsm/` |
| `curl` | Descarga en runtime de JSON de suscriptores / checksums |

yyjson y ModeConv (MMDVM) van vendored; no hacen falta más paquetes apt.

### Compilar

```bash
make
```

Genera el binario `ysf2dmrcon`. Instalación opcional:

```bash
make install PREFIX=/usr/local
```

## Inicio rápido

### YSF ↔ DMR (por defecto)

1. Copie la plantilla y edite sus datos:

   ```bash
   cp ysf2dmrcon.example.ini ysf2dmrcon.ini
   # misma plantilla con nombre explícito:
   # cp ysf2dmrcon-ysf-dmr.example.ini ysf2dmrcon-ysf-dmr.ini
   ```

2. Configure reflector YSF, DGID, servidor DMR, contraseña y talkgroup en
   `OPTIONS`.

3. Ejecute desde el directorio del INI (o use `-c`):

   ```bash
   ./ysf2dmrcon
   # o
   ./ysf2dmrcon -c /ruta/a/ysf2dmrcon.ini
   ```

4. En producción use `[log] level = INFO` (o `WARNING`).

### Modos EchoLink

Hace falta un vocoder AMBE por hardware (UDP, protocolo DV3000 / AMBEServer)
y un indicativo EchoLink validado (`-L` / `-R` / conferencia).

```bash
# EchoLink <-> DMR
cp ysf2dmrcon-echolink-dmr.example.ini ysf2dmrcon-echolink-dmr.ini
# editar contraseñas, bind_addr (o proxy_*), master DMR, host EchoLink (nodo o *CONF*)
./ysf2dmrcon -c ysf2dmrcon-echolink-dmr.ini

# EchoLink <-> YSF
cp ysf2dmrcon-echolink-ysf.example.ini ysf2dmrcon-echolink-ysf.ini
# editar contraseñas, bind_addr (o proxy_*), reflector YSF/DGID, host EchoLink
./ysf2dmrcon -c ysf2dmrcon-echolink-ysf.ini
```

Los `*.ini` locales (con contraseñas) están en `.gitignore`; solo se versionan
los `*.example.ini`. Ver [docs/echolink-bridge.es.md](docs/echolink-bridge.es.md).

## Varias instancias

Un **proceso por puente** (DGID, TG DMR y `dmrid` distintos). Cada instancia
lleva su propio INI:

```bash
./ysf2dmrcon -c ysf2dmrcon-tg71442.ini
./ysf2dmrcon -c ysf2dmrcon-tg71481.ini
```

Pueden compartir la misma base de suscriptores: use el mismo `[aliases] data_dir`
en cada INI (por defecto `./data`).

## Configuración

El modo se elige en `[bridge]`:

```ini
[bridge]
mode = ysf-dmr          ; por defecto — YSF <-> DMR
mode = echolink-dmr     ; EchoLink <-> DMR
mode = echolink-ysf     ; EchoLink <-> YSF
```

Plantillas: `ysf2dmrcon.example.ini` / `ysf2dmrcon-ysf-dmr.example.ini` (YSF↔DMR),
`ysf2dmrcon-echolink-dmr.example.ini`, `ysf2dmrcon-echolink-ysf.example.ini`.

### `[ysf]` / `[dmr]` — YSF ↔ DMR

Claves mínimas: YSF `host`/`port`/`dgid`; DMR `callsign`/`dmrid`/`host`/`port`/
`tg`/`password` (`options` RPTO opcional). TX siempre TS2.
Opcional `clear_dynamic_tg = 1`: al login DMR, PTT silencio a **TG 4000**
primero (quita TGs dinámicos), luego el PTT de conexión a `tg`.

Guía de puesta en marcha:

→ **[docs/ysf-dmr-bridge.es.md](docs/ysf-dmr-bridge.es.md)**
([English](docs/ysf-dmr-bridge.md))

En modo **`echolink-ysf`** no se abre peer DMR: `host`/`port`/`password` no
se usan — ver [docs/echolink-bridge.es.md](docs/echolink-bridge.es.md).

### `[echolink]` / `[vocoder]` — Modos EchoLink

Claves mínimas: `callsign`, `password`, `bind_addr`, `host` (nodo o `*CONF*`)
y `[vocoder] host`/`port`. Puertos **5198/5199/5200** en modo directo.
Proxy EchoLink opcional: `proxy_server` / `proxy_port` / `proxy_password`
(por defecto `PUBLIC`) — entonces no hace falta `bind_addr`.

Guía de puesta en marcha (puertos, proxy, vocoder, INI, comprobar audio):

→ **[docs/echolink-bridge.es.md](docs/echolink-bridge.es.md)**
([English](docs/echolink-bridge.md))

### `[aliases]` — Base de suscriptores


Misma estructura que `ALIASES` en new-adn-server. Archivos en `data_dir`
(por defecto `./data`).

| Clave | Descripción |
|-------|-------------|
| `stale_minutes` | Re-descargar si supera esta antigüedad; también en runtime (por defecto `1440` = 24 h; `0` = solo al inicio, siempre) |
| `reload_minutes` | Cada cuánto mirar si el JSON en disco es más nuevo que la RAM y reconstruir; si falta el archivo, fuerza descarga (`0` = off; default `15`) |
| `data_dir` | Directorio de los JSON (por defecto `./data`) |
| `subscriber_file` / `subscriber_url` | Base principal ID ↔ indicativo |
| `local_subscriber_file` | Superposición local opcional |
| `checksum_file` / `checksum_url` | Manifiesto blake2b opcional (igual que adn-server). Si existe, `subscriber_ids` debe coincidir; si falta, se acepta un JSON válido |

### `[log]`

`level = DEBUG | INFO | WARNING | ERROR`

Overrides opcionales por canal (también `log_level=` en cada sección, o
`dmr=` / `echolink=` / `ysf=` / `vocoder=` bajo `[log]`):

```text
2026-07-17 14:33:59,754 INFO/ysf: EL->YSF call start (src CE5RPY    )
```

## Identidad del locutor

La voz siempre cruza; solo cambia la identidad mostrada/transmitida.

- **YSF ↔ DMR:** [docs/ysf-dmr-bridge.es.md](docs/ysf-dmr-bridge.es.md)
  ([EN](docs/ysf-dmr-bridge.md))
- **EchoLink → DMR / YSF:** RTCP SDES entrante; ver
  [docs/echolink-bridge.es.md](docs/echolink-bridge.es.md)
  ([EN](docs/echolink-bridge.md))

## Estructura del proyecto

| Ruta | Función |
|------|---------|
| `ysf2dmrcon.c` | Bucle principal, señales, configuración |
| `peer_dmr.c` / `peer_ysf.c` | Peers UDP, reconexión, DGID/RPTO |
| `peer_echolink.c` / `el_proxy.c` | Directorio EchoLink, RTP/GSM, RTCP, proxy opcional |
| `bridge.c` | Puente YSF↔DMR, identidad, ritmo ModeConv |
| `bridge_el.c` | EchoLink↔DMR / EchoLink↔YSF |
| `vocoder_remote.c` | Cliente UDP DV3000 / AMBEServer |
| `aliases.c` | Carga/descarga/búsqueda de alias JSON |
| `talker_alias.c` | Decodificación DMRA (solo log) |
| `ysf_fich.c` | Códec FICH YSF y reescritura DGID |
| `hbp/dmr_hbp.c` | Autenticación HBP y códec LC/embebido |
| `mmdvm/` | ModeConv + Golay24128 (YSF2DMR de MMDVM_CM) |
| `vendor/yyjson/` | Parser JSON (MIT) |
| `docs/ysf-dmr-bridge.md` / `.es.md` | Guía YSF↔DMR |
| `docs/echolink-bridge.md` / `.es.md` | Guía EchoLink |

## Licencia

**GPL v3** (o posterior). Ver [LICENSE](LICENSE).

| Componente | Licencia | Origen |
|------------|----------|--------|
| Aplicación (`ysf2dmrcon`, peers, bridge, config, aliases, wrappers) | GPL-3.0-or-later | Copyright (C) 2026 Rodrigo Pérez, CE5RPY |
| `hbp/` | GPL-3.0-or-later | Doug McLain; Esteban Mackay HP3ICC; MMDVM_CM |
| `ysf_fich.c` | GPL-3.0-or-later | Doug McLain; Esteban Mackay HP3ICC |
| `mmdvm/` | GPL-2.0-or-later | Jonathan Naylor G4KLX; Andy Uribe CA6JAU; [MMDVM_CM](https://github.com/juribeparada/MMDVM_CM) |
| `vendor/yyjson/` | MIT | YaoYuan, [yyjson](https://github.com/ibireme/yyjson) |
