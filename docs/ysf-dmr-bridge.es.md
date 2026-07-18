# Puente YSF ↔ DMR (ysf2dmrcon)

Cómo poner en marcha el modo por defecto **`ysf-dmr`** (reflector YSF ↔ peer Homebrew DMR).

Compilación / instalación: [README.es.md](../README.es.md) · [README.md](../README.md).
**English:** [ysf-dmr-bridge.md](ysf-dmr-bridge.md).
Modos EchoLink: [echolink-bridge.es.md](echolink-bridge.es.md).

## Requisitos

1. Reflector YSF (host, puerto, DGID).
2. Master DMR con contraseña Homebrew y talkgroup.
3. `callsign` + `dmrid` del puente únicos por instancia.
4. Este modo no necesita vocoder AMBE externo.

## Arranque rápido

```bash
cp ysf2dmrcon.example.ini ysf2dmrcon.ini
# o: cp ysf2dmrcon-ysf-dmr.example.ini ysf2dmrcon-ysf-dmr.ini
# editar reflector YSF/DGID, host/password/tg DMR, callsign/dmrid
./ysf2dmrcon
# o: ./ysf2dmrcon -c /ruta/a/tu.ini
```

## Modo

```ini
[bridge]
mode = ysf-dmr
```

## `[ysf]`

| Clave | Obligatoria | Descripción |
|-------|-------------|-------------|
| `host` | sí | Hostname o IP del reflector |
| `port` | sí | Habitualmente `42000` |
| `dgid` | sí | Sala a unir (0–99) |

## `[dmr]`

| Clave | Obligatoria | Descripción |
|-------|-------------|-------------|
| `callsign` | sí | Indicativo del puente (monitor) |
| `dmrid` | sí | ID DMR del puente — único por proceso |
| `host` / `port` | sí | Master DMR |
| `tg` | sí | Talkgroup de voz (TX siempre **TS2**) |
| `clear_dynamic_tg` | no | `1` = al login DMR, PTT silencio a **TG 4000** primero (quita TGs dinámicos del peer), luego PTT de conexión a `tg` (por defecto off) |
| `password` | sí | Contraseña del peer Homebrew |
| `options` | no | Cadena RPTO (p. ej. `TS2=1234;SINGLE=0;TIMER=60;`); omitir/vacío = sin RPTO |
| `location` / `description` | no | Texto en el monitor |

Frecuencias RX/TX en cero en RPTC → el monitor muestra N/A (normal en un puente software).

## Alias de suscriptores (`[aliases]`)

Opcional pero recomendable para que los locutores muestren el indicativo / ID correctos.

- Archivos en `data_dir` (por defecto `./data`), misma idea que `ALIASES` de adn-server.
- Ver el README para `subscriber_url`, `stale_minutes`, etc.
- Varios procesos pueden compartir el mismo `data_dir`.

**Identidad (lo que ve el usuario):**

- DMR → YSF: busca el ID de radio en `subscriber_ids.json`; si falta, envía el número.
- YSF → DMR: quita el sufijo tras `-` o `/` (`HP3ICC-FT3` → `HP3ICC`) y busca; si no está, usa el `callsign` + `dmrid` del puente.

## Varios puentes

Un proceso por puente (DGID, TG y `dmrid` distintos), cada uno con su INI:

```bash
./ysf2dmrcon -c ysf2dmrcon-tg71442.ini
./ysf2dmrcon -c ysf2dmrcon-tg71481.ini
```

## Logging

`[log] level=INFO` en uso normal; `DEBUG` en `[dmr]` / `[ysf]` para depurar.
Líneas útiles: `YSF->DMR` / `DMR->YSF` call start/end.

## Notas

- Este proyecto entrega **solo código fuente**.
