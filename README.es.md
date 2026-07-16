# ysf2dmrcon

**Versión 0.1.0**

Puente de voz **reflector YSF ↔ servidor DMR**. Se registra como peer Homebrew
(estilo MMDVMHost) y como cliente YSF (YSFP + sala DGID).

Compilación autocontenida — código en `hbp/`, `mmdvm/` y `vendor/`.
Referencia upstream: [MMDVM_CM](https://github.com/juribeparada/MMDVM_CM).

**Documentación en inglés:** [README.md](README.md)

## Compilación

```bash
cd /opt/ysf2dmr
make
```

Genera el binario `ysf2dmrcon`. Instalación opcional:

```bash
make install PREFIX=/usr/local
```

## Inicio rápido

1. Copie la plantilla y edite sus datos:

   ```bash
   cp ysf2dmrcon.example.ini ysf2dmrcon.ini
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

## Varias instancias

Un **proceso por puente** (DGID, TG DMR y `dmrid` distintos). Cada instancia
lleva su propio INI:

```bash
./ysf2dmrcon -c ysf2dmrcon-tg71442.ini
./ysf2dmrcon -c ysf2dmrcon-tg71481.ini
```

Pueden compartir la misma base de suscriptores: use el mismo `[aliases] data_dir`
en cada INI (por defecto `./data`).

## Configuración (`ysf2dmrcon.ini`)

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

### `[aliases]` — Base de suscriptores

Misma estructura que `ALIASES` en new-adn-server. Archivos en `data_dir`
(por defecto `./data`).

| Clave | Descripción |
|-------|-------------|
| `try_download` | `1` = descargar al inicio si falta o está obsoleto |
| `stale_minutes` | Re-descargar si el archivo supera esta antigüedad (`0` = siempre al inicio) |
| `data_dir` | Directorio de los JSON (por defecto `./data`) |
| `subscriber_file` / `subscriber_url` | Base principal ID ↔ indicativo |
| `local_subscriber_file` | Superposición local opcional |
| `checksum_file` / `checksum_url` | Manifiesto de checksums opcional |

### `[log]`

`level = DEBUG | INFO | WARNING | ERROR`

## Identidad del locutor

La voz siempre cruza; solo cambia la identidad mostrada/transmitida.

### DMR → YSF

- Origen: **ID DMR → indicativo** desde `subscriber_ids.json` únicamente.
- El DMRA (alias del locutor) se decodifica y registra en DEBUG — **nunca** se usa como indicativo YSF (el usuario puede escribir cualquier texto).
- Si el ID no está en el JSON, se envía el número (muchos radios YSF no lo muestran como indicativo).

### YSF → DMR

1. Se quita el sufijo tras el primer `-` o `/` (`HP3ICC-FT3` → `HP3ICC`).
2. Si el indicativo base está en el JSON → se usa ese indicativo y el **primer**
   ID DMR asociado en el archivo.
3. Si no está en la base → identidad del puente en `[dmr]` (`callsign` + `dmrid`).

## Estructura del proyecto

| Ruta | Función |
|------|---------|
| `ysf2dmrcon.c` | Bucle principal, señales, configuración |
| `peer_dmr.c` / `peer_ysf.c` | Peers UDP, reconexión, DGID/RPTO |
| `bridge.c` | Puente de voz, identidad, ritmo ModeConv |
| `aliases.c` | Carga/descarga/búsqueda de alias JSON |
| `talker_alias.c` | Decodificación DMRA (solo log) |
| `ysf_fich.c` | Códec FICH YSF y reescritura DGID |
| `hbp/dmr_hbp.c` | Autenticación HBP y códec LC/embebido |
| `mmdvm/` | ModeConv + Golay24128 (YSF2DMR de MMDVM_CM) |
| `vendor/yyjson/` | Parser JSON (MIT) |

## Licencia

**GPL v3** (o posterior). Ver [LICENSE](LICENSE).

| Componente | Licencia | Origen |
|------------|----------|--------|
| Aplicación (`ysf2dmrcon`, peers, bridge, config, aliases, wrappers) | GPL-3.0-or-later | Copyright (C) 2026 Rodrigo Pérez, CE5RPY |
| `hbp/` | GPL-3.0-or-later | Doug McLain; Esteban Mackay HP3ICC; MMDVM_CM |
| `ysf_fich.c` | GPL-3.0-or-later | Doug McLain; Esteban Mackay HP3ICC |
| `mmdvm/` | GPL-2.0-or-later | Jonathan Naylor G4KLX; Andy Uribe CA6JAU; [MMDVM_CM](https://github.com/juribeparada/MMDVM_CM) |
| `vendor/yyjson/` | MIT | YaoYuan, [yyjson](https://github.com/ibireme/yyjson) |
