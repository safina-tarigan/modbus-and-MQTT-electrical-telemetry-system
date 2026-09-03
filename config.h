#ifndef CONFIG_H
#define CONFIG_H

// ==========================================
// NETWORK CONFIGURATION
// ==========================================
// Copy this file to "config.h" and fill in your own credentials.
// config.h is listed in .gitignore and will never be committed.

#define WIFI_SSID           "your_wifi_ssid"
#define WIFI_PASS           "your_wifi_password"

// ==========================================
// MQTT BROKER CONFIGURATION
// ==========================================
#define MQTT_BROKER_URI     "mqtts://your-broker-host:8883"
#define MQTT_USERNAME       "your_mqtt_username"
#define MQTT_PASSWORD       "your_mqtt_password"

#define TOPIC_PM_RAW        "telemetry/pm_raw"
#define TOPIC_CONFIG        "telemetry/config"
#define TOPIC_STATS         "telemetry/stats"

// ==========================================
// MODBUS POWER METER CONFIGURATION
// ==========================================
#define PM_IP_ADDR          "192.168.10.2"
#define PM_PORT             502
#define MODBUS_UNIT_ID      1

#endif // CONFIG_H
