#pragma once

// SkyRC NC2500 NiMH charger over classic Bluetooth (SPP), for the original ESP32.
//
// How the connection works (details in docs/protocol.md, "Transport"):
//   - Outside pairing mode the charger does not listen for connections. When it is switched on it calls the
//     devices it is paired with, so this component is connectable (but hidden) and answers that call.
//   - It also calls the charger itself (address and channel known), which works while the charger is listening:
//     right after it has connected to something, or in pairing mode.
//   - The charger accepts one connection at a time, so the phone app and this component take turns.

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

// What the Bluetooth stack reports. Its callbacks run on the stack's own task, so they copy what they need into
// one of these and queue it; loop() handles the queue on the main task.
struct BtEvent {
  enum Type : uint8_t {
    SPP_INIT,          // serial (SPP) layer ready
    SPP_DISCOVERY,     // serial service lookup finished (scn = channel)
    SPP_OPEN,          // serial channel open (flag = the charger opened it)
    SPP_CLOSE,         // serial channel closed
    SPP_DATA,          // bytes received
    SPP_CONG,          // congestion changed (flag = congested)
    GAP_FOUND,         // a device seen while searching
    GAP_DISC_STOPPED,  // search finished
    ACL_UP,            // a radio link came up (incoming or outgoing)
  } type;
  int status{0};
  uint32_t handle{0};
  uint8_t scn{0};
  bool flag{false};
  uint8_t bda[6]{};
  std::string name;
  std::vector<uint8_t> data;
};

// One numbered message to the charger's Bluetooth module, resent until the module acknowledges it.
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

  // Configuration (from __init__.py)
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

  // false lets go of the charger (and stops answering it) so the phone app can have it; true takes it back.
  void set_enabled(bool enabled);
  bool is_enabled() const { return this->enabled_; }

  // Configure a slot and start it, like the vendor app's "Send" button. slot is 0-3.
  // mode: Charge, Refresh, Break-In, Discharge or Cycle. charge_a / discharge_a in amps, capacity_mah in mAh,
  // cycles as a count. Values are clamped to the charger's own limits and only the ones the mode uses are sent.
  bool apply_slot(uint8_t slot, const std::string &mode, float charge_a, float discharge_a, float capacity_mah,
                  float cycles);

  // End a slot's program. The vendor app has no stop command; a set-slot with mode 5 (Done) does it. slot is 0-3.
  bool stop_slot(uint8_t slot);

  // Bluetooth stack callbacks (run on the stack's task).
  static void gap_callback_(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
  static void spp_callback_(esp_spp_cb_event_t event, esp_spp_cb_param_t *param);

 protected:
  enum class State : uint8_t {
    STARTING,             // Bluetooth stack coming up
    IDLE,                 // not connected; answering the charger's calls, calling it at next_connect_at_
    INQUIRY,              // searching for the charger by name (no mac_address configured)
    SDP,                  // looking up its serial channel (no channel configured)
    CONNECTING,           // opening the serial channel
    HANDSHAKE_OPEN,       // opening our data channel with the charger's Bluetooth module
    HANDSHAKE_INFO,       // sending the vendor app's info requests
    HANDSHAKE_INFO_WAIT,  // waiting for them to be acknowledged
    HANDSHAKE_MODE,       // switching the module to data mode
    HANDSHAKE_CHALLENGE,  // challenge/response with the charger
    READY,                // connected: polling and accepting commands
    FAILED,
  };

  struct Slot {
    sensor::Sensor *voltage{nullptr}, *current{nullptr}, *capacity{nullptr}, *temperature{nullptr};
    sensor::Sensor *run_time{nullptr}, *resistance{nullptr}, *cycles{nullptr};
    text_sensor::TextSensor *mode{nullptr}, *state{nullptr}, *summary{nullptr};
    binary_sensor::BinarySensor *present{nullptr};
  };

  // Connection management
  void push_event_(BtEvent &&e);
  void handle_event_(BtEvent &e);
  void set_state_(State s);
  esp_spp_sec_t spp_security_() const;
  void begin_connect_();
  void disconnect_();
  void schedule_reconnect_();
  void note_unreachable_(const char *why);
  void on_link_lost_(const char *message);
  void on_channel_open_(uint32_t handle, bool opened_by_charger);
  void publish_connected_(bool c);
  void mark_offline_();

  // Protocol layers, bottom to top (docs/protocol.md, "Layers"):
  //   link     55 | len | payload | checksum
  //   module   00 | cmd | seq(2) | data      (numbered and acknowledged; our data rides on channel 1000)
  //   adapter  95 19 | len | message | checksum
  //   session  1B | cmd | data
  //   charger  0F | len | cmd | ... | checksum | FF FF
  void feed_link_(const uint8_t *data, size_t len);
  void handle_link_payload_(const uint8_t *p, size_t n);
  void send_link_(const uint8_t *payload, size_t n);
  static uint8_t link_checksum_(const uint8_t *p, size_t n);
  void queue_tx_(std::vector<uint8_t> &&frame);
  void pump_tx_();

  void enqueue_i_(uint8_t cmd, const std::vector<uint8_t> &data);
  void pump_i_();
  void handle_i_frame_(const uint8_t *p, size_t n);

  void feed_b_(const uint8_t *data, size_t len);
  void send_channel_(const std::vector<uint8_t> &message);
  static uint8_t b_checksum_(const uint8_t *d, size_t n);

  void handle_session_(const uint8_t *p, size_t n);
  void send_session_(uint8_t cmd, const std::vector<uint8_t> &data);
  void send_challenge_();
  static void compute_response_(const uint8_t *challenge, uint8_t *out);

  void send_rfa_(const std::vector<uint8_t> &data);
  void feed_app_(const uint8_t *data, size_t len);
  void handle_app_frame_(const uint8_t *f, size_t n);
  void parse_status_(const uint8_t *f, size_t n);
  void send_query_status_();
  void send_query_version_();
  bool check_slot_command_(uint8_t slot, const char *action);
  void send_set_slot_(uint8_t *packet);

  std::string hex_(const uint8_t *d, size_t n) const;

  // Configuration
  bool has_mac_{false};
  uint8_t target_bda_[6]{};  // the charger's address (configured, or found by searching)
  std::string name_match_{"skyrc"};
  uint8_t channel_cfg_{0};  // serial channel; 0 = look it up
  std::string pin_{"1234"};
  uint8_t security_{1};
  uint32_t poll_interval_ms_{15000};
  bool log_frames_{false};
  bool skip_auth_check_{false};

  binary_sensor::BinarySensor *connected_{nullptr};
  std::array<Slot, NUM_SLOTS> slots_{};
  bool slot_present_[NUM_SLOTS]{};

  // Connection state
  State state_{State::STARTING};
  uint32_t state_since_{0};
  bool enabled_{true};
  bool spp_ready_{false};
  bool target_known_{false};
  uint8_t scn_{1};
  uint32_t spp_handle_{0};
  bool congested_{false};
  uint32_t next_connect_at_{0};
  uint32_t backoff_ms_{0};
  uint8_t inquiries_{0};
  uint32_t last_role_set_{0};
  bool was_connected_{false};
  bool link_dead_{false};  // an established link stopped acknowledging

  // While connected
  uint32_t last_rx_ms_{0};
  uint32_t last_poll_{0};
  uint32_t last_keepalive_{0};
  uint8_t challenge_[16]{};
  uint8_t challenge_attempts_{0};
  bool auth_ok_{false};
  bool identity_logged_{false};

  // What has been published or logged, so each is said once
  bool published_connected_{false};
  bool offline_published_{false};  // entities already show "disconnected"
  bool outage_logged_{false};      // already said the charger is away (once per outage)
  bool warned_silent_{false};
  bool warned_auth_{false};

  // Events from the Bluetooth task
  std::mutex mutex_;
  std::deque<BtEvent> events_;

  // Link layer
  std::deque<std::vector<uint8_t>> tx_queue_;
  uint32_t last_tx_{0};
  uint8_t lstate_{0};
  uint16_t lexpect_{0};
  uint16_t lgot_{0};
  std::vector<uint8_t> lbuf_;

  // Module layer
  std::deque<IItem> iq_;
  uint16_t i_seq_{0};
  uint16_t chan_id_{1000};
  uint16_t last_rx_seq_{10000};
  bool channel_open_{false};

  // Adapter layer
  uint8_t bstate_{0};
  uint16_t bexpect_{0};
  uint16_t bgot_{0};
  std::vector<uint8_t> bbuf_;

  // Charger frames
  std::vector<uint8_t> abuf_;
};

}  // namespace skyrc_nc2500
}  // namespace esphome

#endif  // USE_ESP32
