#pragma once
// State shared between main.cpp, the CAN driver, the trip logger and the uploader.

#include <atomic>
#include <cstdint>

#include <cstddef>

extern std::atomic<bool> wifi_up;

// Diagnostic CAN (can.cpp)
void can_start();
bool can_ready();  // node up, not listen-only, not bus-off
bool can_set_listen_only(bool listen);
// ISO-TP request to module `tx`, reply from `rx`: bytes received, 0 = no
// reply in time, -1 = couldn't send.
int can_request(uint16_t tx, uint16_t rx, const uint8_t *req, int len, uint8_t *out, int max, int timeout_ms);
void can_status(char *out, size_t len);
void can_stop();  // before deep sleep
void can_probe(char *out, size_t len);  // desk wiring check of the CAN pins
void can_hold_dominant(int seconds);
void can_analyze(char *out, size_t len);  // desk: CANH on GPIO6, CANL on GPIO7    // desk: hold TX low so it can be measured

// ---- on-flash trip file format (little endian, packed) ----
// File: TripHeader followed by Record[] at ~2 Hz. Written as /logs/NNNNNN.tmp
// while recording, renamed to .bin when the trip ends (or on next boot).

#define TRIP_MAGIC 0x4C44424Fu  // "OBDL"
// Health scan file (/logs/hNNNNNN.hlt): a TripHeader with this magic and
// record_size 0, followed by a JSON body (raw hex replies from each module).
#define HEALTH_MAGIC 0x4844424Fu  // "OBDH"
#define TRIP_VERSION 2
// TripHeader.reserved[0] flags
#define TRIP_FLAG_FUEL_ML 0x01  // fuel_ml_per_l holds absolute mL (from MAF), not per litre

struct __attribute__((packed)) TripHeader {
    uint32_t magic;
    uint8_t version;
    uint8_t reserved[3];
    uint32_t trip_id;          // persistent counter from NVS
    uint32_t boot_id;          // persistent boot counter from NVS
    int64_t start_uptime_ms;   // esp_timer at trip start
    int64_t start_epoch;       // unix seconds, 0 if clock wasn't set yet
    uint16_t record_size;
    uint16_t reserved2;
};

// Raw OBD bytes are stored as the ECU sent them (A, or A*256+B); the server
// applies the SAE J1979 scaling.  dist/fuel are integrated at the fast poll rate.
struct __attribute__((packed)) Record {
    uint32_t t_ms;        // since trip start
    uint16_t rpm;         // already scaled (rpm)
    uint8_t speed_kph;    // 0D
    uint8_t coolant;      // 05  A-40 C
    uint8_t iat;          // 0F  A-40 C
    uint8_t map_kpa;      // 0B
    uint8_t baro_kpa;     // 33
    uint8_t throttle;     // 11  A*100/255 %
    uint8_t load;         // 04  A*100/255 %
    uint8_t pedal;        // 49  A*100/255 %
    uint8_t stft;         // 06  (A-128)*100/128 %
    uint8_t ltft;         // 07
    uint8_t fuel_status;  // 03 A
    uint8_t timing;       // 0E  A/2-64 deg
    uint8_t fuel_level;   // 2F  A*100/255 %
    uint8_t ambient;      // 46  A-40 C
    uint16_t abs_load;    // 43  raw*100/255 %
    uint16_t lambda;      // 44  raw*2/65536
    uint16_t voltage_mv;  // 42
    uint16_t cat_temp;    // 3C  raw/10-40 C
    uint16_t rail;        // 23  raw*10 kPa
    float dist_m;         // cumulative this trip
    float fuel_ml_per_l;  // cumulative estimated fuel, mL per litre of displacement (uncalibrated)
    uint32_t odo_km;      // VW UDS 22 295A on the engine ECU (odometer), 0 if unknown
};
static_assert(sizeof(TripHeader) == 36, "header layout");
static_assert(sizeof(Record) == 42, "record layout");

// Status LED (led.cpp)
extern std::atomic<bool> uploading;
extern std::atomic<int> pending_trips;  // finished trips not yet on the server
void led_start();

// Set by the uploader when the dashboard asks for a full health scan.
extern std::atomic<bool> scan_requested;
const char *health_note();  // what the last ignition-on health check decided

// Power (power.cpp)
extern std::atomic<bool> power_hold;  // don't sleep (OTA in progress)
void power_early_check();  // first thing in app_main: may go straight back to sleep
void power_start();
void power_status(char *out, size_t len);
float battery_volts();     // NAN without a divider on OBD_VBAT_GPIO
int adc1_mv(int channel);
bool ota_pending_verify();
void led_off();

void logger_start();
int64_t logger_last_answer_us();  // esp_timer time the engine ECU last replied (0 = never)
bool logger_idle();               // no trip being recorded
const char *vehicle_vin();        // "" until read from the car
float logger_ecu_volts();         // battery volts the ECU reports (PID 42), 0 if not fresh
void uploader_start();
// Latest values as JSON for the live view; returns false if no fresh data.
bool logger_live_json(char *out, size_t len);
const char *logger_state();
// The trip being recorded right now (flushed part only), so the uploader can
// stream it while WiFi is up. Both return false/-1 when no trip is open.
bool logger_trip_info(uint32_t *trip_id, int64_t *start_us, long *size);
long logger_read_trip(uint32_t trip_id, long offset, void *buf, size_t max);
