#include <Arduino.h>
#include <pms_frame.h>

#ifdef BOARD_HAS_PSRAM
#error This first diagnostic must not initialize PSRAM before its mode is verified.
#endif

// Waveshare ESP32-S3-SIM7670G-4G V2.0 with the owner's four-wire PMS5003T.
// This diagnostic does not mount TF, touch the modem, or write flash/SD.
namespace {
constexpr int kSensorRx = 1; // sensor TX -> host RX
constexpr int kSensorTx = 2; // sensor RX <- host TX; V1 gauge conflict
constexpr std::uint32_t kBaud = 9600;
constexpr std::uint32_t kReportMs = 10000;
HardwareSerial sensor(1);
plantower::Parser parser{plantower::Model::Pms5003t};
plantower::Frame latest{};
std::uint32_t frames = 0;
std::uint32_t last_frame_ms = 0;
std::uint32_t last_report_ms = 0;
} // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println(
      "AQ DIAG board=waveshare-sim7670g-v2 sensor=PMS5003T mode=uart-only");
  Serial.printf(
      "AQ DIAG chip=%s flash_bytes=%lu psram_bytes=%lu rx=%d tx=%d\n",
      ESP.getChipModel(), static_cast<unsigned long>(ESP.getFlashChipSize()),
      static_cast<unsigned long>(ESP.getPsramSize()), kSensorRx, kSensorTx);
  sensor.begin(kBaud, SERIAL_8N1, kSensorRx, kSensorTx);
  last_report_ms = millis();
}

void loop() {
  while (sensor.available()) {
    plantower::Frame frame;
    if (parser.push(static_cast<std::uint8_t>(sensor.read()), frame)) {
      latest = frame;
      ++frames;
      last_frame_ms = millis();
    }
  }
  const std::uint32_t now = millis();
  if (static_cast<std::uint32_t>(now - last_report_ms) >= kReportMs) {
    last_report_ms = now;
    const bool fresh =
        frames && static_cast<std::uint32_t>(now - last_frame_ms) <= 5000;
    const bool warmed = now >= 30000;
    Serial.printf(
        "AQ PMS frames=%lu fresh=%u warmed=%u error=%u checksum_errors=%lu "
        "length_errors=%lu age_ms=%lu ",
        static_cast<unsigned long>(frames), fresh, warmed, latest.sensor_error,
        static_cast<unsigned long>(parser.checksum_failures()),
        static_cast<unsigned long>(parser.length_failures()),
        frames ? static_cast<unsigned long>(now - last_frame_ms) : 0UL);
    if (fresh && warmed && latest.sensor_error == 0) {
      Serial.printf("pm1=%u pm25=%u pm10=%u temp_c=%.1f rh_pct=%.1f\n",
                    latest.atmospheric_pm1, latest.atmospheric_pm25,
                    latest.atmospheric_pm10, latest.temperature_deci_c / 10.0f,
                    latest.humidity_deci_percent / 10.0f);
    } else {
      Serial.println("values=null");
    }
  }
  delay(10);
}
