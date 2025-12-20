## Project Description

This project is a **Portable Weather Station** built on the **ESP32 / ESP32-S3**, designed to operate in both **offline and online modes**. Local environmental readings are captured using a **BMP280 sensor** for temperature and pressure, ensuring continuous operation even without an internet connection.

When Wi-Fi is available, the device can **manually detect nearby networks and connect using a custom on-screen digital keyboard**, fully navigable with **four physical buttons (UP, DOWN, SELECT, BACK)**. Entered credentials are stored via **ESP32 Preferences**, allowing automatic reconnection after reboot. Online connectivity enables weather data retrieval from the **Open-Meteo API (Mindanao region)**, complementing local sensor data with extended forecasts and environmental context.

The system includes a **planned visual and audio weather indication mechanism**, which is **not yet fully implemented in the current release**. This feature is intended to provide an intuitive, at-a-glance interpretation of forecast data without relying solely on the display, making the device more useful in low-visibility or hands-free scenarios.

The mechanism is designed to translate weather prediction data—such as rain probability and general weather conditions—into simple hardware-based signals. These indicators are meant to act as **supplementary alerts**, not replacements for detailed on-screen information, and are planned to operate automatically once reliable forecast thresholds are met.

## Planned Features
 A visual and audio weather indication mechanism is planned for future implementation. This feature is intended to provide quick, non-visual feedback about upcoming weather conditions, allowing users to interpret forecasts at a glance without relying solely on the display.

The system is designed to convert forecast data—such as rain probability and general weather conditions—into simple hardware-based signals. These indicators are planned as supplementary alerts, enhancing situational awareness while preserving low power consumption.

- **Red LED + Buzzer:** Intended to activate when rainfall is predicted. Alert frequency and buzzer patterns are planned to scale with rain probability or severity, providing a graded warning rather than a single binary alert.
- **Green LED:** Intended to indicate normal, clear, or sunny weather conditions, offering a quick visual confirmation of stable weather.
- GPS-Based Location Detection (GY-NEO6MV2 / NEO-6M)
- This module will allow the system to determine its geographic location independently, without relying solely on network-based positioning.
- Accurate latitude and longitude data will be used to Automatically select the correct location-based forecast from the weather API. Improve forecast accuracy when compared to manually configured or region-based data. Maintain location awareness even when Wi-Fi is unavailable.

The planned GPS integration will combine local hardware-based positioning with API-based weather data, enabling more precise and autonomous operation, particularly in portable or outdoor deployments.
  
  
This feature is not yet implemented in the current codebase and is reserved for a future update once forecast evaluation logic and alert thresholds are finalized.
Once implemented, this system will be configurable and designed to integrate cleanly with both offline sensor data trends and online forecast inputs, further enhancing usability while maintaining low power consumption.


---

## What's Changed
- Added portable weather station UI optimized for button navigation  
- Integrated BMP280 sensor for local temperature and pressure readings  
- Implemented non-blocking Wi-Fi connection state machine  
- Added on-screen digital keyboard for Wi-Fi credential input  
- Enabled persistent Wi-Fi storage using ESP32 Preferences  
- Added LED and buzzer weather prediction based on API data  
- Fixed TFT redraw logic to prevent flickering and sensor-related glitches  

![weather_ui](https://github.com/user-attachments/assets/REPLACE_IMAGE_1)
![wiring](https://github.com/user-attachments/assets/REPLACE_IMAGE_2)

---

## Hardware Connections

| Component | ESP32 / ESP32-S3 Pin | Notes |
|----------|---------------------|-------|
| **TFT VCC** | 3.3V | Power supply |
| **TFT GND** | GND | Ground |
| **TFT SCL** | GPIO18 | SPI Clock |
| **TFT SDA** | GPIO23 | SPI MOSI |
| **TFT RES** | GPIO4 | Reset |
| **TFT DC** | GPIO2 | Data / Command |
| **TFT CS** | GPIO5 | Chip Select |
| **BMP280 SDA** | GPIO27 | I2C Data |
| **BMP280 SCL** | GPIO26 | I2C Clock |
| **BTN UP** | GPIO15 | Navigate up |
| **BTN DOWN** | GPIO32 | Navigate down (input-only safe pin) |
| **BTN SELECT** | GPIO21 | Select / confirm |
| **BTN BACK** | GPIO12 | Back / cancel |
| **Red LED** | GPIO16 | Blinks faster when rain is predicted |
| **Green LED** | GPIO17 | Blinks when weather is normal/sunny |
| **Buzzer** | GPIO19 | Emits sound when rain is predicted |

---

## New Features
- **Local Weather Monitoring**  
  Measures temperature and pressure via BMP280, allowing operation without internet access.  

- **Online Weather Integration**  
  Retrieves weather data from the Open-Meteo API for the Mindanao region to supplement local sensor readings.  

- **Button-Controlled Interface**  
  Navigable entirely through four physical buttons for menus, Wi-Fi setup, and on-screen keyboard input.  

- **On-Screen Digital Keyboard**  
  Enables manual Wi-Fi credential entry directly on the TFT display.  

- **Persistent Wi-Fi Credentials**  
  Credentials stored via ESP32 Preferences for automatic reconnection after reboot.  

- **LED and Buzzer Weather Prediction**  
  Uses online weather data to trigger visual and audio cues:  
  - **Red LED + Buzzer:** Rain predicted  
  - **Green LED:** Normal/sunny conditions  

- **Non-Blocking Wi-Fi Logic**  
  Keeps UI responsive by using a state machine for network connection handling.  

- **Stable TFT Rendering**  
  Optimized redraw logic prevents flickering and sensor-induced glitches.  

---

## Future Improvements
- Multi-day weather forecast display with icons and trends  
- Integration of additional sensors (humidity, air quality)  
- Wi-Fi signal strength indicator on UI  
- Local data logging with export or visualization support  
- Automatic network selection among known networks based on availability  
- Power optimization using deep-sleep modes for battery operation  
- UI customization: themes, units (°C / °F), and user preferences  

---

## Notes
- All components operate on **3.3V logic only**  
- Buttons are momentary, normally-open, wired between GPIO and GND with internal pull-ups enabled  
- GPIO32 is input-only and safe for button use  
- Ensure all modules share a common ground  
- Keep SPI and I2C wiring short to reduce noise and display instability  

---

## Project Author
**Joshua C. Godilos** – Creator and designer of the system.  

**Reason for Building:**  
To develop a **self-contained, reliable weather monitoring system** that works offline and online, supports manual Wi-Fi connection without external tools, and demonstrates advanced embedded system design using limited input interfaces.
