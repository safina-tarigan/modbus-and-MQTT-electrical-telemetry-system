# ESP32 Real-Time Electrical Telemetry System (Parallel vs. Sequential FreeRTOS Architectures)

Undergraduate thesis project comparing sequential and parallel FreeRTOS task architectures on a dual-core ESP32 for real-time electrical telemetry, using a Modbus TCP to MQTT data pipeline.

## Overview

The system polls an ABB M1M20 power meter over Modbus TCP and publishes readings to an MQTT broker (HiveMQ) for real-time monitoring. As part of the thesis, two architectures were designed and benchmarked:

- **Sequential:** a single task handles both Modbus polling and MQTT publishing.
- **Parallel:** Modbus polling runs on Core 1, MQTT publishing on Core 0, communicating via FreeRTOS queues.

This repository contains the firmware for the **parallel architecture** only.

## Key Engineering Highlights

- Architected a parallel processing design isolating Modbus TCP polling (Core 1) from MQTT publishing (Core 0), communicating via FreeRTOS queues, resolving a blocking bottleneck present in the sequential architecture.
- Diagnosed and resolved a critical firmware bug involving message ID wraparound collisions using a monotonic-clock staleness check.
- Identified a socket-level bottleneck (1-second `SO_RCVTIMEO` timeout) creating a processing-time ceiling in the parallel architecture.
- Built a full end-to-end IIoT monitoring stack using HiveMQ, Telegraf, InfluxDB, and Grafana, bridged via a FastAPI control server on a self-managed Linux VPS.
- Automated data collection across 1–10 Hz sampling frequencies and performed statistical analysis on delay, RTT, jitter, and CPU utilization.

## Tech Stack

ESP32 (W5500 Ethernet) · FreeRTOS · C · Modbus TCP · MQTT · Python (FastAPI) · InfluxDB · Grafana · Telegraf · Linux VPS

## Setup

1. Copy `config.h.example` to `config.h`.
2. Fill in your own WiFi/network, MQTT broker, and power meter register details in `config.h`.
3. Build and flash with ESP-IDF.

`config.h` is excluded from version control via `.gitignore` — never commit real credentials.

## Repository Contents

- `main.c` — ESP32 firmware (parallel architecture)
- `config.h.example` — configuration template (copy to `config.h` and fill in your own values)
- `.gitignore` — excludes local secrets and build artifacts

## Note

This repository contains the firmware component of the thesis. The Python monitoring stack (FastAPI control bridge, sampling scheduler) is not yet included here.
