# adn-bridge

**Versión 0.3.1**

Puente de voz — configure dos peers `[peer.*]`:

| Layout | Peers |
|--------|-------|
| YSF ↔ DMR | `ysf` + `dmr` |
| EchoLink ↔ DMR | `echolink` + `dmr` (`vocoder_host`/`port` en `[peer.el]`) |
| EchoLink ↔ YSF | `echolink` + `ysf` (`vocoder_host`/`port` en `[peer.el]`) |

Compilación autocontenida — código en `hbp/`, `mmdvm/` y `vendor/`.
Referencia upstream: [MMDVM_CM](https://github.com/juribeparada/MMDVM_CM).
Guía completa: [docs/bridge.es.md](docs/bridge.es.md) ([EN](docs/bridge.md)).

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
   # Plantilla master (peers YSF+DMR; EchoLink comentado)
   cp examples/adn-bridge.example.ini config/adn-bridge.ini

   # O un layout listo para usar:
   # cp examples/adn-bridge-ysf-dmr.example.ini config/adn-bridge.ini
   # cp examples/adn-bridge-echolink-dmr.example.ini config/adn-bridge.ini
   # cp examples/adn-bridge-echolink-ysf.example.ini config/adn-bridge.ini
   ```

| Plantilla | Layout |
|-----------|--------|
| `examples/adn-bridge.example.ini` | Master — peers YSF+DMR (EchoLink comentado) |
| `examples/adn-bridge-ysf-dmr.example.ini` | YSF ↔ DMR |
| `examples/adn-bridge-echolink-dmr.example.ini` | EchoLink ↔ DMR |
| `examples/adn-bridge-echolink-ysf.example.ini` | EchoLink ↔ YSF |

2. Ejecute con la ruta del INI:

   ```bash
   ./adn-bridge -c config/adn-bridge.ini
   # instalado: /opt/adn-bridge/adn-bridge -c /opt/adn-bridge/config/adn-bridge.ini
   ```

Los modos EchoLink necesitan vocoder AMBE por hardware e indicativo `-L` /
`-R` / conferencia — [docs/bridge.es.md](docs/bridge.es.md)
([EN](docs/bridge.md)).

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

Defina **dos** `[peer.<nombre>]` habilitados. Secciones globales: `[aliases]`,
`[log]`. Los peers EchoLink requieren `vocoder_host` y `vocoder_port`.

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

Mezclas válidas: `dmr+ysf`, `echolink+dmr`, `echolink+ysf` (un peer de cada
tipo, ambos habilitados).

Plantillas: `examples/adn-bridge.example.ini` y un archivo por layout en
`examples/adn-bridge-*.example.ini`.

### Peers YSF ↔ DMR

`type = ysf` o `type = dmr` en `[peer.*]`. Claves YSF: `host`/`port`/`callsign`/
`dgid`. DMR: `callsign`/`dmrid`/`host`/`port`/`tg`/`password` (`options` RPTO
opcional). TX siempre TS2. Opcional `clear_dynamic_tg = 1`.

→ **[docs/bridge.es.md](docs/bridge.es.md)** ([EN](docs/bridge.md))
— secciones *YSF ↔ DMR* y *Referencia de peers*.

### Peers EchoLink

`type = echolink` con `callsign`, `password`, `bind_addr` (o `proxy_*`), `host`.
`vocoder_host` / `vocoder_port` en cada `[peer.el]` (layouts EchoLink).

→ **[docs/bridge.es.md](docs/bridge.es.md)** ([EN](docs/bridge.md))
— secciones *EchoLink* y *Diagnóstico*.

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

Ver [docs/bridge.es.md](docs/bridge.es.md) ([EN](docs/bridge.md)) — *Alias de suscriptores* e *identidad*.

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
| `docs/bridge.md` / `bridge.es.md` | Configuración y operación (todos los layouts) |
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
