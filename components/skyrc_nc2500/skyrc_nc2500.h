#pragma once

#ifdef USE_ESP32

#include "esphome/core/component.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include <esp_gap_bt_api.h>
#include <esp_spp_api.h>

#include <array>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace esphome {
namespace skyrc_nc2500 {

static const uint8_t NUM_SLOTS = 4;
static const uint8_t SLOT_RECORD_LEN = 17;

// Everything the Bluetooth stack tells us is queued from its task and handled in loop().
struct BtEvent {
  enum Type : uint8_t { SPP_INIT, SPP_DISCOVERY, SPP_OPEN, SPP_CLOSE, SPP_DATA, SPP_CONG, GAP_FOUND, GAP_DISC_STOPPED } type;
  int status{0};
  uint32_t handle{0};
  uint8_t scn{0};
  bool flag{false};
  uint8_t bda[6]{};
  std::string name;
  std::vector<uint8_t> data;
};

// One numbered message to the charger's module. It is resent every 500 ms until the module acknowledges it.
struct IItem {
  uint8_t cmd;
  std::vector<uint8_t> frame;
  uint8_t tries{0};
  uint32_t last_sent{0};
};

class SkyrcNC2500 : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override;
  float get_setup_priority() const override { return setup_priority::BLUETOOTH; }

  void set_mac(uint64_t mac);
  void set_name_match(const std::string &s) { this->name_match_ = s; }
  void set_channel(uint8_t c) { this->channel_cfg_ = c; }
  void set_pin(const std::string &s) { this->pin_ = s; }
  void set_security(uint8_t s) { this->security_ = s; }
  void set_poll_interval(uint32_t ms) { this->poll_interval_ms_ = ms; }
  void set_log_frames(bool b) { this->log_frames_ = b; }
  void set_skip_auth_check(bool b) { this->skip_auth_check_ = b; }

  void set_connected_sensor(binary_sensor::BinarySensor *s) { this->connected_ = s; }
  void set_voltage_sensor(uint8_t i, sensor::Sensor *s) { this->slots_[i].voltage = s; }
  void set_current_sensor(uint8_t i, sensor::Sensor *s) { this->slots_[i].current = s; }
  void set_capacity_sensor(uint8_t i, sensor::Sensor *s) { this->slots_[i].capacity = s; }
  void set_temperature_sensor(uint8_t i, sensor::Sensor *s) { this->slots_[i].temperature = s; }
  void set_run_time_sensor(uint8_t i, sensor::Sensor *s) { this->slots_[i].run_time = s; }
  void set_resistance_sensor(uint8_t i, sensor::Sensor *s) { this->slots_[i].resistance = s; }
  void set_cycles_sensor(uint8_t i, sensor::Sensor *s) { this->slots_[i].cycles = s; }
  void set_mode_sensor(uint8_t i, text_sensor::TextSensor *s) { this->slots_[i].mode = s; }
  void set_state_sensor(uint8_t i, text_sensor::TextSensor *s) { this->slots_[i].state = s; }
  void set_summary_sensor(uint8_t i, text_sensor::TextSensor *s) { this->slots_[i].summary = s; }
  void set_present_sensor(uint8_t i, binary_sensor::BinarySensor *s) { this->slots_[i].present = s; }

  // Lets the phone app have the charger: it only accepts one Bluetooth connection at a time.
  void set_enabled(bool enabled);
  bool is_enabled() const { return this->enabled_; }

  // Configure a slot and start it, exactly like the vendor app's "Send" button. slot is 0-3.
  // mode: Charge, Refresh, Break-In, Discharge or Cycle. Values outside the charger's own limits are clamped.
  //   charge_a / discharge_a in amps, capacity_mah in mAh, cycles as a count. Only the values the mode uses are sent.
  bool apply_slot(uint8_t slot, const std::string &mode, float charge_a, float discharge_a, float capacity_mah,
                  float cycles);

  // End a slot's program. The vendor app has no stop command; a set-slot with mode 5 (Done) and no values
  // does it (docs/protocol.md). slot is 0-3.
  bool stop_slot(uint8_t slot);

  // Called from the Bluetooth stack's task.
  static void gap_callback_(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
  static void spp_callback_(esp_spp_cb_event_t event, esp_spp_cb_param_t *param);

 protected:
  enum class State : uint8_t {
    STARTING,       // bringing the Bluetooth stack up
    IDLE,           // waiting to (re)connect
    INQUIRY,        // looking for the charger
    SDP,            // finding the serial channel
    CONNECTING,
    HANDSHAKE_OPEN,       // opening the data channel with the charger's Bluetooth module
    HANDSHAKE_INFO,       // sending the info requests
    HANDSHAKE_INFO_WAIT,
    HANDSHAKE_MODE,
    HANDSHAKE_CHALLENGE,
    READY,
    FAILED,
  };

  struct Slot {
    sensor::Sensor *voltage{nullptr}, *current{nullptr}, *capacity{nullptr}, *temperature{nullptr};
    sensor::Sensor *run_time{nullptr}, *resistance{nullptr}, *cycles{nullptr};
    text_sensor::TextSensor *mode{nullptr}, *state{nullptr}, *summary{nullptr};
    binary_sensor::BinarySensor *present{nullptr};
  };

  void push_event_(BtEvent &&e);
  void handle_event_(BtEvent &e);
  void set_state_(State s);
  void begin_connect_();
  void disconnect_();
  void schedule_reconnect_();
  void publish_connected_(bool c);
  void mark_offline_();
  void note_unreachable_(const char *why);

  // Layers, bottom to top:
  //   link     0x55 | len | payload | checksum
  //   module   00 | cmd | seq(2) | data      (numbered, acknowledged; data rides on channel 1000)
  //   adapter  95 19 | len | message | checksum
  //   session  1B | cmd | data
  void enqueue_i_(uint8_t cmd, const std::vector<uint8_t> &data);
  void pump_i_();
  void handle_i_frame_(const uint8_t *p, size_t n);
  void feed_b_(const uint8_t *data, size_t len);
  void send_channel_(const std::vector<uint8_t> &message);
  void handle_session_(const uint8_t *p, size_t n);
  static uint8_t b_checksum_(const uint8_t *d, size_t n);
  void feed_link_(const uint8_t *data, size_t len);
  void handle_link_payload_(const uint8_t *p, size_t n);
  void send_link_(const uint8_t *payload, size_t n);
  void send_session_(uint8_t cmd, const std::vector<uint8_t> &data);
  void send_rfa_(const std::vector<uint8_t> &data);
  void queue_tx_(std::vector<uint8_t> &&frame);
  void pump_tx_();
  static uint8_t link_checksum_(const uint8_t *p, size_t n);

  // Charger frames (0x0F | len | cmd | ... | chk | 0xFF 0xFF).
  void feed_app_(const uint8_t *data, size_t len);
  void handle_app_frame_(const uint8_t *f, size_t n);
  void parse_status_(const uint8_t *f, size_t n);
  void send_query_status_();
  void send_query_version_();
  void send_challenge_();
  static void compute_response_(const uint8_t *challenge, uint8_t *out);

  std::string hex_(const uint8_t *d, size_t n) const;

  // Configuration
  uint64_t mac_{0};
  bool has_mac_{false};
  std::string name_match_{"skyrc"};
  std::string pin_{"1234"};
  uint8_t security_{1};
  uint8_t channel_cfg_{0};  // serial channel; 0 = look it up
  uint32_t poll_interval_ms_{15000};
  bool log_frames_{false};
  bool skip_auth_check_{false};
  bool enabled_{true};

  binary_sensor::BinarySensor *connected_{nullptr};
  std::array<Slot, NUM_SLOTS> slots_{};

  // Runtime
  State state_{State::STARTING};
  uint32_t state_since_{0};
  uint32_t last_tx_{0};
  uint32_t last_poll_{0};
  uint32_t next_connect_at_{0};
  uint32_t backoff_ms_{5000};
  uint8_t info_index_{0};
  uint8_t challenge_attempts_{0};
  bool auth_ok_{false};
  bool published_connected_{false};
  bool offline_published_{false};  // entities already show "disconnected"
  bool outage_logged_{false};      // already told the user the charger is away (once per outage)
  bool was_connected_{false};
  bool warned_auth_{false};
  uint8_t inquiries_{0};
  uint32_t last_rx_ms_{0};
  bool bt_ready_{false};
  bool spp_ready_{false};
  uint8_t target_bda_[6]{};
  bool target_known_{false};
  uint8_t scn_{1};
  uint32_t spp_handle_{0};
  bool congested_{false};
  uint8_t challenge_[16]{};
  bool slot_present_[NUM_SLOTS]{};

  std::mutex mutex_;
  std::deque<BtEvent> events_;
  std::deque<std::vector<uint8_t>> tx_queue_;

  // Module layer
  std::deque<IItem> iq_;
  uint16_t i_seq_{0};
  uint16_t chan_id_{1000};
  uint16_t last_rx_seq_{10000};
  bool channel_open_{false};
  bool warned_silent_{false};
  // Adapter parser
  uint8_t bstate_{0};
  uint16_t bexpect_{0};
  uint16_t bgot_{0};
  std::vector<uint8_t> bbuf_;
  // Link parser
  uint8_t lstate_{0};
  uint16_t lexpect_{0};
  uint16_t lgot_{0};
  std::vector<uint8_t> lbuf_;
  // Charger-frame reassembly
  std::vector<uint8_t> abuf_;
};

}  // namespace skyrc_nc2500
}  // namespace esphome

#endif  // USE_ESP32
