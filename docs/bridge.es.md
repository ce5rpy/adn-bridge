# adn-bridge — configuración y operación

Guía completa para todos los layouts: **YSF ↔ DMR**, **EchoLink ↔ DMR** y
**EchoLink ↔ YSF**.

Compilación / instalación: [README.es.md](../README.es.md) · [README.md](../README.md).
**English:** [bridge.md](bridge.md).

## Resumen

Configure **exactamente dos** `[peer.<nombre>]` habilitados. Secciones globales:
`[aliases]`, `[log]`. Los peers EchoLink llevan `vocoder_host` / `vocoder_port` inline.

| Layout | Peers | Vocoder |
|--------|-------|---------|
| YSF ↔ DMR | `ysf` + `dmr` | no requerido |
| EchoLink ↔ DMR | `echolink` + `dmr` | obligatorio |
| EchoLink ↔ YSF | `echolink` + `ysf` | obligatorio |

Plantillas en `examples/`:

| Archivo | Layout |
|---------|--------|
| `adn-bridge-ysf-dmr.example.ini` | YSF ↔ DMR |
| `adn-bridge-echolink-dmr.example.ini` | EchoLink ↔ DMR |
| `adn-bridge-echolink-ysf.example.ini` | EchoLink ↔ YSF |
| `adn-bridge.example.ini` | Master (YSF+DMR; EchoLink comentado) |

Generador interactivo: `./examples/generate-config.sh` (`--lang es`).

## Arranque rápido

```bash
mkdir -p config
cp examples/adn-bridge-ysf-dmr.example.ini config/adn-bridge.ini
# editar peers, luego:
./adn-bridge -c config/adn-bridge.ini
# instalado: /opt/adn-bridge/adn-bridge -c /opt/adn-bridge/config/adn-bridge.ini
```

Layouts EchoLink:

```bash
cp examples/adn-bridge-echolink-dmr.example.ini config/adn-bridge-el-dmr.ini
cp examples/adn-bridge-echolink-ysf.example.ini config/adn-bridge-el-ysf.ini
```

Varias instancias: un proceso por puente (DGID, TG y `dmrid` distintos), cada
uno con su INI. Pueden compartir el mismo `[aliases] data_dir`.

---

## YSF ↔ DMR

### Requisitos

1. Reflector YSF (host, puerto, DGID).
2. Master DMR con contraseña Homebrew y talkgroup.
3. `callsign` + `dmrid` únicos por instancia.
4. No requiere vocoder AMBE externo.

### Ejemplo

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

Líneas útiles: `YSF->DMR` / `DMR->YSF` call start/end.

---

## EchoLink ↔ DMR / EchoLink ↔ YSF

### Requisitos

1. Estación EchoLink validada (`CALL-L`, `CALL-R` o conferencia).
2. Vocoder AMBE por hardware vía UDP (DV3000 / AMBEServer).
3. Red — **directo:** UDP **5198–5199** entrante + TCP **5200** al directorio;
   **proxy:** TCP saliente al EchoLink Proxy (puerto **8100** por defecto), sin UDP local.

Al enlazar debería aparecer `linked to … (RTCP SDES)` en el log.

### EchoLink Proxy (detrás de NAT)

```ini
[peer.el]
type = echolink
enabled = true
callsign = N0CALL-L
password = tu-password-directorio
host = *ALGUNACONF*
proxy_server = tu.proxy.ejemplo
proxy_port = 8100
proxy_password = PUBLIC
```

Sin claves `proxy_*` → modo directo (`bind_addr` obligatorio). Con proxy,
busca `echolink: connected to proxy …` / `using proxy …`.

### Vocoder (en `[peer.el]`)

AMBE por hardware (DV3000 / AMBEServer) — en el peer que lo requiere (EchoLink
hoy; D-Star etc. después). Claves obligatorias:

```ini
[peer.el]
type = echolink
...
vocoder_host = 127.0.0.1
vocoder_port = 2460
# vocoder_log_level = INFO   # opcional; sobrescribe [log] del canal vocoder
```

`vocoder_port` por defecto **2460** si se omite; `vocoder_host` es obligatorio.

Sin vocoder accesible no hay voz entre EchoLink y DMR/YSF.

### Checklist de log (EchoLink → DMR/YSF)

| Paso | Texto en el log | Significado |
|------|-----------------|-------------|
| 0 | `connected to proxy` / `using proxy` | Modo proxy — TCP OK |
| 1 | `directory login OK` | Login al directorio OK |
| 2 | `connecting to …` | IP de `host` resuelta |
| 3 | `linked to … (RTCP SDES)` | Enlazado (solo control) |
| 4 | `RTP RX` o `EL audio rms=` | Llega audio EchoLink |
| 5 | `EL->DMR call start` / `EL->YSF call start` | Llamada hacia radio |
| 6 | `vocoder ENC` / `vocoder ready at` | Vocoder hardware activo |
| 7 | `EL->DMR call end` / `EL->YSF call end` | Fin de llamada |

Inverso: `DMR->EL call start` / `YSF->EL call start`, luego `vocoder DEC`.

`linked to …` sin `RTP RX` / `call start` = enlazado en silencio (nadie TX en
ese nodo/conferencia).

Filtrar en vivo:

```bash
./adn-bridge -c tu.ini 2>&1 | grep -E 'linked|RTP|EL audio|EL->|YSF->EL|DMR->EL|vocoder|call start|call end'
```

---

## Referencia de peers

### Claves comunes (todos los `[peer.*]`)

| Clave | Obligatoria | Descripción |
|-------|-------------|-------------|
| `type` | sí | `dmr`, `ysf` o `echolink` (alias `el`) |
| `enabled` | no | Por defecto **true** |
| `log_level` | no | Sobrescribe `[log] level` del canal del peer |

### `type = ysf`

| Clave | Obligatoria | Descripción |
|-------|-------------|-------------|
| `host` / `port` | sí | Reflector (puerto habitual `42000`) |
| `callsign` | sí | Indicativo gateway en el reflector |
| `dgid` | sí | Sala 0–99 |

### `type = dmr`

| Clave | Obligatoria | Descripción |
|-------|-------------|-------------|
| `callsign` | sí | Indicativo del puente (monitor) |
| `dmrid` | sí | ID DMR único por proceso |
| `host` / `port` | sí | Master DMR |
| `tg` | sí | Talkgroup de voz (TX siempre **TS2**) |
| `password` | sí | Contraseña peer Homebrew |
| `options` | no | Cadena RPTO; omitir/vacío = sin RPTO (típico en XLX) |
| `location` / `description` | no | Texto en el monitor |
| `clear_dynamic_tg` | no | Ver abajo |

**`clear_dynamic_tg`** — opcional, **por defecto desactivado** (omitir la clave
o `= 0`).

- `1` — al login DMR: PTT silencio a **TG 4000** primero (quita TGs dinámicos
  residuales del peer), luego PTT de conexión a `tg`.
- Recomendado en masters ADN con TGs dinámicos; omitir en XLX y masters sin
  OPTIONS/RPTO.

Frecuencias RX/TX en cero en RPTC → el monitor muestra N/A (normal en puentes
software).

### `type = echolink`

| Clave | Obligatoria | Descripción |
|-------|-------------|-------------|
| `callsign` | sí | Tu estación EchoLink |
| `password` | sí | Contraseña del directorio |
| `bind_addr` | sí* | IP local UDP 5198/5199 (*no si hay `proxy_server`) |
| `host` | sí | Nodo o `*CONF*` a enlazar |
| `qth` / `email` | no | Metadatos del directorio |
| `directory_servers` | no | Por defecto: `serverN.echolink.org` públicos |
| `login_interval` | no | Por defecto **360** s |
| `station_list_interval` | no | Por defecto **600** s |
| `gain` | no | Escala PCM EchoLink → DMR/YSF antes del AMBE; defecto **1.0**;
  sugerido **0.5** en EchoLink↔YSF, **1.0** en EchoLink↔DMR; rango **0**–**4** |
| `vocoder_host` | sí | Host del servidor AMBE (DV3000 / AMBEServer) |
| `vocoder_port` | sí* | Habitualmente `2460` (*defecto **2460** si se omite la clave) |
| `vocoder_log_level` | no | Sobrescribe `[log]` del canal vocoder |
| `proxy_server` / `proxy_port` / `proxy_password` | no | EchoLink Proxy (puerto **8100**, password **PUBLIC**) |

Puertos **5198 / 5199 / 5200** fijos en código (no son claves INI).

---

## `[aliases]`

Opcional; recomendable para indicativo / ID DMR correctos.

- Archivos en `data_dir` (por defecto `./data`), mismo modelo que `ALIASES` de adn-server.
- Varios procesos pueden compartir un `data_dir`.

**Reglas de identidad:**

- **DMR → YSF:** busca ID en `subscriber_ids.json`; si falta, envía el número.
- **YSF → DMR:** quita sufijo tras `-` o `/`, busca; si no está, `callsign` + `dmrid` del puente.
- **EchoLink → DMR/YSF:** nombre del locutor vía RTCP SDES; ID DMR vía alias.

---

## `[log]`

`level = INFO` en uso normal. `log_level = DEBUG` en el peer al depurar.

Ejemplo (EchoLink ↔ DMR):

```ini
[log]
level = INFO

[peer.el]
log_level = DEBUG

[peer.master]
log_level = DEBUG
```

Formato de línea:

```text
2026-07-17 14:33:59,754 INFO/echolink: echolink: linked to *REDCHILE* (RTCP SDES)
```

Con systemd: `journalctl -u adn-bridge@instancia -f`.

---

## Diagnóstico

### YSF ↔ DMR

| Síntoma | Revisar |
|---------|---------|
| Sin audio en ningún sentido | ¿DMR conectado? ¿YSF enlazado? logs DEBUG `call_active` |
| ID de locutor incorrecto | `[aliases]` data_dir y `subscriber_ids.json` |
| Solo PTT de conexión DMR | `tg`, `password`, `options` / tipo de master |

### Layouts EchoLink

| Falta en el log | Revisar |
|-----------------|---------|
| `connected to proxy` | `proxy_server`, puerto, password, TCP saliente |
| `directory login OK` | `callsign`/`password`, TCP 5200 o proxy |
| `station list: … not found` | ortografía de `host` / presencia en directorio |
| Solo `linked to …` | UDP/NAT o proxy; nadie TX en la conferencia |
| `vocoder ENC timeout` | `[peer.el] vocoder_host`/`vocoder_port`, hardware AMBE |
| `call start` pero silencio en DMR/YSF | `tg`/password del peer o host/DGID YSF |

---

## Notas

- Half-duplex: una dirección de ingress activa a la vez.
- Distribución solo fuente; rutas EchoLink usan vocoder AMBE externo.
