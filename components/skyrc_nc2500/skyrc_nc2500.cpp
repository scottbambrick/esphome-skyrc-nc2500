#ifdef USE_ESP32

#include "skyrc_nc2500.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <esp_bt.h>
#include <esp_bt_device.h>
#include <esp_bt_main.h>
#include <esp_random.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace esphome {
namespace skyrc_nc2500 {

static const char *const TAG = "skyrc_nc2500";

// The charger's "iBT-02" bridge module speaks a framed protocol over the serial (SPP) link.
// Everything below was read out of the vendor's Android app (com.revogi.smartcharge 0.9.9).
static const uint8_t LINK_SOF = 0x55;
static const uint8_t SESSION_ID = 0x1B;
static const uint8_t CMD_INFO_REQUEST = 0x01;
static const uint8_t CMD_MODE = 0x11;
static const uint8_t CMD_RFA_WRITE = 0x12;
static const uint8_t CMD_CHALLENGE = 0x60;
static const uint8_t CMD_RFA_DATA = 0x91;
static const uint8_t CMD_CHALLENGE_REPLY = 0xE0;
static const uint8_t RFA_TYPE = 0x82;
static const uint16_t INFO_IDS[] = {0x0100, 0x0100, 0x0101, 0x0102, 0x0103, 0x0106, 0x0107, 0x2000, 0x2100};
static const uint8_t AUTH_KEY[16] = {0xBB, 0x11, 0x77, 0x33, 0xBB, 0x11, 0x77, 0x33,
                                     0xCC, 0x63, 0x11, 0x56, 0xDC, 0xF0, 0x0F, 0x26};

static const char *const MODE_NAMES[] = {"Charge", "Refresh", "Break-In", "Discharge", "Cycle", "Done", "Wait", "Error"};
static const char *const STATE_NAMES[] = {"Not working", "Charging", "Resting", "Discharging", "Resting", "Done", "Done"};

static SkyrcNC2500 *instance_ = nullptr;

static std::string bda_to_string(const uint8_t *b) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
  return buf;
}

// ---------------------------------------------------------------------------------------------
// Setup / config
// ---------------------------------------------------------------------------------------------

void SkyrcNC2500::set_mac(uint64_t mac) {
  this->mac_ = mac;
  this->has_mac_ = true;
  for (int i = 0; i < 6; i++)
    this->target_bda_[i] = (mac >> (8 * (5 - i))) & 0xFF;
}

void SkyrcNC2500::setup() {
  instance_ = this;
  this->lbuf_.reserve(300);

  esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  esp_err_t err = esp_bt_controller_init(&cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Bluetooth controller init failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Bluetooth controller enable failed: %s (is this an original ESP32?)", esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  err = esp_bluedroid_init();
  if (err == ESP_OK)
    err = esp_bluedroid_enable();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Bluedroid init failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  esp_bt_gap_register_callback(SkyrcNC2500::gap_callback_);
  esp_spp_register_callback(SkyrcNC2500::spp_callback_);

  esp_spp_cfg_t spp_cfg = {};
  spp_cfg.mode = ESP_SPP_MODE_CB;
  spp_cfg.enable_l2cap_ertm = true;
  spp_cfg.tx_buffer_size = 0;
  err = esp_spp_enhanced_init(&spp_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "SPP init failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  // We are only ever the initiator: not connectable, not discoverable.
  esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

  // "Just works" for secure simple pairing; a fixed PIN for older (legacy) pairing.
  esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
  esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(uint8_t));
  esp_bt_pin_code_t pin_code = {};
  size_t pin_len = std::min<size_t>(this->pin_.size(), 16);
  memcpy(pin_code, this->pin_.data(), pin_len);
  esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED, pin_len, pin_code);
}

void SkyrcNC2500::dump_config() {
  ESP_LOGCONFIG(TAG, "SkyRC NC2500 (classic Bluetooth)");
  if (this->has_mac_)
    ESP_LOGCONFIG(TAG, "  Charger address: %s", bda_to_string(this->target_bda_).c_str());
  else
    ESP_LOGCONFIG(TAG, "  Charger found by name containing: '%s'", this->name_match_.c_str());
  ESP_LOGCONFIG(TAG, "  Pairing: %s, PIN set (%u digits)", this->security_ ? "authenticate" : "none",
                (unsigned) this->pin_.size());
  ESP_LOGCONFIG(TAG, "  Poll interval: %ums", (unsigned) this->poll_interval_ms_);
  ESP_LOGCONFIG(TAG, "  Log frames: %s", YESNO(this->log_frames_));
  LOG_BINARY_SENSOR("  ", "Connected", this->connected_);
}

// Before a reboot or update, let go of the charger properly. Otherwise it keeps the dead link for a couple of
// minutes and refuses new connections.
void SkyrcNC2500::on_shutdown() {
  if (this->spp_handle_ != 0) {
    esp_spp_disconnect(this->spp_handle_);
    delay(600);
  }
}

void SkyrcNC2500::set_enabled(bool enabled) {
  if (this->enabled_ == enabled)
    return;
  this->enabled_ = enabled;
  ESP_LOGI(TAG, "Bluetooth link %s", enabled ? "enabled" : "released (charger free for the phone app)");
  this->outage_logged_ = false;
  if (!enabled) {
    this->was_connected_ = false;
    this->disconnect_();
    this->mark_offline_();
    this->set_state_(State::IDLE);
  } else {
    this->next_connect_at_ = millis();
    this->backoff_ms_ = 5000;
  }
}

// ---------------------------------------------------------------------------------------------
// Bluetooth stack callbacks (run on the Bluetooth task: copy the data and queue it)
// ---------------------------------------------------------------------------------------------

void SkyrcNC2500::push_event_(BtEvent &&e) {
  std::lock_guard<std::mutex> lock(this->mutex_);
  if (this->events_.size() < 64)
    this->events_.push_back(std::move(e));
}

void SkyrcNC2500::gap_callback_(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  if (instance_ == nullptr)
    return;
  switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
      BtEvent e;
      e.type = BtEvent::GAP_FOUND;
      memcpy(e.bda, param->disc_res.bda, 6);
      for (int i = 0; i < param->disc_res.num_prop; i++) {
        esp_bt_gap_dev_prop_t *p = &param->disc_res.prop[i];
        if (p->type == ESP_BT_GAP_DEV_PROP_BDNAME) {
          e.name.assign((const char *) p->val, p->len);
        } else if (p->type == ESP_BT_GAP_DEV_PROP_EIR && e.name.empty()) {
          uint8_t len = 0;
          uint8_t *n = esp_bt_gap_resolve_eir_data((uint8_t *) p->val, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &len);
          if (n == nullptr)
            n = esp_bt_gap_resolve_eir_data((uint8_t *) p->val, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &len);
          if (n != nullptr)
            e.name.assign((const char *) n, len);
        }
      }
      instance_->push_event_(std::move(e));
      break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT: {
      if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
        BtEvent e;
        e.type = BtEvent::GAP_DISC_STOPPED;
        instance_->push_event_(std::move(e));
      }
      break;
    }
    case ESP_BT_GAP_PIN_REQ_EVT: {
      esp_bt_pin_code_t pin_code = {};
      size_t n = std::min<size_t>(instance_->pin_.size(), 16);
      memcpy(pin_code, instance_->pin_.data(), n);
      esp_bt_gap_pin_reply(param->pin_req.bda, true, n, pin_code);
      ESP_LOGD(TAG, "Pairing: PIN requested, replied with the configured PIN");
      break;
    }
    case ESP_BT_GAP_CFM_REQ_EVT:
      esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
      break;
    case ESP_BT_GAP_AUTH_CMPL_EVT:
      if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS)
        ESP_LOGD(TAG, "Pairing/authentication OK with %s", bda_to_string(param->auth_cmpl.bda).c_str());
      else
        ESP_LOGD(TAG, "Pairing/authentication failed (status %d)", (int) param->auth_cmpl.stat);
      break;
    default:
      break;
  }
}

void SkyrcNC2500::spp_callback_(esp_spp_cb_event_t event, esp_spp_cb_param_t *param) {
  if (instance_ == nullptr)
    return;
  BtEvent e;
  switch (event) {
    case ESP_SPP_INIT_EVT:
      e.type = BtEvent::SPP_INIT;
      e.status = param->init.status;
      break;
    case ESP_SPP_DISCOVERY_COMP_EVT:
      e.type = BtEvent::SPP_DISCOVERY;
      e.status = param->disc_comp.status;
      e.scn = param->disc_comp.scn_num > 0 ? param->disc_comp.scn[0] : 0;
      break;
    case ESP_SPP_OPEN_EVT:
      e.type = BtEvent::SPP_OPEN;
      e.status = param->open.status;
      e.handle = param->open.handle;
      break;
    case ESP_SPP_CLOSE_EVT:
      e.type = BtEvent::SPP_CLOSE;
      e.status = param->close.status;
      e.handle = param->close.handle;
      break;
    case ESP_SPP_DATA_IND_EVT:
      e.type = BtEvent::SPP_DATA;
      e.data.assign(param->data_ind.data, param->data_ind.data + param->data_ind.len);
      break;
    case ESP_SPP_CONG_EVT:
      e.type = BtEvent::SPP_CONG;
      e.flag = param->cong.cong;
      break;
    case ESP_SPP_WRITE_EVT:
      e.type = BtEvent::SPP_CONG;
      e.flag = param->write.cong;
      break;
    default:
      return;
  }
  instance_->push_event_(std::move(e));
}

// ---------------------------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------------------------

void SkyrcNC2500::set_state_(State s) {
  this->state_ = s;
  this->state_since_ = millis();
}

void SkyrcNC2500::publish_connected_(bool c) {
  if (this->connected_ != nullptr && (!this->published_connected_ || this->connected_->state != c))
    this->connected_->publish_state(c);
  this->published_connected_ = true;
}

void SkyrcNC2500::schedule_reconnect_() {
  // A charger that is switched off is normal. Search gently: 5 s, 10 s, 20 s ... never slower than every 2 minutes (30 s once the address is known)
  // (each search briefly competes with Wi-Fi for the radio, so we don't hammer it).
  this->next_connect_at_ = millis() + this->backoff_ms_;
  // With a known address a connect attempt is cheap, so keep trying every 30 s at most; searching is dearer.
  this->backoff_ms_ = std::min<uint32_t>(this->backoff_ms_ * 2, this->has_mac_ ? 30000 : 120000);
  this->set_state_(State::IDLE);
}

void SkyrcNC2500::mark_offline_() {
  if (this->offline_published_)
    return;
  this->offline_published_ = true;
  this->publish_connected_(false);
  for (auto &s : this->slots_) {
    if (s.voltage != nullptr) s.voltage->publish_state(NAN);
    if (s.current != nullptr) s.current->publish_state(NAN);
    if (s.capacity != nullptr) s.capacity->publish_state(NAN);
    if (s.temperature != nullptr) s.temperature->publish_state(NAN);
    if (s.run_time != nullptr) s.run_time->publish_state(NAN);
    if (s.resistance != nullptr) s.resistance->publish_state(NAN);
    if (s.cycles != nullptr) s.cycles->publish_state(NAN);
    if (s.mode != nullptr) s.mode->publish_state("Disconnected");
    if (s.state != nullptr) s.state->publish_state("Disconnected");
    if (s.summary != nullptr) s.summary->publish_state("Not connected");
    if (s.present != nullptr) s.present->publish_state(false);
  }
}

// The charger is off, out of range, or already connected to the phone: say so once, then retry quietly.
void SkyrcNC2500::note_unreachable_(const char *why) {
  if (!this->outage_logged_) {
    ESP_LOGI(TAG, "Charger not reachable (%s) - probably switched off or busy with the phone. Retrying quietly.", why);
    this->outage_logged_ = true;
  } else {
    ESP_LOGD(TAG, "Charger still not reachable (%s)", why);
  }
  this->mark_offline_();
  this->schedule_reconnect_();
}

void SkyrcNC2500::begin_connect_() {
  if (!this->spp_ready_)
    return;
  this->target_known_ = this->has_mac_;
  if (this->has_mac_ && this->channel_cfg_ > 0) {
    // Address and channel known: connect straight away, like a phone reconnecting to a paired device.
    // No search is needed, so this works without the charger being in pairing mode.
    ESP_LOGD(TAG, "Connecting to %s on channel %u", bda_to_string(this->target_bda_).c_str(), this->channel_cfg_);
    this->scn_ = this->channel_cfg_;
    this->set_state_(State::CONNECTING);
    esp_spp_connect(this->security_ ? ESP_SPP_SEC_AUTHENTICATE : ESP_SPP_SEC_NONE, ESP_SPP_ROLE_MASTER, this->scn_,
                    this->target_bda_);
  } else if (this->has_mac_) {
    ESP_LOGD(TAG, "Looking up the serial service on %s", bda_to_string(this->target_bda_).c_str());
    this->set_state_(State::SDP);
    esp_spp_start_discovery(this->target_bda_);
  } else {
    // The first couple of searches list everything they see (handy for finding the address); later ones stay quiet.
    this->inquiries_ = std::min<uint8_t>(this->inquiries_ + 1, 100);
    ESP_LOGD(TAG, "Searching for a Bluetooth device named like '%s'", this->name_match_.c_str());
    this->set_state_(State::INQUIRY);
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
  }
}

void SkyrcNC2500::disconnect_() {
  if (this->spp_handle_ != 0) {
    esp_spp_disconnect(this->spp_handle_);
  }
  if (this->state_ == State::INQUIRY)
    esp_bt_gap_cancel_discovery();
}

void SkyrcNC2500::handle_event_(BtEvent &e) {
  switch (e.type) {
    case BtEvent::SPP_INIT:
      if (e.status == ESP_SPP_SUCCESS) {
        this->spp_ready_ = true;
        ESP_LOGD(TAG, "Bluetooth serial ready");
        this->mark_offline_();
        this->set_state_(State::IDLE);
      } else {
        ESP_LOGE(TAG, "SPP init failed (%d)", e.status);
      }
      break;

    case BtEvent::GAP_FOUND: {
      std::string lower = e.name;
      std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
      if (this->log_frames_ || this->inquiries_ <= 2) {
        ESP_LOGI(TAG, "  Found %s  '%s'", bda_to_string(e.bda).c_str(), e.name.c_str());
      } else {
        ESP_LOGV(TAG, "  Found %s  '%s'", bda_to_string(e.bda).c_str(), e.name.c_str());
      }
      if (this->state_ == State::INQUIRY && !this->target_known_ && !lower.empty() &&
          lower.find(this->name_match_) != std::string::npos) {
        memcpy(this->target_bda_, e.bda, 6);
        this->target_known_ = true;
        ESP_LOGI(TAG, "Charger found: %s (%s)", bda_to_string(e.bda).c_str(), e.name.c_str());
        esp_bt_gap_cancel_discovery();
      }
      break;
    }

    case BtEvent::GAP_DISC_STOPPED:
      if (this->state_ == State::INQUIRY) {
        if (this->target_known_) {
          this->set_state_(State::SDP);
          esp_spp_start_discovery(this->target_bda_);
        } else {
          if (this->inquiries_ <= 2)
            ESP_LOGI(TAG, "No charger found yet. If it is listed above under another name, set mac_address: in the config.");
          this->note_unreachable_("not found nearby");
        }
      }
      break;

    case BtEvent::SPP_DISCOVERY:
      if (this->state_ != State::SDP)
        break;
      if (e.status == ESP_SPP_SUCCESS && e.scn > 0) {
        this->scn_ = e.scn;
        ESP_LOGI(TAG, "Serial service on channel %u", this->scn_);
      } else {
        this->scn_ = 1;
        ESP_LOGD(TAG, "Serial service lookup failed (%d), trying channel 1", e.status);
      }
      this->set_state_(State::CONNECTING);
      esp_spp_connect(this->security_ ? ESP_SPP_SEC_AUTHENTICATE : ESP_SPP_SEC_NONE, ESP_SPP_ROLE_MASTER, this->scn_,
                      this->target_bda_);
      break;

    case BtEvent::SPP_OPEN:
      if (e.status == ESP_SPP_SUCCESS) {
        ESP_LOGD(TAG, "Bluetooth link open, starting handshake");
        this->spp_handle_ = e.handle;
        this->congested_ = false;
        this->lstate_ = 0;
        this->abuf_.clear();
        this->tx_queue_.clear();
        this->auth_ok_ = false;
        this->info_index_ = 0;
        this->challenge_attempts_ = 0;
        this->iq_.clear();
        this->i_seq_ = 0;
        this->last_rx_seq_ = 10000;
        this->channel_open_ = false;
        this->bstate_ = 0;
        this->set_state_(State::HANDSHAKE_OPEN);
        this->enqueue_i_(0x3F, {(uint8_t) (this->chan_id_ >> 8), (uint8_t) (this->chan_id_ & 0xFF), 0x01});
      } else {
        this->note_unreachable_("connect refused");
      }
      break;

    case BtEvent::SPP_CLOSE:
      this->spp_handle_ = 0;
      this->tx_queue_.clear();
      this->iq_.clear();
      if (this->was_connected_) {
        ESP_LOGI(TAG, "Charger disconnected (switched off?). Will keep looking.");
        this->was_connected_ = false;
        this->outage_logged_ = true;
        this->mark_offline_();
        this->backoff_ms_ = 10000;
        this->schedule_reconnect_();
      } else if (this->state_ != State::IDLE) {
        this->note_unreachable_("link dropped during setup");
      }
      break;

    case BtEvent::SPP_DATA:
      this->feed_link_(e.data.data(), e.data.size());
      break;

    case BtEvent::SPP_CONG:
      this->congested_ = e.flag;
      break;
  }
}

void SkyrcNC2500::loop() {
  std::deque<BtEvent> local;
  {
    std::lock_guard<std::mutex> lock(this->mutex_);
    local.swap(this->events_);
  }
  for (auto &e : local)
    this->handle_event_(e);

  const uint32_t now = millis();
  const uint32_t in_state = now - this->state_since_;

  switch (this->state_) {
    case State::STARTING:
    case State::FAILED:
      break;

    case State::IDLE:
      // Signed difference: still correct when millis() wraps after ~49.7 days
      if (this->enabled_ && this->spp_ready_ && (int32_t) (now - this->next_connect_at_) >= 0)
        this->begin_connect_();
      break;

    case State::INQUIRY:
      if (in_state > 20000) {
        esp_bt_gap_cancel_discovery();
        this->note_unreachable_("search timed out");
      }
      break;

    case State::SDP:
      if (in_state > 15000) {
        this->note_unreachable_("lookup timed out");
      }
      break;

    case State::CONNECTING:
      if (in_state > 20000) {
        if (this->spp_handle_ != 0)
          esp_spp_disconnect(this->spp_handle_);
        this->note_unreachable_("connect timed out");
      }
      break;

    case State::HANDSHAKE_OPEN:
      if (this->channel_open_) {
        this->set_state_(State::HANDSHAKE_INFO);
      } else if (in_state > 8000) {
        if (!this->warned_silent_) {
          ESP_LOGW(TAG, "Connected, but the charger's module did not answer. Giving up for now.");
          this->warned_silent_ = true;
        }
        if (this->spp_handle_ != 0)
          esp_spp_disconnect(this->spp_handle_);
        this->note_unreachable_("module not answering");
      }
      break;

    case State::HANDSHAKE_INFO:
      for (uint16_t id : INFO_IDS)
        this->send_session_(CMD_INFO_REQUEST, {(uint8_t) (id & 0xFF), (uint8_t) (id >> 8)});
      this->set_state_(State::HANDSHAKE_INFO_WAIT);
      break;

    case State::HANDSHAKE_INFO_WAIT:
      if (this->iq_.empty() && in_state > 1500) {
        this->send_session_(CMD_MODE, {0x01, 0x09});
        this->set_state_(State::HANDSHAKE_MODE);
      } else if (in_state > 15000) {
        this->note_unreachable_("setup stalled");
        if (this->spp_handle_ != 0)
          esp_spp_disconnect(this->spp_handle_);
      }
      break;

    case State::HANDSHAKE_MODE:
      if (this->iq_.empty() && in_state > 800) {
        if (this->skip_auth_check_) {
          this->set_state_(State::READY);
          this->last_poll_ = 0;
        } else {
          this->challenge_attempts_ = 1;
          this->send_challenge_();
          this->set_state_(State::HANDSHAKE_CHALLENGE);
        }
      }
      break;

    case State::HANDSHAKE_CHALLENGE:
      if (this->auth_ok_) {
        this->set_state_(State::READY);
        this->last_poll_ = 0;
      } else if (in_state > 1500) {
        if (this->challenge_attempts_ < 3) {
          this->challenge_attempts_++;
          this->send_challenge_();
          this->state_since_ = now;
        } else {
          if (!this->warned_auth_) {
            ESP_LOGW(TAG, "Charger did not answer the setup challenge as expected; carrying on anyway");
            this->warned_auth_ = true;
          } else {
            ESP_LOGD(TAG, "Challenge not answered as expected; carrying on");
          }
          this->set_state_(State::READY);
          this->last_poll_ = 0;
        }
      }
      break;

    case State::READY:
      if (this->last_poll_ == 0) {
        if (this->outage_logged_ || !this->published_connected_)
          ESP_LOGI(TAG, "Charger connected");
        this->outage_logged_ = false;
        this->offline_published_ = false;
        this->was_connected_ = true;
        this->last_rx_ms_ = now;
        this->publish_connected_(true);
        this->backoff_ms_ = 5000;
        this->send_query_version_();
        this->last_poll_ = now;
        this->send_query_status_();
      } else if (now - this->last_poll_ >= this->poll_interval_ms_) {
        this->last_poll_ = now;
        this->send_query_status_();
      }
      // Connected but silent (charger switched off while the link was up): let go quietly and start searching again.
      if (now - this->last_rx_ms_ > std::max<uint32_t>(this->poll_interval_ms_ * 4, 60000)) {
        ESP_LOGI(TAG, "Charger stopped answering. Will keep looking.");
        this->was_connected_ = false;
        this->outage_logged_ = true;
        this->mark_offline_();
        if (this->spp_handle_ != 0)
          esp_spp_disconnect(this->spp_handle_);
        this->backoff_ms_ = 10000;
        this->schedule_reconnect_();
      }
      break;
  }

  this->pump_i_();
  this->pump_tx_();
}

// ---------------------------------------------------------------------------------------------
// Link layer
// ---------------------------------------------------------------------------------------------

uint8_t SkyrcNC2500::link_checksum_(const uint8_t *p, size_t n) {
  uint8_t sum = (uint8_t) (n & 0xFF) + (uint8_t) ((n >> 8) & 0xFF);
  for (size_t i = 0; i < n; i++)
    sum += p[i];
  return (uint8_t) (~sum + 1);
}

std::string SkyrcNC2500::hex_(const uint8_t *d, size_t n) const {
  std::string s;
  char b[4];
  for (size_t i = 0; i < n; i++) {
    snprintf(b, sizeof(b), "%02X ", d[i]);
    s += b;
  }
  return s;
}

void SkyrcNC2500::queue_tx_(std::vector<uint8_t> &&frame) {
  if (this->tx_queue_.size() < 32)
    this->tx_queue_.push_back(std::move(frame));
}

void SkyrcNC2500::pump_tx_() {
  if (this->spp_handle_ == 0 || this->tx_queue_.empty() || this->congested_)
    return;
  const uint32_t now = millis();
  if (now - this->last_tx_ < 25)
    return;
  auto &f = this->tx_queue_.front();
  esp_err_t err = esp_spp_write(this->spp_handle_, f.size(), f.data());
  if (err != ESP_OK)
    ESP_LOGD(TAG, "write failed: %s", esp_err_to_name(err));
  this->last_tx_ = now;
  this->tx_queue_.pop_front();
}

void SkyrcNC2500::send_link_(const uint8_t *payload, size_t n) {
  std::vector<uint8_t> f;
  f.reserve(n + 5);
  f.push_back(LINK_SOF);
  if (n <= 252) {
    f.push_back((uint8_t) n);
  } else {
    f.push_back(0x00);
    f.push_back((uint8_t) (n >> 8));
    f.push_back((uint8_t) (n & 0xFF));
  }
  f.insert(f.end(), payload, payload + n);
  f.push_back(link_checksum_(payload, n));
  if (this->log_frames_)
    ESP_LOGI(TAG, "TX %s", this->hex_(payload, n).c_str());
  this->queue_tx_(std::move(f));
}

void SkyrcNC2500::send_session_(uint8_t cmd, const std::vector<uint8_t> &data) {
  std::vector<uint8_t> p;
  p.reserve(data.size() + 2);
  p.push_back(SESSION_ID);
  p.push_back(cmd);
  p.insert(p.end(), data.begin(), data.end());
  this->send_channel_(p);
}

// ---------------------------------------------------------------------------------------------
// Adapter + module layers
// ---------------------------------------------------------------------------------------------

uint8_t SkyrcNC2500::b_checksum_(const uint8_t *d, size_t n) {
  const bool is_long = n + 4 >= 256;
  uint8_t sum = (uint8_t) (n & 0xFF) + (uint8_t) ((n >> 8) & 0xFF) + (is_long ? 90 : 88);
  for (size_t i = 0; i < n; i++)
    sum += d[i];
  return (uint8_t) (~sum + 1);
}

// Wrap a session message as 95 19 | total | message | checksum and send it as numbered data on our channel.
void SkyrcNC2500::send_channel_(const std::vector<uint8_t> &message) {
  if (message.size() + 4 >= 256 || message.size() > 118)
    return;  // we never build anything this big
  std::vector<uint8_t> payload;
  payload.reserve(message.size() + 7);
  payload.push_back((uint8_t) (this->chan_id_ >> 8));
  payload.push_back((uint8_t) (this->chan_id_ & 0xFF));
  payload.push_back(0x95);
  payload.push_back(0x19);
  payload.push_back((uint8_t) (message.size() + 4));
  payload.insert(payload.end(), message.begin(), message.end());
  payload.push_back(b_checksum_(message.data(), message.size()));
  this->enqueue_i_(0x43, payload);
}

void SkyrcNC2500::enqueue_i_(uint8_t cmd, const std::vector<uint8_t> &data) {
  if (this->iq_.size() >= 24)
    return;  // the charger isn't keeping up; drop rather than pile up
  IItem it;
  it.cmd = cmd;
  it.frame.reserve(data.size() + 4);
  it.frame.push_back(0x00);
  it.frame.push_back(cmd);
  it.frame.push_back((uint8_t) (this->i_seq_ >> 8));
  it.frame.push_back((uint8_t) (this->i_seq_ & 0xFF));
  it.frame.insert(it.frame.end(), data.begin(), data.end());
  this->i_seq_++;
  this->iq_.push_back(std::move(it));
}

// Only the first message in the queue is on the wire; it is resent every 500 ms until acknowledged.
void SkyrcNC2500::pump_i_() {
  if (this->iq_.empty() || this->spp_handle_ == 0)
    return;
  auto &it = this->iq_.front();
  const uint32_t now = millis();
  if (it.tries == 0 || now - it.last_sent >= 500) {
    if (it.tries >= 20) {
      ESP_LOGD(TAG, "No acknowledgement after %u tries, dropping", it.tries);
      this->iq_.pop_front();
      return;
    }
    it.tries++;
    it.last_sent = now;
    this->send_link_(it.frame.data(), it.frame.size());
  }
}

void SkyrcNC2500::handle_i_frame_(const uint8_t *p, size_t n) {
  // p = 00 | cmd | seq(2) | ...
  const uint8_t cmd = p[1];
  const uint16_t seq = (uint16_t) (p[2] << 8) | p[3];
  if (cmd == 0x42) {
    // Data from the charger on a channel. Acknowledge it, and pass it on once.
    uint8_t result = 1;  // unknown channel
    if (n >= 6) {
      const uint16_t chan = (uint16_t) (p[4] << 8) | p[5];
      if (chan == this->chan_id_ && this->channel_open_) {
        result = 0;
        if (seq != this->last_rx_seq_) {
          this->last_rx_seq_ = seq;
          this->feed_b_(p + 6, n - 6);
        }
      } else if (chan == this->chan_id_) {
        result = 2;  // channel not open yet
      }
    }
    const uint8_t ack[6] = {0x00, 0x02, p[2], p[3], result, 0x42};
    this->send_link_(ack, sizeof(ack));
    return;
  }
  if (this->iq_.empty())
    return;
  auto &head = this->iq_.front();
  const uint16_t head_seq = (uint16_t) (head.frame[2] << 8) | head.frame[3];
  if (seq != head_seq)
    return;
  bool acked = false;
  bool ok = true;
  if (cmd == 0x41 && n >= 6 && p[5] == head.cmd) {
    acked = true;
    ok = p[4] == 0;
  } else if (cmd == (uint8_t) (head.cmd + 1)) {
    acked = true;
  }
  if (!acked)
    return;
  if (head.cmd == 0x3F) {
    if (ok) {
      this->channel_open_ = true;
      ESP_LOGD(TAG, "Data channel open");
    } else {
      ESP_LOGD(TAG, "Charger module refused the data channel");
    }
  }
  this->iq_.pop_front();
}

// Adapter stream parser: 95 19 | len | message | checksum (a zero length byte means a 16-bit length follows).
void SkyrcNC2500::feed_b_(const uint8_t *data, size_t len) {
  // bstate_: 0 = 0x95, 1 = 0x19, 2 = length, 3 = length high, 4 = length low, 5 = message, 6 = checksum
  for (size_t i = 0; i < len; i++) {
    const uint8_t b = data[i];
    switch (this->bstate_) {
      case 0:
        if (b == 0x95)
          this->bstate_ = 1;
        break;
      case 1:
        this->bstate_ = b == 0x19 ? 2 : (b == 0x95 ? 1 : 0);
        break;
      case 2:
        if (b == 0) {
          this->bstate_ = 3;
        } else if (b < 6) {
          this->bstate_ = 0;
        } else {
          this->bexpect_ = b - 4;
          this->bgot_ = 0;
          this->bbuf_.assign(this->bexpect_, 0);
          this->bstate_ = 5;
        }
        break;
      case 3:
        this->bexpect_ = (uint16_t) b << 8;
        this->bstate_ = 4;
        break;
      case 4:
        this->bexpect_ += b;
        if (this->bexpect_ < 8 || this->bexpect_ > 400) {
          this->bstate_ = 0;
        } else {
          this->bexpect_ -= 6;
          this->bgot_ = 0;
          this->bbuf_.assign(this->bexpect_, 0);
          this->bstate_ = 5;
        }
        break;
      case 5:
        this->bbuf_[this->bgot_++] = b;
        if (this->bgot_ >= this->bexpect_)
          this->bstate_ = 6;
        break;
      case 6:
        if (b_checksum_(this->bbuf_.data(), this->bexpect_) == b) {
          if (this->bbuf_[0] == SESSION_ID)
            this->handle_session_(this->bbuf_.data(), this->bexpect_);
        } else {
          ESP_LOGD(TAG, "Dropped an adapter frame with a bad checksum");
        }
        this->bstate_ = 0;
        break;
    }
  }
}

void SkyrcNC2500::send_rfa_(const std::vector<uint8_t> &data) {
  std::vector<uint8_t> p;
  p.push_back(RFA_TYPE);
  p.push_back((uint8_t) data.size());
  p.insert(p.end(), data.begin(), data.end());
  this->send_session_(CMD_RFA_WRITE, p);
}

void SkyrcNC2500::feed_link_(const uint8_t *data, size_t len) {
  // lstate_: 0 = start byte, 1 = length, 2 = length high, 3 = length low, 4 = payload, 5 = checksum
  for (size_t i = 0; i < len; i++) {
    uint8_t b = data[i];
    switch (this->lstate_) {
      case 0:
        if (b == LINK_SOF)
          this->lstate_ = 1;
        break;
      case 1:
        if (b == 0) {
          this->lstate_ = 2;
        } else if (b > 252) {
          this->lstate_ = 0;
        } else {
          this->lexpect_ = b;
          this->lgot_ = 0;
          this->lbuf_.assign(b, 0);
          this->lstate_ = 4;
        }
        break;
      case 2:
        this->lexpect_ = (uint16_t) b << 8;
        this->lstate_ = 3;
        break;
      case 3:
        this->lexpect_ += b;
        if (this->lexpect_ == 0 || this->lexpect_ > 300) {
          this->lstate_ = 0;
        } else {
          this->lgot_ = 0;
          this->lbuf_.assign(this->lexpect_, 0);
          this->lstate_ = 4;
        }
        break;
      case 4:
        this->lbuf_[this->lgot_++] = b;
        if (this->lgot_ >= this->lexpect_)
          this->lstate_ = 5;
        break;
      case 5:
        if (link_checksum_(this->lbuf_.data(), this->lexpect_) == b) {
          this->handle_link_payload_(this->lbuf_.data(), this->lexpect_);
        } else {
          ESP_LOGD(TAG, "Dropped a frame with a bad checksum");
        }
        this->lstate_ = 0;
        break;
    }
  }
}

void SkyrcNC2500::handle_link_payload_(const uint8_t *p, size_t n) {
  this->last_rx_ms_ = millis();
  if (this->log_frames_)
    ESP_LOGI(TAG, "RX %s", this->hex_(p, n).c_str());
  if (n >= 4 && p[0] == 0x00)
    this->handle_i_frame_(p, n);
}

void SkyrcNC2500::handle_session_(const uint8_t *p, size_t n) {
  if (this->log_frames_)
    ESP_LOGI(TAG, "Session message: %s", this->hex_(p, n).c_str());
  if (n < 2 || p[0] != SESSION_ID)
    return;
  uint8_t cmd = p[1];
  if (cmd == CMD_CHALLENGE_REPLY && n >= 18) {
    uint8_t expect[16];
    compute_response_(this->challenge_, expect);
    if (memcmp(expect, p + 2, 16) == 0) {
      ESP_LOGD(TAG, "Charger answered the challenge correctly");
      this->auth_ok_ = true;
    } else {
      ESP_LOGD(TAG, "Challenge answer differs from the expected one: %s", this->hex_(p + 2, 16).c_str());
    }
  } else if (cmd == CMD_RFA_DATA && n >= 5 && (p[2] == 0x00 || p[2] == 0x80)) {
    size_t dlen = std::min<size_t>(p[4], n - 5);
    this->feed_app_(p + 5, dlen);
  }
}

// ---------------------------------------------------------------------------------------------
// Challenge (the charger proves it is genuine; we can also just skip it)
// ---------------------------------------------------------------------------------------------

static void rotate_right_128(uint8_t *a) {
  uint8_t carry = a[15] & 1;
  for (int i = 15; i >= 1; i--)
    a[i] = (uint8_t) ((a[i] >> 1) | ((a[i - 1] & 1) << 7));
  a[0] = (uint8_t) ((a[0] >> 1) | (carry << 7));
}

void SkyrcNC2500::compute_response_(const uint8_t *challenge, uint8_t *out) {
  memcpy(out, challenge, 16);
  uint32_t k[2] = {557064756u, 664220296u};
  for (uint32_t word : k) {
    for (int bit = 0; bit < 32; bit++) {
      if (word & 1) {
        for (int i = 0; i < 16; i++)
          out[i] ^= AUTH_KEY[i];
      }
      rotate_right_128(out);
      word >>= 1;
    }
  }
}

void SkyrcNC2500::send_challenge_() {
  for (int i = 0; i < 16; i += 4) {
    uint32_t r = esp_random();
    memcpy(this->challenge_ + i, &r, 4);
  }
  this->send_session_(CMD_CHALLENGE, std::vector<uint8_t>(this->challenge_, this->challenge_ + 16));
}

// ---------------------------------------------------------------------------------------------
// Charger frames
// ---------------------------------------------------------------------------------------------

void SkyrcNC2500::send_query_status_() { this->send_rfa_({0x0F, 0x03, 0x55, 0x00, 0x55, 0xFF, 0xFF}); }
void SkyrcNC2500::send_query_version_() { this->send_rfa_({0x0F, 0x03, 0x57, 0x00, 0x57, 0xFF, 0xFF}); }

void SkyrcNC2500::feed_app_(const uint8_t *data, size_t len) {
  this->abuf_.insert(this->abuf_.end(), data, data + len);
  if (this->abuf_.size() > 512)
    this->abuf_.clear();
  while (!this->abuf_.empty()) {
    if (this->abuf_[0] != 0x0F) {
      this->abuf_.erase(this->abuf_.begin());
      continue;
    }
    if (this->abuf_.size() < 2)
      return;
    size_t total = (size_t) this->abuf_[1] + 4;
    if (total < 5 || total > 200) {
      this->abuf_.erase(this->abuf_.begin());
      continue;
    }
    if (this->abuf_.size() < total)
      return;
    this->handle_app_frame_(this->abuf_.data(), total);
    this->abuf_.erase(this->abuf_.begin(), this->abuf_.begin() + total);
  }
}

void SkyrcNC2500::handle_app_frame_(const uint8_t *f, size_t n) {
  if (this->log_frames_)
    ESP_LOGI(TAG, "Charger frame (%u bytes): %s", (unsigned) n, this->hex_(f, n).c_str());
  if (n == 4 + NUM_SLOTS * SLOT_RECORD_LEN + 3) {
    this->parse_status_(f, n);
  } else if (f[2] == 0x57) {
    ESP_LOGI(TAG, "Charger identity: %s", this->hex_(f + 2, n - 2).c_str());
  }
}

void SkyrcNC2500::parse_status_(const uint8_t *f, size_t n) {
  uint8_t sum = 0;
  for (size_t i = 2; i < n - 3; i++)
    sum += f[i];
  if (sum != f[n - 3])
    ESP_LOGD(TAG, "Status checksum mismatch (got %02X, expected %02X) - using it anyway", f[n - 3], sum);

  for (uint8_t i = 0; i < NUM_SLOTS; i++) {
    const uint8_t *r = f + 4 + i * SLOT_RECORD_LEN;
    const uint8_t mode = r[0];
    const bool present = r[1] != 0;
    const float mah = (r[2] << 8) | r[3];
    const float secs = (r[4] << 8) | r[5];
    const float volts = ((r[6] << 8) | r[7]) / 1000.0f;
    const float amps = ((r[8] << 8) | r[9]) / 1000.0f;
    const uint8_t doing = r[10];
    const float temp = (float) r[11] - 40.0f;
    const float cycles = r[12];
    const float milliohm = (r[13] << 8) | r[14];

    const char *mode_s = mode < sizeof(MODE_NAMES) / sizeof(MODE_NAMES[0]) ? MODE_NAMES[mode] : "Unknown";
    const char *state_s = doing < sizeof(STATE_NAMES) / sizeof(STATE_NAMES[0]) ? STATE_NAMES[doing] : "Unknown";
    if (!present) {
      mode_s = "Empty";
      state_s = "Empty";
    }
    if (this->log_frames_)
      ESP_LOGI(TAG, "Slot %u: %s, mode %s, state %s, %.3f V, %.3f A, %.0f mAh, %.0f s, %.0f C, cycle %.0f, %.0f mOhm", i + 1,
               present ? "battery" : "empty", mode_s, state_s, volts, amps, mah, secs, temp, cycles, milliohm);

    this->slot_present_[i] = present;
    auto &s = this->slots_[i];
    if (s.present != nullptr) s.present->publish_state(present);
    if (s.mode != nullptr) s.mode->publish_state(mode_s);
    if (s.state != nullptr) s.state->publish_state(state_s);
    if (s.summary != nullptr) {
      // One short line for dashboards, e.g. "Charging 1.35 V 0.20 A 148 mAh"
      char line[64];
      if (!present)
        snprintf(line, sizeof(line), "Empty");
      else if (doing == 1 || doing == 3)
        snprintf(line, sizeof(line), "%s %.2f V %.2f A %.0f mAh", state_s, volts, amps, mah);
      else
        snprintf(line, sizeof(line), "%s %.2f V %.0f mAh", state_s, volts, mah);
      s.summary->publish_state(line);
    }
    if (s.voltage != nullptr) s.voltage->publish_state(present ? volts : NAN);
    if (s.current != nullptr) s.current->publish_state(present ? amps : NAN);
    if (s.capacity != nullptr) s.capacity->publish_state(present ? mah : NAN);
    if (s.temperature != nullptr) s.temperature->publish_state(present ? temp : NAN);
    if (s.run_time != nullptr) s.run_time->publish_state(present ? secs / 60.0f : NAN);
    if (s.resistance != nullptr) s.resistance->publish_state(present ? milliohm : NAN);
    if (s.cycles != nullptr) s.cycles->publish_state(present ? cycles : NAN);
  }
}

// Limits and steps are the ones the vendor app enforces.
static float clamp_step(float v, float lo, float hi, float step) {
  if (std::isnan(v))
    v = lo;
  v = std::min(std::max(v, lo), hi);
  return std::round(v / step) * step;
}

bool SkyrcNC2500::apply_slot(uint8_t slot, const std::string &mode, float charge_a, float discharge_a,
                             float capacity_mah, float cycles) {
  if (slot >= NUM_SLOTS)
    return false;
  if (this->state_ != State::READY) {
    ESP_LOGW(TAG, "Slot %u: not applied - the charger is not connected", slot + 1);
    return false;
  }
  if (!this->slot_present_[slot]) {
    ESP_LOGW(TAG, "Slot %u: not applied - there is no battery in it", slot + 1);
    return false;
  }
  uint8_t mode_id;
  if (mode == "Charge") mode_id = 0;
  else if (mode == "Refresh") mode_id = 1;
  else if (mode == "Break-In") mode_id = 2;
  else if (mode == "Discharge") mode_id = 3;
  else if (mode == "Cycle") mode_id = 4;
  else {
    ESP_LOGW(TAG, "Slot %u: not applied - unknown mode '%s'", slot + 1, mode.c_str());
    return false;
  }

  const uint16_t chg = (uint16_t) clamp_step(charge_a * 1000.0f, 200, 2500, 100);
  const uint16_t dis = (uint16_t) clamp_step(discharge_a * 1000.0f, 100, 1000, 100);
  const uint16_t cap = (uint16_t) clamp_step(capacity_mah, 500, 3500, 100);
  const uint8_t cyc = (uint8_t) clamp_step(cycles, 1, 12, 1);

  // 0F 0D 05 <slot> <mode> <v1 hi lo> <v2 hi lo> <v3 hi lo> <cycles> 00 00 <chk> FF FF
  uint8_t p[17] = {0x0F, 0x0D, 0x05, slot, mode_id};
  switch (mode_id) {
    case 0:  // Charge
      p[5] = chg >> 8; p[6] = chg & 0xFF;
      break;
    case 1:  // Refresh: charge, then discharge
      p[5] = chg >> 8; p[6] = chg & 0xFF;
      p[7] = dis >> 8; p[8] = dis & 0xFF;
      break;
    case 2:  // Break-In: needs the rated capacity
      p[9] = cap >> 8; p[10] = cap & 0xFF;
      break;
    case 3:  // Discharge
      p[7] = dis >> 8; p[8] = dis & 0xFF;
      break;
    case 4:  // Cycle: charge, discharge, number of cycles
      p[5] = chg >> 8; p[6] = chg & 0xFF;
      p[7] = dis >> 8; p[8] = dis & 0xFF;
      p[11] = cyc;
      break;
  }
  uint8_t sum = 0;
  for (int i = 2; i < 14; i++)
    sum += p[i];
  p[14] = sum;
  p[15] = 0xFF;
  p[16] = 0xFF;

  ESP_LOGI(TAG, "Slot %u: starting %s (charge %u mA, discharge %u mA, capacity %u mAh, cycles %u)", slot + 1,
           mode.c_str(), mode_id == 0 || mode_id == 1 || mode_id == 4 ? chg : 0,
           mode_id == 1 || mode_id == 3 || mode_id == 4 ? dis : 0, mode_id == 2 ? cap : 0, mode_id == 4 ? cyc : 0);
  this->send_rfa_(std::vector<uint8_t>(p, p + sizeof(p)));
  // Ask for the new state shortly afterwards instead of waiting for the next poll.
  this->last_poll_ = millis() - this->poll_interval_ms_ + 2000;
  return true;
}

bool SkyrcNC2500::stop_slot(uint8_t slot) {
  if (slot >= NUM_SLOTS)
    return false;
  if (this->state_ != State::READY) {
    ESP_LOGW(TAG, "Slot %u: not stopped - the charger is not connected", slot + 1);
    return false;
  }
  if (!this->slot_present_[slot]) {
    ESP_LOGW(TAG, "Slot %u: not stopped - there is no battery in it", slot + 1);
    return false;
  }

  // A set-slot with mode 5 (Done) and no values ends the slot's program:
  // 0F 0D 05 <slot> 05 00 00 00 00 00 00 00 00 00 <chk> FF FF
  uint8_t p[17] = {0x0F, 0x0D, 0x05, slot, 0x05};
  uint8_t sum = 0;
  for (int i = 2; i < 14; i++)
    sum += p[i];
  p[14] = sum;
  p[15] = 0xFF;
  p[16] = 0xFF;

  ESP_LOGI(TAG, "Slot %u: stopping", slot + 1);
  this->send_rfa_(std::vector<uint8_t>(p, p + sizeof(p)));
  // Ask for the new state shortly afterwards instead of waiting for the next poll.
  this->last_poll_ = millis() - this->poll_interval_ms_ + 2000;
  return true;
}

}  // namespace skyrc_nc2500
}  // namespace esphome

#endif  // USE_ESP32
