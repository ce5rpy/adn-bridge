# adn-bridge

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

Genera el binario `adn-bridge`. Instalación opcional:

```bash
sudo make install
# → /opt/adn-bridge/adn-bridge
# → /opt/adn-bridge/config/*.example.ini
# → /opt/adn-bridge/data/
```

## Inicio rápido

1. Copie una plantilla a `config/` (instalación: `/opt/adn-bridge/config/`):

   ```bash
   mkdir -p config
   # Master (todas las secciones; elija mode=) — punto de partida recomendado
   cp examples/adn-bridge.example.ini config/adn-bridge.ini

   # O un archivo listo por modo:
   # cp examples/adn-bridge-ysf-dmr.example.ini config/adn-bridge.ini
   # cp examples/adn-bridge-echolink-dmr.example.ini config/adn-bridge.ini
   # cp examples/adn-bridge-echolink-ysf.example.ini config/adn-bridge.ini
   ```

| Plantilla | Modo |
|-----------|------|
| `examples/adn-bridge.example.ini` | Master — todas las claves, elija `mode=` |
| `examples/adn-bridge-ysf-dmr.example.ini` | `ysf-dmr` |
| `examples/adn-bridge-echolink-dmr.example.ini` | `echolink-dmr` |
| `examples/adn-bridge-echolink-ysf.example.ini` | `echolink-ysf` |

2. Ejecute con la ruta del INI:

   ```bash
   ./adn-bridge -c config/adn-bridge.ini
   # instalado: /opt/adn-bridge/adn-bridge -c /opt/adn-bridge/config/adn-bridge.ini
   ```

Los modos EchoLink necesitan vocoder AMBE por hardware e indicativo `-L` /
`-R` / conferencia — [docs/echolink-bridge.es.md](docs/echolink-bridge.es.md)
([EN](docs/echolink-bridge.md)).

Los INI locales en `config/` están en `.gitignore`; las plantillas viven en
`examples/*.example.ini`.

## Varias instancias

Un **proceso por puente** (DGID, TG DMR y `dmrid` distintos). Cada instancia
lleva su propio INI bajo `config/`:

```bash
./adn-bridge -c config/adn-bridge-tg71442.ini
./adn-bridge -c config/adn-bridge-tg71481.ini
```

Pueden compartir la misma base de suscriptores: use el mismo `[aliases] data_dir`
en cada INI (por defecto `./data`, o `/opt/adn-bridge/data` al instalar).

### Configuración interactiva + systemd

```bash
./examples/generate-config.sh --lang es
# genera adn-bridge-<instancia>.ini (+ unit .service opcional)
```

Enter acepta el valor sugerido (URLs de alias ADN, puertos, etc.).

Ejemplos systemd en `examples/`:

| Archivo | Uso |
|---------|-----|
| `adn-bridge.service` | Una instancia (editar rutas) |
| `adn-bridge@.service` | Plantilla: `systemctl enable --now adn-bridge@redchile` → INI `config/adn-bridge-redchile.ini` |

```bash
sudo make install
# INI: /opt/adn-bridge/config/adn-bridge-redchile.ini
sudo cp examples/adn-bridge@.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now adn-bridge@redchile.service
```

## Configuración

El modo se elige en `[bridge]`:

```ini
[bridge]
mode = ysf-dmr          ; por defecto — YSF <-> DMR
mode = echolink-dmr     ; EchoLink <-> DMR
mode = echolink-ysf     ; EchoLink <-> YSF
```

Plantillas: master `examples/adn-bridge.example.ini` (todos los modos) y un archivo
por modo en `examples/adn-bridge-*.example.ini`.

### `[ysf]` / `[dmr]` — YSF ↔ DMR

Claves mínimas: YSF `host`/`port`/`dgid`; DMR `callsign`/`dmrid`/`host`/`port`/
`tg`/`password` (`options` RPTO opcional). TX siempre TS2.
Opcional `clear_dynamic_tg = 1`: al login DMR, PTT silencio a **TG 4000**
primero (quita TGs dinámicos), luego el PTT de conexión a `tg`.

Guía de puesta en marcha:

→ **[docs/ysf-dmr-bridge.es.md](docs/ysf-dmr-bridge.es.md)**
([English](docs/ysf-dmr-bridge.md))

En modo **`echolink-ysf`** omita `[dmr]` por completo (sin peer DMR). La
identidad de gateway YSF sale de `[echolink] callsign` — ver
[docs/echolink-bridge.es.md](docs/echolink-bridge.es.md).

### `[echolink]` / `[vocoder]` — Modos EchoLink

Claves mínimas: `callsign`, `password`, `bind_addr`, `host` (nodo o `*CONF*`)
y `[vocoder] host`/`port`. Puertos **5198/5199/5200** en modo directo.
Proxy EchoLink opcional: `proxy_server` / `proxy_port` / `proxy_password`
(por defecto `PUBLIC`) — entonces no hace falta `bind_addr`.

Guía de puesta en marcha (puertos, proxy, vocoder, INI, comprobar audio):

→ **[docs/echolink-bridge.es.md](docs/echolink-bridge.es.md)**
([English](docs/echolink-bridge.md))

### `[aliases]` — Base de suscriptores


Misma estructura que `ALIASES` en adn-server. Archivos en `data_dir`
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
| `adn_bridge.c` | Bucle principal, señales, configuración |
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
| `examples/` | `*.example.ini`, units systemd, `generate-config.sh` |
| `config/` | INIs de instancia locales (gitignored; `make install` → `/opt/adn-bridge/config/`) |
| `data/` | Caché de alias (gitignored; `make install` → `/opt/adn-bridge/data/`) |

## Agradecimientos

Gracias a **[Esteban Mackay, HP3ICC](https://gitlab.com/hp3icc)**, por las ideas, ayudas, pruebas y correcciones a lo largo del desarrollo.

## Licencia

**GPL v3** (o posterior). Ver [LICENSE](LICENSE).

| Componente | Licencia | Origen |
|------------|----------|--------|
| Aplicación (`adn-bridge`, peers, bridge, config, aliases, wrappers) | GPL-3.0-or-later | Copyright (C) 2026 Rodrigo Pérez, CE5RPY |
| `hbp/` | GPL-3.0-or-later | Doug McLain; Esteban Mackay HP3ICC; MMDVM_CM |
| `ysf_fich.c` | GPL-3.0-or-later | Doug McLain; Esteban Mackay HP3ICC |
| `mmdvm/` | GPL-2.0-or-later | Jonathan Naylor G4KLX; Andy Uribe CA6JAU; [MMDVM_CM](https://github.com/juribeparada/MMDVM_CM) |
| `vendor/yyjson/` | MIT | YaoYuan, [yyjson](https://github.com/ibireme/yyjson) |
