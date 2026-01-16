# Synchronize-offline-data-from-the-SD-Card
An ESP32 IoT Gateway using SIM7600 to read Modbus RS485 sensors. It supports sending telemetry, syncing attributes, and handling RPC commands from ThingsBoard. Includes offline data logging to SD card to prevent data loss.

This project implements an IoT Gateway on ESP32 using the SIM7600 module for 4G connectivity. The device reads industrial sensor data via Modbus RS485 and communicates with the ThingsBoard platform.

It is designed to handle network instability by buffering data to an SD card when offline and syncing it back when online.

# Features

**Modbus Master:** Reads Temperature and Humidity from RS485 sensors.
**ThingsBoard Integration:**
    **Telemetry:** Uploads sensor data, RSSI signal strength, and fan status.
    **Attributes:** Syncs device information (Fan ID) with the server, asks for attributes response every 20 seconds.
    **RPC Controller:** Allows remote control of the device (e.g., setting Fan Speed) from the dashboard.
**Offline Data Logging:**
    * If the 4G signal is weak or lost, data is written to an SD card.
    * Uses a safe transaction method (renaming files) to ensure data is not lost during sync.
    * Auto-syncs historical data with correct timestamps (converted to UTC) when the network recovers.

## Configuration (you should check before doing anything else!)

Before flashing, update the `config` variables in the code:

1.  **ThingsBoard Server:** Update `tbServer`, `tbPort`, and `tbToken`.
2.  **APN:** Update the `apn` variable for your SIM card provider (e.g., "v-internet" for Viettel).
3.  **Timezone:** Since the time zone where I live is UTC+7, after getting the local epoch, I have to subtract 25200 seconds (7 hours) to synchronize with the world clock, which Thingsboard uses. You should check for your timezone and choose a suitable value to change (25200) in the row: unsigned long utcEpoch = localEpoch - 25200; 

## How it works

1.  The system initializes Modbus, SD Card, and SIM7600.
2.  It continuously checks signal quality. If the signal is good, it connects to MQTT.
3.  Every 5 seconds, it reads the sensor:
     **Online:** Sends JSON telemetry directly to ThingsBoard.
     **Offline:** Saves the data + timestamp to `offline.txt` on the SD card.
4.  If the device reconnects to the network, it checks for offline files and uploads them in batches while prioritizing new real-time data.
