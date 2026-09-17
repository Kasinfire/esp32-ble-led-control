# Práctica 2 – Control de LED vía BLE con ESP32

## Descripción

Práctica 2 de IoT: control remoto de un LED en ESP32 vía **Bluetooth Low Energy (BLE)** (comandos `ON` / `OFF` / `BLINK:HZ` / `AUTO`), con lectura de potenciómetro (`READ_POT`) y consulta de estado (`STATUS`) mediante GATT. Reemplaza la comunicación por puerto serial usada en la Práctica 1 por un servicio BLE personalizado (Peripheral / GATT Server), reutilizando el circuito y la lógica de control desarrollados en esa actividad.

Probado con **nRF Connect for Mobile** como cliente GATT (Central).

## Hardware

- ESP32
- LED + resistencia de protección → GPIO 25
- Push-button → GPIO 26 (pull-down interna)
- Potenciómetro (terminal central) → GPIO 34 (ADC)

## Funcionamiento

- **Estado A**: el parpadeo del LED se controla dinámicamente con el potenciómetro.
- **Estado B**: el parpadeo se congela; el LED conserva su estado actual. El botón físico alterna entre A y B (toggle con antirrebote).
- El control remoto por BLE puede forzar encendido, apagado o una frecuencia fija, o devolver el control al modo automático.

## Servicio BLE

| | UUID |
|---|---|
| Service | `5a237299-a20c-4fb4-9feb-349936f07607` |
| Characteristic (Read / Write / Notify) | `b5ce6bd8-71ad-4799-9385-a5b29d56d419` |

Nombre del dispositivo anunciado: `KETEIMPORTA-ESP32`

### Comandos (Write)

| Comando | Efecto |
|---|---|
| `ON` | Habilita el sistema |
| `OFF` | Apaga el LED y deshabilita el sistema |
| `BLINK:HZ` | Fuerza el parpadeo a la frecuencia indicada (ej. `BLINK:2`) |
| `AUTO` | Regresa el control de frecuencia al potenciómetro local |
| `READ_POT` | Inicia el envío de lecturas del potenciómetro por 5 segundos |
| `STATUS` | Solicita el estado actual del sistema |

### Notificaciones (Notify)

- Lecturas periódicas del potenciómetro tras `READ_POT`, formato `POT: <valor>`.
- Estado del sistema tras `STATUS` o al detectar un cambio de modo automáticamente, formato:
  - `S,<modo>,<segundos_desde_cambio>,<lectura_adc>`
  - `S,BLINK,<hz>,<segundos_desde_cambio>,<lectura_adc>`

## Cómo probarlo

1. Cargar `practica2_ble_esp32.ino` al ESP32 desde el Arduino IDE.
2. Abrir nRF Connect, escanear y conectar a `IoT-Practica2-ESP32`.
3. Habilitar **Notify** en la característica.
4. Escribir comandos (como texto UTF-8) desde la app: `ON`, `OFF`, `BLINK:2`, `AUTO`, `READ_POT`, `STATUS`.
