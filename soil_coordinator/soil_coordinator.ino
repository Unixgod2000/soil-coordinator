/* ============================================================
 * Soil Coordinator — AY-303Z Zigbee soil sensors (Tuya _TZE284_, batch _2547_)
 * SENSORS (bind by IEEE; short changes on rejoin):
 *   1: a4c13815952fbfdc (0x7d5d)   2: a4c138505b4ece6a (0x0af6)   3: a4c138f03debeaad (0x5c6b)
 * DP MAP: 3=soil_moisture  5=temp/10°C  15=battery%  106=moisture_warning(bool)
 *         109=fixed threshold(ignored)  9/102/104/105/110/111/112=calibration(ignored)
 * ============================================================ */

#ifndef ZIGBEE_MODE_ZCZR
#error "Set Tools > Zigbee Mode > Zigbee ZCZR (coordinator/router)"
#endif

#define DEBUG_RAW          0
#define ROSTER_INTERVAL_MS 30000

#include "Zigbee.h"
#include <map>

bool zb_apsde_data_indication_handler(esp_zb_apsde_data_ind_t ind);  // library's, for chaining

static std::map<uint16_t, String> known;   // short -> IEEE string (loop-task only)
static uint32_t last_roster = 0;

static String ieee_to_string(esp_zb_ieee_addr_t ieee) {
  char s[17];
  sprintf(s, "%02x%02x%02x%02x%02x%02x%02x%02x",
          ieee[7], ieee[6], ieee[5], ieee[4], ieee[3], ieee[2], ieee[1], ieee[0]);
  return String(s);
}

static void emit_join(uint16_t s, const String &ieee) {
  Serial.printf("{\"event\":\"join\",\"ieee\":\"%s\",\"short\":\"0x%04x\"}\n", ieee.c_str(), s);
}

static void poll_devices() {
  esp_zb_nwk_info_iterator_t it = ESP_ZB_NWK_INFO_ITERATOR_INIT;
  esp_zb_nwk_neighbor_info_t nb;
  esp_zb_lock_acquire(portMAX_DELAY);
  while (esp_zb_nwk_get_next_neighbor(&it, &nb) == ESP_OK) {
    if (nb.short_addr == 0x0000) continue;
    String ieee = ieee_to_string(nb.ieee_addr);
    if (known.find(nb.short_addr) == known.end() || known[nb.short_addr] != ieee) {
      known[nb.short_addr] = ieee;
      emit_join(nb.short_addr, ieee);          // new device or reassigned short
    }
  }
  esp_zb_lock_release();
}

static void emit_roster() { for (auto &kv : known) emit_join(kv.first, kv.second); }

static void decode_tuya(uint16_t src, const uint8_t *a, uint8_t len) {
  if (len < 3) return;
  uint8_t cmd = a[2];
  if (cmd != 0x01 && cmd != 0x02) return;
  uint16_t i = 5;
  while (i + 4 <= len) {
    uint8_t  dp   = a[i];
    uint16_t dlen = (a[i + 2] << 8) | a[i + 3];
    i += 4;
    if (i + dlen > len) break;
    int32_t val = 0;
    for (uint16_t k = 0; k < dlen; k++) val = (val << 8) | a[i + k];
    i += dlen;
    switch (dp) {
      case 3:   Serial.printf("{\"event\":\"data\",\"short\":\"0x%04x\",\"soil_moisture\":%ld}\n", src, (long)val); break;
      case 5:   Serial.printf("{\"event\":\"data\",\"short\":\"0x%04x\",\"temperature\":%.1f}\n", src, val / 10.0); break;
      case 15:  Serial.printf("{\"event\":\"data\",\"short\":\"0x%04x\",\"battery\":%ld}\n", src, (long)val); break;
      case 106: Serial.printf("{\"event\":\"data\",\"short\":\"0x%04x\",\"moisture_warning\":%ld}\n", src, (long)val); break;
      case 109: break;
      default:  Serial.printf("{\"event\":\"data\",\"short\":\"0x%04x\",\"dp%u\":%ld}\n", src, dp, (long)val); break;
    }
  }
}

bool my_apsde_handler(esp_zb_apsde_data_ind_t ind) {
  if (ind.status == 0x00 && ind.cluster_id == 0xEF00) {
#if DEBUG_RAW
    Serial.printf("EF00 raw 0x%04x len %u: ", ind.src_short_addr, ind.asdu_length);
    for (uint32_t i = 0; i < ind.asdu_length; i++) Serial.printf("%02x ", ind.asdu[i]);
    Serial.println();
#endif
    decode_tuya(ind.src_short_addr, ind.asdu, ind.asdu_length);
  }
  return zb_apsde_data_indication_handler(ind);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("{\"event\":\"boot\"}");
  Zigbee.setRebootOpenNetwork(180);
  if (!Zigbee.begin(ZIGBEE_COORDINATOR)) {
    Serial.println("{\"event\":\"error\",\"msg\":\"zigbee begin failed\"}");
    while (true) { delay(1000); }
  }
  esp_zb_aps_data_indication_handler_register(my_apsde_handler);
  Serial.println("{\"event\":\"ready\"}");
}

void loop() {
  poll_devices();
  if (millis() - last_roster > ROSTER_INTERVAL_MS) { last_roster = millis(); emit_roster(); }
  delay(2000);
}