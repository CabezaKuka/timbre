# Timbre WiFi — Wemos D1 mini + app Android

## Firmware (`firmware/timbre_wifi/timbre_wifi.ino`)
- Placa: **LOLIN(WEMOS) D1 R2 & mini**, core ESP8266 3.x. No usa librerías externas.
- La configuración está al inicio del archivo: pin del relé, si es activo en LOW, duración y cantidad de pulsos, cooldown, ventana del AP y clave del AP.

**Conexión**
| D1 mini | Módulo relé |
|---|---|
| 5V | VCC |
| G | GND |
| D1 (GPIO5) | IN |

La bocina va por los contactos COM/NO del relé, con su propia alimentación.

> ⚠️ Con módulos de 5 V activos en LOW, los 3,3 V del ESP en HIGH a veces no alcanzan para apagar el optoacoplador, y el relé queda pegado. Si pasa, hay tres salidas: alimentar el VCC del módulo con 3,3 V, usar un módulo de 3,3 V o activo en HIGH (con `RELAY_ACTIVE_LOW false`), o poner un transistor en el medio.

**Uso**
1. Al encender, el equipo crea la red `Timbre-XXXX` (abierta) y el LED parpadea rápido.
2. Conéctese a esa red: el teléfono abre el portal solo. Si no lo hace, entre a http://192.168.4.1.
3. Elija la red, escriba la clave y el nombre del timbre, y toque **Guardar y conectar**.
4. Si nadie se conecta al AP durante 60 s, el equipo lo cierra y se conecta a la red guardada. Mientras se conecta, el LED parpadea lento; una vez conectado, se apaga y solo se prende cuando suena.
5. Para cambiar de red: apague y encienda el equipo, y vuelva a entrar al AP.

**Pruebas sin la app**
- En el monitor serie (115200) se ve la IP.
- Desde un navegador: `http://<ip>/` muestra un botón; `http://<ip>/ring` toca directo.

## App Android (raíz del repo)
- Ábrala en Android Studio con *Open*, sobre la carpeta raíz del repo. El proyecto no trae `gradle-wrapper.jar`: Android Studio descarga Gradle 8.9 solo. Requiere Android 7 o superior.
- **Funcionamiento:** al tocar, la app envía `RING` por UDP a la última IP conocida. Si no responde, busca por broadcast (`PING`) en la red WiFi y lo encuentra aunque haya cambiado la IP.
- **Varios timbres en la misma red:** la app los lista para elegir uno y lo recuerda por nombre.
- **Datos móviles:** el socket se fuerza por la WiFi, aunque el teléfono tenga datos móviles activos.

**Protocolo** (UDP, puerto 4210):
| Envío | Respuesta |
|---|---|
| `PING` | `TIMBRE\|nombre` |
| `RING` | `OK\|nombre` (sonó) o `BUSY\|nombre` (ya estaba sonando / en cooldown) |

## Limitaciones
- Las redes de invitados con aislamiento de clientes bloquean la comunicación entre el teléfono y el equipo.
- Cualquier teléfono con la app conectado a esa red puede tocar el timbre: es el comportamiento pedido.
- Durante los primeros 60 s, cualquiera cerca del equipo puede reconfigurarlo. Para evitarlo, ponga una clave en `AP_PASSWORD`.
