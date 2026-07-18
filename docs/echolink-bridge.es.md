# Puente EchoLink (ysf2dmrcon)

Cómo poner en marcha **`echolink-dmr`** (EchoLink ↔ DMR) y **`echolink-ysf`** (EchoLink ↔ YSF).

Compilación / instalación: [README.es.md](../README.es.md) · [README.md](../README.md).
**English:** [echolink-bridge.md](echolink-bridge.md).
YSF ↔ DMR (sin EchoLink): [ysf-dmr-bridge.es.md](ysf-dmr-bridge.es.md).

## Requisitos

1. Estación EchoLink validada (`CALL-L`, `CALL-R`, o indicativo de conferencia si hospedas).
2. Vocoder AMBE por hardware accesible por UDP (protocolo DV3000 / AMBEServer).
3. Firewall / NAT: permitir **UDP 5198–5199** (y salida **TCP 5200** al directorio EchoLink).

## Arranque rápido

```bash
# EchoLink <-> DMR
cp ysf2dmrcon-echolink-dmr.example.ini ysf2dmrcon-echolink-dmr.ini
# editar: contraseñas, bind_addr, master DMR, host EchoLink, vocoder
./ysf2dmrcon -c ysf2dmrcon-echolink-dmr.ini

# EchoLink <-> YSF
cp ysf2dmrcon-echolink-ysf.example.ini ysf2dmrcon-echolink-ysf.ini
# editar: contraseñas, bind_addr, reflector YSF/DGID, host EchoLink, vocoder
./ysf2dmrcon -c ysf2dmrcon-echolink-ysf.ini
```

Cuando el peer se resuelve y enlaza deberías ver algo como
`linked to … (RTCP SDES)`.

## Modo

```ini
[bridge]
mode = echolink-dmr     ; o echolink-ysf
```

## `[echolink]` — qué configurar

| Clave | Obligatoria | Descripción |
|-------|-------------|-------------|
| `callsign` | sí | Tu estación EchoLink |
| `password` | sí | Contraseña del directorio |
| `bind_addr` | sí | IP local de UDP 5198/5199 |
| `host` | sí | Nodo (`CALL-L` / `CALL-R`) o conferencia (`*NOMBRE*`) a la que conectar |
| `qth` / `email` | no | Metadatos opcionales del directorio |
| `directory_servers` | no | Por defecto los `serverN.echolink.org` públicos |
| `login_interval` | no | Refresco de presencia (por defecto **360** s) |
| `station_list_interval` | no | Cada cuánto refrescar la IP del peer desde la lista (por defecto **600** s) |
| `gain` | no | Escala de audio **EchoLink → DMR/YSF** antes del AMBE. `1.0` = sin cambios (defecto); sugerido **0.5** en `echolink-ysf` y **1.0** en `echolink-dmr`; rango aceptado mayor que **0** y hasta **4**. No afecta DMR/YSF → EchoLink. |
| `log_level` | no | Si no, hereda `[log]` |

Los puertos **5198 / 5199 / 5200** están fijos en el código (no son claves del INI).

## `[vocoder]`

```ini
[vocoder]
host = 127.0.0.1
port = 2460
```

Apunta `host`/`port` a tu vocoder por hardware. Sin él no hay voz entre EchoLink y DMR/YSF.

## Lado DMR o YSF

**`echolink-dmr`:** completa `[dmr]` como peer Homebrew (`callsign`, `dmrid`, `host`, `port`, `tg`, `password`; `options` RPTO opcional). TX siempre en TS2.

**`echolink-ysf`:** completa `[ysf]` (`host`, `port`, `dgid`). `[dmr] host` / `password` no se usan; deja `callsign` / `dmrid` para la identidad YSF.

Los alias de suscriptores (`[aliases]`) mapean indicativo ↔ ID DMR igual que en YSF↔DMR — ver [ysf-dmr-bridge.es.md](ysf-dmr-bridge.es.md) y el README.

## Logs (salida del programa en la terminal)

Los textos como `EL->DMR`, `RTP RX` o `vocoder ENC` **no van en el INI**:
aparecen en el **log** que imprime `ysf2dmrcon` al ejecutarlo.

1. Abre una terminal en el servidor.
2. Arranca el puente en primer plano (así ves el log al instante):

```bash
./ysf2dmrcon -c ysf2dmrcon-echolink-dmr.ini
```

3. Deja esa ventana abierta. Cada evento escribe una línea ahí.
4. Si usas systemd: `journalctl -u nombre-del-servicio -f`  
   Si usas nohup/redirección: mira el archivo donde enviaste la salida (`>> bridge.log 2>&1`).

### Activar más detalle (en el INI, luego reiniciar)

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

Así se ve una línea real en la terminal:

```text
2026-07-17 14:33:59,754 INFO/echolink: echolink: linked to *REDCHILE* (RTCP SDES)
2026-07-17 14:34:10,120 INFO/dmr: EL->DMR call start (TG 730170, src CE5RPY    id 7300391)
```

- Fecha/hora + nivel (`INFO`/`DEBUG`/…) + canal (`echolink`, `dmr`, `ysf`, `vocoder`)
- Después de los dos puntos: el mensaje a buscar

### Qué debe salir, en orden (EchoLink → DMR o YSF)

| Orden | Texto a buscar **en el log de la terminal** | Significado |
|------|-----------------------------------------------|-------------|
| 1 | `echolink: directory login OK` | Login al directorio bien |
| 2 | `echolink: connecting to …` | Ya tiene la IP del `host` |
| 3 | `echolink: linked to … (RTCP SDES)` | Enlazado (solo control; **aún no es audio**) |
| 4 | `echolink: RTP RX` (DEBUG) o `echolink: EL audio rms=` (INFO) | Llega audio EchoLink (RTP) |
| 5 | `EL->DMR call start` o `EL->YSF call start` | Empieza la llamada hacia DMR/YSF |
| 6 | `vocoder ENC` (DEBUG) o al arranque `vocoder ready at` | El vocoder por hardware está convirtiendo |
| 7 | `EL->DMR call end` o `EL->YSF call end` | Fin de la llamada |

Camino inverso: en el mismo log busca `DMR->EL call start` o `YSF->EL call start`, y `vocoder DEC`.

**Importante:** `linked to …` sin `RTP RX` / `EL audio rms` / `call start` = enlazado pero en silencio (nadie transmite en ese nodo/conferencia, o tu app EchoLink está en otra sala).

### Si no hay audio

| Lo que **no** aparece en el log | Qué revisar |
|---------------------------------|-------------|
| `directory login OK` | `callsign`/`password`, red TCP 5200, `directory_servers` |
| IP / `connecting to` / `station list: … not found` | `host` mal escrito o no está en el directorio |
| `linked to …` | Firewall UDP 5198/5199, NAT, peer sin respuesta |
| `RTP RX` / `EL audio rms` (estando `linked`) | Nadie habla en esa conferencia/nodo |
| Tras `call start`: `vocoder ENC timeout` o `PRODID probe failed` | `[vocoder] host`/`port` y el hardware AMBE |
| `call start` pero silencio en master DMR / reflector YSF | `[dmr]` TG/password o `[ysf]` host/DGID |

Filtrar solo lo importante mientras corre:

```bash
./ysf2dmrcon -c tu.ini 2>&1 | grep -E 'linked|RTP|EL audio|EL->|YSF->EL|DMR->EL|vocoder|call start|call end'
```

## Notas

- Una sola dirección de llamada a la vez (half-duplex).
- Este proyecto entrega **solo código fuente**; el encode/decode AMBE lo hace el vocoder por hardware.
