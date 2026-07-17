# ysf2dmrcon

**Versión 0.2.1**

Puente de voz con tres modos:

| Modo | Función |
|------|---------|
| `ysf-dmr` (por defecto) | Reflector YSF ↔ servidor DMR (peer Homebrew + YSFP/DGID) |
| `echolink-dmr` | EchoLink ↔ DMR (GSM/RTP + vocoder AMBE remoto) |
| `echolink-ysf` | EchoLink ↔ YSF (mismo camino EchoLink + vocoder) |

Compilación autocontenida — código en `hbp/`, `mmdvm/` y `vendor/`.
Referencia upstream: [MMDVM_CM](https://github.com/juribeparada/MMDVM_CM).
Detalle EchoLink: [docs/echolink-bridge.md](docs/echolink-bridge.md).

**Documentación en inglés:** [README.md](README.md)

## Compilación

### Dependencias (Debian / Ubuntu)

```bash
sudo apt-get update
sudo apt-get install -y build-essential libssl-dev curl libgsm1
```

| Paquete | Para qué |
|---------|----------|
| `build-essential` | `gcc`, `g++`, `make` |
| `libssl-dev` | OpenSSL (`-lcrypto`) para checksums blake2b de aliases |
| `curl` | Descarga en runtime de JSON de suscriptores / checksums |
| `libgsm1` | Audio GSM EchoLink (`libgsm.so.1`); cabeceras en `vendor/gsm/` |

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

Hace falta un vocoder AMBE remoto (UDP DV3000 / AMBEServer, p. ej. md380-emu en
`127.0.0.1:2460`) y un indicativo EchoLink validado (`-L` / `-R` / conferencia).

```bash
# EchoLink <-> DMR
cp ysf2dmrcon-echolink-dmr.example.ini ysf2dmrcon-echolink-dmr.ini
# editar contraseñas, bind_addr, master DMR, host EchoLink (nodo o *CONF*)
./ysf2dmrcon -c ysf2dmrcon-echolink-dmr.ini

# EchoLink <-> YSF
cp ysf2dmrcon-echolink-ysf.example.ini ysf2dmrcon-echolink-ysf.ini
# editar contraseñas, bind_addr, reflector YSF/DGID, host EchoLink
./ysf2dmrcon -c ysf2dmrcon-echolink-ysf.ini
```

Los `*.ini` locales (con contraseñas) están en `.gitignore`; solo se versionan
los `*.example.ini`. Ver [docs/echolink-bridge.md](docs/echolink-bridge.md).

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

### `[ysf]` — Reflector YSF

| Clave | Descripción |
|-------|-------------|
| `host` | Hostname o IP del reflector |
| `port` | Puerto UDP YSF (habitualmente `42000`) |
| `dgid` | Sala DGID al conectar/reconectar |

**RadioID** en CSD/DCH **DMR→YSF** está fijado a `*****` (default DMR2YSF/YSF2DMR).
No es configurable.

**YSF→DMR (entrante):** muchos portátiles añaden sufijo tras `-` o `/` en el
campo origen del wire (p. ej. `HP3ICC-FT3`, `CE5RPY/FT3`). El puente quita ese
sufijo y usa solo el indicativo base para buscar en el JSON.

### `[dmr]` — Identidad del peer Homebrew

| Clave | Descripción |
|-------|-------------|
| `callsign` | Indicativo del puente (monitor “Bridges”, RPTC) |
| `dmrid` | ID DMR del puente — único por instancia |
| `location` | Texto en Linked Systems del monitor (máx. 20 caracteres) |
| `description` | Etiqueta corta del puente |
| `host` / `port` | Servidor DMR ADN |
| `tg` | Talkgroup de voz obligatorio (DMRD + PTT 1s al conectar); TX siempre TS2 |
| `options` | Cadena RPTO opcional — omitir/vacío = sin RPTO; si se pone, se envía tal cual |
| `password` | Contraseña del peer Homebrew |

**Monitor:** frecuencias RX/TX en cero en RPTC → el monitor muestra N/A
(correcto para un puente software, no un hotspot).

`default_ysf_dmrid` se acepta por compatibilidad pero **no se usa**; si el
locutor YSF no está en la base de datos, se usa el `callsign` + `dmrid` de
esta sección.

En modo **`echolink-ysf`** no se abre peer DMR: `host`/`port`/`password` no
se usan (`password` puede ser un placeholder). `callsign` / `dmrid` siguen
alimentando el RadioID de CSD/DCH YSF. El indicativo de locutor/gateway en el
wire es el `[echolink] callsign` **completo** (p. ej. `CE5RPY-L`), con el mismo
layout CSD/DCH que DMR→YSF.

### `[echolink]` — Estación EchoLink (modos echolink-*)

Puertos **fijos en código** (no van en el INI): UDP **5198** RTP, UDP **5199**
RTCP, TCP **5200** directorio.

| Clave | Descripción |
|-------|-------------|
| `callsign` | Estación EchoLink validada (`N0CALL-L`, `-R` o `*CONF*`) |
| `password` | Contraseña del directorio EchoLink |
| `bind_addr` | IP local para enlazar 5198/5199 |
| `host` | Indicativo de nodo o conferencia a conectar (se resuelve por directorio); IPv4 aún aceptada en lab |
| `qth` / `email` | Metadatos opcionales del directorio |
| `directory_servers` | Hosts de directorio separados por coma (por defecto los cuatro `serverN.echolink.org`) |
| `login_interval` | Periodo de login de presencia (por defecto **360** s, compatible tlb) |
| `station_list_interval` | Refresco de lista / IP del peer (por defecto **600** s) |
| `gain` | Escala lineal PCM **EchoLink → DMR/YSF** (antes del AMBE). Rango **mayor que 0 .. 4**: **1.0** = sin cambio (por defecto), `0.5` ≈ −6 dB, `4.0` = máximo. No afecta DMR/YSF → EchoLink. |
| `log_level` | Nivel opcional del canal (`DEBUG`…`ERROR`); si no, hereda `[log]` |

Login/lista de directorio van en un **hilo en segundo plano** para no bloquear
el audio. EL→DMR/YSF arranca con cualquier PCM entrante (incl. silencio con
PTT); el hangtime sigue la presencia de PCM (~700 ms). Detalle:
[docs/echolink-bridge.md](docs/echolink-bridge.md).

### `[vocoder]` — AMBE remoto (modos echolink-*)

| Clave | Descripción |
|-------|-------------|
| `host` / `port` | Endpoint UDP DV3000 / AMBEServer (lab: md380-emu `127.0.0.1:2460`) |
| `log_level` | Opcional; conviene `WARNING` salvo depurar encode/decode |

Usa RATET **34** (49 bit) con `interleave49` de md380-emu en el cable; ModeConv
ve tramas 49-bit raw desintercaladas.

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

### DMR → YSF

- Origen: **ID DMR → indicativo** desde `subscriber_ids.json` únicamente.
- El DMRA (alias del locutor) se decodifica y registra en DEBUG — **nunca** se usa como indicativo YSF (el usuario puede escribir cualquier texto).
- Si el ID no está en el JSON, se envía el número (muchos radios YSF no lo muestran como indicativo).

### YSF → DMR

1. Se quita el sufijo tras el primer `-` o `/` (`HP3ICC-FT3` → `HP3ICC`).
2. Si el indicativo base está en el JSON → se usa ese indicativo y el **primer**
   ID DMR asociado en el archivo. El índice en memoria son dos tablas contiguas
   open-addressing (`id→indicativo` e `indicativo→id` primario); todos los IDs
   se conservan, así DMR→YSF resuelve p. ej. `7300391` y `7300392` → `CE5RPY`.
3. Si no está en la base → identidad del puente en `[dmr]` (`callsign` + `dmrid`).

### EchoLink → DMR / YSF

- La identidad del locutor es el indicativo **base de la estación EchoLink
  local** (`[echolink] callsign`, cortando tras `-` / `/`), no el `host` remoto.
- El framing CSD/DCH de EL→YSF coincide con DMR→YSF (compatible DMR2YSF).

## Estructura del proyecto

| Ruta | Función |
|------|---------|
| `ysf2dmrcon.c` | Bucle principal, señales, configuración |
| `peer_dmr.c` / `peer_ysf.c` | Peers UDP, reconexión, DGID/RPTO |
| `peer_echolink.c` | Directorio EchoLink, RTP/GSM, RTCP |
| `bridge.c` | Puente YSF↔DMR, identidad, ritmo ModeConv |
| `bridge_el.c` | EchoLink↔DMR / EchoLink↔YSF |
| `vocoder_remote.c` | Cliente UDP DV3000 / AMBEServer |
| `aliases.c` | Carga/descarga/búsqueda de alias JSON |
| `talker_alias.c` | Decodificación DMRA (solo log) |
| `ysf_fich.c` | Códec FICH YSF y reescritura DGID |
| `hbp/dmr_hbp.c` | Autenticación HBP y códec LC/embebido |
| `mmdvm/` | ModeConv + Golay24128 (YSF2DMR de MMDVM_CM) |
| `vendor/yyjson/` | Parser JSON (MIT) |
| `docs/echolink-bridge.md` | Modos EchoLink, puertos, notas de vocoder |

## Licencia

**GPL v3** (o posterior). Ver [LICENSE](LICENSE).

| Componente | Licencia | Origen |
|------------|----------|--------|
| Aplicación (`ysf2dmrcon`, peers, bridge, config, aliases, wrappers) | GPL-3.0-or-later | Copyright (C) 2026 Rodrigo Pérez, CE5RPY |
| `hbp/` | GPL-3.0-or-later | Doug McLain; Esteban Mackay HP3ICC; MMDVM_CM |
| `ysf_fich.c` | GPL-3.0-or-later | Doug McLain; Esteban Mackay HP3ICC |
| `mmdvm/` | GPL-2.0-or-later | Jonathan Naylor G4KLX; Andy Uribe CA6JAU; [MMDVM_CM](https://github.com/juribeparada/MMDVM_CM) |
| `vendor/yyjson/` | MIT | YaoYuan, [yyjson](https://github.com/ibireme/yyjson) |
