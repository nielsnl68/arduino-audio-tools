#pragma once
#define NO_GLOBAL_ESP_NOW 1

#include <new>  //std::nothrow
#include <vector>
// #include <string>
// #include <sstream>
// #include <vector>
// #include <functional>
#include "AudioTools/Communication/ESPNowStream.h"
#include "AudioTools/Concurrency/RTOS.h"
#include "AudioTools/CoreAudio/BaseStream.h"
#include "esp_crc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"

namespace audio_tools {

/// Request message codes :
enum {
  NOWTALK_PING = 0x01,
  NOWTALK_PONG = 0x02,

  NOWTALK_SERVER_REQUEST_DETAILS = 0x03,
  NOWTALK_CLIENT_DETAILS = 0x04,
  NOWTALK_CLIENT_NEWPEER = 0x05,

  NOWTALK_CLIENT_REGISTER = 0x06,
  NOWTALK_CLIENT_REMOVE = 0x07,
  NOWTALK_SERVER_ACCEPT = 0x08,

  NOWTALK_SERVER_NEW_BADGE_NAME = 0x0d,
  NOWTALK_SERVER_NEW_ACCESS_KEY = 0x0e,

  NOWTALK_ACK = 0x10,
  NOWTALK_NACK = 0x11,

  NOWTALK_CLIENT_START_CALL = 0x30,
  NOWTALK_SERVER_SEND_PEER = 0x31,
  NOWTALK_SERVER_PEER_GONE = 0x32,
  NOWTALK_SERVER_OVER_WEB = 0x33,

  NOWTALK_CLIENT_REQUEST = 0x37,
  NOWTALK_CLIENT_RECEIVE = 0x38,
  NOWTALK_CLIENT_CLOSED = 0x39,

  NOWTALK_STREAM_START = 0x3d,
  NOWTALK_STREAM_DATA = 0x3e,
  NOWTALK_STREAM_END = 0x3df,

  NOWTALK_CLIENT_HELPSOS = 0xff,
};

// clang-format on

class NowTalkStream;

using NowTalkCallback =
    std::function<void(const uint8_t* peer, uint8_t opcode, uint8_t* data,
                       size_t len, bool broadcast, uint8_t rssi)>;

struct NowTalkData {
  uint8_t opcode;
  uint8_t data[MY_ESP_NOW_MAX_LEN];
  ESPNowAddress mac;
  bool isBroadcasted;
  uint8_t RSSI;
  size_t len;
};

/**
 * @brief Configuration for ESP-NOW protocol
 * @author Phil Schatzmann
 * @copyright GPLv3
 */
struct NowTalkStreamConfig : ESPNowStreamConfig {
  ESPNowAddress dest_address = {0, 0, 0, 0, 0, 0};
  NowTalkCallback nowtalk_cb = nullptr;
};

String badgeID() {
  String baseChars = "0123456789AbCdEfGhIjKlMnOpQrStUvWxYz";  // aBcDeFgHiJkLmNoPqRsTuVwXyZ
  uint8_t base = baseChars.length();
  String result = "";
  uint32_t chipId = 0xa5000000;
  uint8_t crc = 0;
  uint64_t fusedMac = 0LL;
  esp_efuse_mac_get_default((uint8_t *) (&fusedMac));
  for (int i = 0; i < 17; i = i + 8) {
    chipId |= ((fusedMac >> (40 - i)) & 0xff) << i;
  }

  do {
    result = baseChars[chipId % base] + result;  // Add on the left
    crc += chipId % base;
    chipId /= base;
  } while (chipId != 0);
  return result + baseChars[crc % base];
}

std::vector<String> split(const String &s, char delim) {
  std::vector<String> elems;
  size_t pos = s.indexOf(delim);
  size_t from = 0;
  while (pos >= 0) {
    elems.push_back(s.substring(from, pos));
    from = pos + 1;
    pos = s.indexOf(delim, from);
  }
  elems.push_back(s.substring(from));
  return elems;
}

/**
 * @brief ESPNow as Arduino Stream.
 * @ingroup communications
 * @author Phil Schatzmann
 * @copyright GPLv3
 */
class NowTalkStream : public ESPNowStream {
 public:
  NowTalkStream() {};

  ~NowTalkStream() {
    if (xSemaphore != nullptr) vSemaphoreDelete(xSemaphore);
  }

  NowTalkStreamConfig defaultConfig() {
    NowTalkStreamConfig result;
    return result;
  }

  /// Initialization of ESPNow
  bool begin() { return begin(cfg); }

  /// Initialization of ESPNow incl WIFI
  bool begin(NowTalkStreamConfig cfg) {
    this->cfg = cfg;

    taskHandler.begin([this]() {
      NowTalkData msg;
      std::vector<String> parts;

      if (this->gueue.dequeue(msg)) {
        switch (msg.opcode) {
          case NOWTALK_CLIENT_REGISTER:
          case NOWTALK_ACK:
          case NOWTALK_NACK:
            break;
          case NOWTALK_CLIENT_REMOVE:
            break;

          case NOWTALK_PING:
            // do not respond on Ping messages
            break;

          case NOWTALK_PONG:
            parts = this->split_((char*)&msg.data);
            // turn off timeout
            // this->init_ping_timeout(true);
            if (parts.size() > 1) {
              /*
              std::string date = parts[1];
              configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
              tm xtm = createTM(date.c_str());
              setTime(makeTime(xtm))
              */

              //  ESP_LOGD(TAG, config.timerPing);

              bool isNewMaster = (this->current_server_mac_[0] == 0);
              if (isNewMaster) {
                //this->set_current_server_mac((uint8_t*)&packet.mac);
                if (this->main_server_mac_[0] == 0) {
                  memcpy(this->main_server_mac_, msg.mac, 6);
                  this->main_server_name_ = parts[0];
                  //this->save_configuration();
                }
                this->addPeer(msg.mac);
              }
            }

            if (this->wakeup_) {
              this->goto_sleep();
            }
            break;

          case NOWTALK_SERVER_REQUEST_DETAILS:
            if ((this->main_server_name_ != "") && (this->badge_name_ != "")) {
              ESP_LOGI("Handler", "Detail request from: " MACSTR,
                       MAC2STR(msg.mac));
              this->addPeer(msg.mac);

              this->sendPacket(NOWTALK_CLIENT_DETAILS,
                         this->main_server_access_key_ + "~" +
                             this->badge_name_ + "~" + badgeID());
              esp_now_del_peer(msg.mac);
              return;
            }
            break;

          case NOWTALK_SERVER_NEW_ACCESS_KEY:
            parts = this->split_((char*)&msg.data);
            if (parts.size() > 1) {
              String OldIP = parts[0];
              String NewIP = parts[1];
              if (this->main_server_access_key_ == OldIP) {
                this->main_server_access_key_ = NewIP;
                this->sendPacket(NOWTALK_CLIENT_ACK);
                ESP_LOGI(TAG, "Update IP to: \n%s",
                         this->main_server_access_key_.c_str());
                break;
              }
            }
            break;

          case NOWTALK_SERVER_NEW_BADGE_NAME:
            parts = this->split_((char*)&msg.data);
            if (parts.size() > 1) {
              String OldName = parts[0];
              String NewName = parts[1];
              if (this->badge_name_ == OldName) {
                this->badge_name_ = NewName;  // <- destination's capacity
                this->sendPacket(NOWTALK_CLIENT_ACK);
                ESP_LOGI(TAG, "Update Name to: \n%s",
                         this->badge_name_.c_str());
                return;
              }
            }
            break;
          case NOWTALK_STREAM_DATA:
            // make sure that the receive buffer is available - moved from begin
            // to make sure that it is only allocated when needed
            ESPNowStream::handle_recv_cb(msg.mac, msg.data, msg.len,
                                         msg.isBroadcasted, msg.RSSI);

          default:
            if (cfg.nowtalk_cb != nullptr) {
              cfg.nowtalk_cb(msg.mac, msg.opcode, msg.data, msg.len,
                             msg.isBroadcasted, msg.RSSI);
            } else {
              ESP_LOGI(TAG, "Unknown command: " MACSTR " [%02x] %s",
                       MAC2STR(msg.mac), msg.opcode, msg.data);
            }
        }
        this->send(msg.mac, NOWTALK_CLIENT_NACK);
      }
    });

    return ESPNowStream::begin((ESPNowStreamConfig)cfg);
  }

  /// DeInitialization
  void end() override { ESPNowStream::end(); }

  void setDestination(ESPNowAddress address) { cfg.dest_address = address; }

  bool sendPacket(uint8_t opcode, std::string msg) {
    int retry_count = 0;
    return this->sendPacket(opcode, (const uint8_t*)msg.data(), msg.size(),
                            retry_count);
  }
  bool sendPacket(uint8_t opcode, String msg) {
    int retry_count = 0;
    return this->sendPacket(opcode, (const uint8_t*)msg.c_str(), msg.length(),
                            retry_count);
  }
  bool sendPacket(uint8_t opcode, const uint8_t* data = nullptr,
                  size_t len = 0) {
    int retry_count = 0;
    return sendPacket(opcode, data, len, retry_count);
  }

  bool sendPacket(uint8_t opcode, const uint8_t* data, size_t len,
                  int& retry_count) {
    bool err = false;
    NowTalkData mydata = {.opcode = opcode};
    if (data != nullptr && len > 0) {
      memcpy((void*)&mydata.data, data, len);
    } else {
      len = 0;
    }
    if (cfg.dest_address[0] != 0) {
      if (!esp_now_is_peer_exist(cfg.dest_address.data())) {
        addPeer(cfg.dest_address);
      }
      err = ESPNowStream::sendPacket((const uint8_t*)&mydata, len + 1,
                                     retry_count, cfg.dest_address.data());
    } else {
      err = ESPNowStream::sendPacket((const uint8_t*)&mydata, len + 1,
                                     retry_count, nullptr);
    }
    return err;
  }

 private:
  NowTalkStreamConfig cfg;

 protected:
  BufferRTOS<uint8_t> buffer{0};
  QueueRTOS<NowTalkData> queue{10};  //(10);
  Task taskHandler{"dlna", 8000, 10, 0};

  boolean wakeup_ = false;
  String badge_name_ = "";
  String main_server_name_ = "";
  String main_server_access_key_ = "";
  uint8_t main_server_mac_[6] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};


  bool sendPacket(const uint8_t* data, size_t len, int& retry_count,
                  const uint8_t* destination = nullptr) override {
    return sendPacket(0x3e, data, len, retry_count);
  }

  std::vector<String> split_(char* payload) {
    String str(payload);
    return split(str, '~');
  }

  void handle_recv_cb(const uint8_t* mac_addr, const uint8_t* data,
                      int data_len, bool broadcast, uint8_t rssi) override {
    NowTalkData msg;
    memcpy((void*)&msg, data, data_len);
    memcpy(msg.mac.data(), mac_addr, 6);
    msg.isBroadcasted = broadcast;
    msg.RSSI = rssi;
    msg.len = data_len - 1;
    queue.enqueue(msg);
  }
};
}  // namespace audio_tools
