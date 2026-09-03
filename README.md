# ESP32 Real-Time Electrical Telemetry System
### Parallel FreeRTOS Architecture

Undergraduate thesis project implementing a parallel task architecture on a dual-core ESP32 for real-time electrical telemetry.

The system reads electrical measurements from an ABB M1M20 power meter using Modbus TCP and publishes the data to an MQTT broker for monitoring.

## Overview

Modbus polling and MQTT publishing are separated into independent FreeRTOS tasks. The Modbus task runs on Core 1, while the MQTT task runs on Core 0. The two tasks exchange measurement data through a FreeRTOS queue.

## What I Worked On

- Implemented a parallel FreeRTOS architecture on the dual-core ESP32.
- Separated Modbus TCP polling and MQTT publishing into independent tasks.
- Used a FreeRTOS queue to pass measurement data between tasks.
- Investigated a message ID wraparound issue that caused incorrect delay measurements and fixed it using a monotonic-clock check for stale entries.
- Traced a processing-time limitation to a 1-second `SO_RCVTIMEO` socket timeout.
- Published telemetry data over MQTT with QoS 1, using message acknowledgments to measure delay and processing time per sample.
- Connected the ESP32 telemetry system to HiveMQ, Telegraf, InfluxDB, and Grafana for data collection and monitoring.
- Ran tests at sampling frequencies from 1-10 Hz and analyzed delay, RTT, jitter, and CPU utilization.

## System

```text
ABB M1M20
    |
    | Modbus TCP
    v
ESP32 + W5500
    |
    +-- Core 1: Modbus Polling
    |
    +-- Core 0: MQTT Publishing (QoS 1)
             |
             | MQTT
             v
          HiveMQ
             |
             v
          Telegraf
             |
             v
          InfluxDB
             |
             v
          Grafana
```

A separate FastAPI server was used for system control and configuration.

## Tech Stack

**Embedded:** ESP32, W5500 Ethernet, C, ESP-IDF, FreeRTOS

**Protocols:** Modbus TCP, MQTT (QoS 1)

**Monitoring & Data:** HiveMQ, Telegraf, InfluxDB, Grafana

**Backend:** Python, FastAPI, Linux VPS

## Testing

The architecture was tested at sampling frequencies between 1 and 10 Hz. The measurements collected during testing included:

- Total delay
- MQTT round-trip time (RTT)
- Jitter
- CPU utilization
- Number of transmitted samples

## Repository Contents

```text
.
├── main.c
├── config.h
└── .gitignore
```

- `main.c` - ESP32 firmware for the parallel architecture
- `config.h` - example configuration file
- `.gitignore` - excludes local configuration and build files

## Setup

1. Copy `config.h` to `config.h`.
2. Add your own network, MQTT broker, and power meter configuration.
3. Build and flash the firmware using ESP-IDF.

`config.h` is excluded from version control. Do not commit credentials or other private configuration.

## Notes

This repository currently contains the ESP32 firmware for the parallel architecture. The Python monitoring and control components, including the FastAPI server and sampling scheduler, are not included yet.
