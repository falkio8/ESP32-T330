#pragma once
/**
 * ESPHome External Component: Landis+Gyr T330 / Ultraheat Heat Meter
 * ====================================================================
 * Board:    ESP32S3 (WiFi)
 * UART:     UART2, TX/RX configurable via YAML
 *
 * Protokoll: M-Bus, 4-Sequenz-Handshake
 *   Seq1: Wake-up + Versionsanfrage   (2400 Baud)
 *   Seq2: Application Reset           (2400 Baud)
 *   Seq3: Slave-Auswahl               (2400 Baud)
 *   Seq4: Daten anfordern, zweiphasig lesen:
 *         Phase 1: 2400 Baud (800 ms)  -> Frame 0: stor=0 Aktualdaten
 *         Phase 2: 9600 Baud           -> Frames 1-27: Archive stor=1..25
 *
 * Architektur: FreeRTOS-Task auf Core 0 - ESPHome-Hauptloop (Core 1) nie blockiert.
 *
 * Hintergrund zweiphasige Lektuere:
 *   Der T330 sendet die Aktualdaten (stor=0) bei 2400 Baud, wechselt dann
 *   selbst auf 9600 Baud fuer die Archivdaten. Ein Linux-UART-Treiber puffert
 *   Bytes unabhaengig vom eingestellten Baud (daher funktioniert das Perl-Skript
 *   mit 1500ms Verzoegerung). Die ESP32-UART-Hardware verwirft Bytes mit
 *   Framing-Fehlern sofort - deshalb ist Phase 1 bei 2400 Baud notwendig.
 */

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/network/util.h"
#include "esphome/components/time/real_time_clock.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <cstring>
#include <vector>
#include <cmath>
#include <atomic>

namespace esphome {
namespace t330_meter {

static const char *const TAG = "t330";

#define T330_UART_PORT   UART_NUM_2
#define T330_RX_BUF_SIZE 8192

// ── Messwert-Container ────────────────────────────────────────────────────────
struct T330Data {
    float energy_kwh          = NAN;
    float volume_qm           = NAN;
    float power_w             = NAN;
    float volume_flow_qm_h    = NAN;
    float flow_temp_c         = NAN;
    float return_temp_c       = NAN;
    float temp_diff_k         = NAN;
    float operating_time_h    = NAN;
    float activity_duration_s = NAN;
    std::string fabrication_number;
};

// ─────────────────────────────────────────────────────────────────────────────
class T330Component : public PollingComponent {
 public:
    void set_tx_pin(int p) { tx_pin_ = p; }
    void set_rx_pin(int p) { rx_pin_ = p; }

    void set_energy_kwh_sensor(sensor::Sensor *s)           { energy_kwh_sensor_ = s; }
    void set_volume_qm_sensor(sensor::Sensor *s)            { volume_qm_sensor_ = s; }
    void set_power_w_sensor(sensor::Sensor *s)              { power_w_sensor_ = s; }
    void set_volume_flow_sensor(sensor::Sensor *s)          { volume_flow_sensor_ = s; }
    void set_flow_temp_sensor(sensor::Sensor *s)            { flow_temp_sensor_ = s; }
    void set_return_temp_sensor(sensor::Sensor *s)          { return_temp_sensor_ = s; }
    void set_temp_diff_sensor(sensor::Sensor *s)            { temp_diff_sensor_ = s; }
    void set_operating_time_sensor(sensor::Sensor *s)       { operating_time_sensor_ = s; }
    void set_activity_duration_sensor(sensor::Sensor *s)    { activity_duration_sensor_ = s; }
    void set_fabrication_sensor(text_sensor::TextSensor *s) { fabrication_sensor_ = s; }
    void set_last_read_sensor(text_sensor::TextSensor *s)    { last_read_sensor_ = s; }
    void set_read_status_sensor(text_sensor::TextSensor *s)  { read_status_sensor_ = s; }
    void set_time(time::RealTimeClock *t)                    { time_ = t; }

    float get_setup_priority() const override { return setup_priority::DATA; }

    void dump_config() override {
        ESP_LOGCONFIG(TAG, "T330 Heat Meter:");
        ESP_LOGCONFIG(TAG, "  TX Pin: GPIO%d", tx_pin_);
        ESP_LOGCONFIG(TAG, "  RX Pin: GPIO%d", rx_pin_);
    }

    void setup() override {
        ESP_LOGI(TAG, "Setup UART2: TX=GPIO%d RX=GPIO%d", tx_pin_, rx_pin_);
        trigger_sem_ = xSemaphoreCreateBinary();
        data_mutex_  = xSemaphoreCreateMutex();
        uart_init_(2400);
        xTaskCreatePinnedToCore(meter_task_, "t330_task", 12288, this, 5, &task_handle_, 0);
        ESP_LOGI(TAG, "FreeRTOS task gestartet auf Core 0");
    }

    void update() override {
        ESP_LOGI(TAG, "Triggering read cycle");
        xSemaphoreGive(trigger_sem_);
    }

    void loop() override {
        // Status publizieren (nach jedem Leseversuch, auch bei Fehler)
        if (status_ready_) {
            std::string st;
            if (xSemaphoreTake(data_mutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
                st = pending_status_;
                status_ready_ = false;
                xSemaphoreGive(data_mutex_);
            }
            if (read_status_sensor_ && !st.empty()) {
                read_status_sensor_->publish_state(st);
                ESP_LOGI(TAG, "Lesestatus: %s", st.c_str());
            }
        }

        // Messdaten publizieren (nur bei Erfolg)
        if (!data_ready_) return;
        T330Data d;
        if (xSemaphoreTake(data_mutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
            d = pending_data_;
            data_ready_ = false;
            xSemaphoreGive(data_mutex_);
        } else {
            return;
        }
        publish_(d);
        ESP_LOGI(TAG, "Data published to Home Assistant");
    }

 private:
    int  tx_pin_         = 4;
    int  rx_pin_         = 36;
    bool uart_installed_ = false;

    TaskHandle_t      task_handle_  = nullptr;
    SemaphoreHandle_t trigger_sem_  = nullptr;
    SemaphoreHandle_t data_mutex_   = nullptr;
    std::atomic<bool> data_ready_{false};
    std::atomic<bool> status_ready_{false};
    T330Data          pending_data_;
    std::string       pending_status_;

    sensor::Sensor          *energy_kwh_sensor_        = nullptr;
    sensor::Sensor          *volume_qm_sensor_         = nullptr;
    sensor::Sensor          *power_w_sensor_           = nullptr;
    sensor::Sensor          *volume_flow_sensor_       = nullptr;
    sensor::Sensor          *flow_temp_sensor_         = nullptr;
    sensor::Sensor          *return_temp_sensor_       = nullptr;
    sensor::Sensor          *temp_diff_sensor_         = nullptr;
    sensor::Sensor          *operating_time_sensor_    = nullptr;
    sensor::Sensor          *activity_duration_sensor_ = nullptr;
    text_sensor::TextSensor *fabrication_sensor_       = nullptr;
    text_sensor::TextSensor *last_read_sensor_          = nullptr;
    text_sensor::TextSensor *read_status_sensor_        = nullptr;
    time::RealTimeClock     *time_                      = nullptr;

    static void meter_task_(void *param) {
        static_cast<T330Component *>(param)->task_loop_();
    }

    void task_loop_() {
        for (;;) {
            xSemaphoreTake(trigger_sem_, portMAX_DELAY);
            if (!network::is_connected()) {
                set_status_("FEHLER: Netzwerk nicht verbunden");
                ESP_LOGW(TAG, "Netzwerk nicht verbunden - Abfrage uebersprungen");
                continue;
            }
            ESP_LOGI(TAG, "Task: starting meter read");
            T330Data data;
            if (read_meter_(data)) {
                if (xSemaphoreTake(data_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
                    pending_data_ = data;
                    data_ready_   = true;
                    xSemaphoreGive(data_mutex_);
                }
                set_status_("OK");
                ESP_LOGI(TAG, "Task: read successful");
            } else {
                set_status_("FEHLER: Ablesung fehlgeschlagen - Retry in 2 min");
                ESP_LOGW(TAG, "Task: read failed - retry in 120s");
                vTaskDelay(pdMS_TO_TICKS(120000));
                // Retry
                T330Data data2;
                ESP_LOGI(TAG, "Task: starting retry read");
                if (read_meter_(data2)) {
                    if (xSemaphoreTake(data_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
                        pending_data_ = data2;
                        data_ready_   = true;
                        xSemaphoreGive(data_mutex_);
                    }
                    set_status_("OK (Retry)");
                    ESP_LOGI(TAG, "Task: retry successful");
                } else {
                    set_status_("FEHLER: Ablesung fehlgeschlagen (auch nach Retry)");
                    ESP_LOGW(TAG, "Task: retry also failed");
                }
            }
        }
    }

    void set_status_(const std::string &status) {
        if (xSemaphoreTake(data_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
            pending_status_ = status;
            status_ready_ = true;
            xSemaphoreGive(data_mutex_);
        }
    }

    // ── UART Hilfsfunktionen ──────────────────────────────────────────────────
    void uart_init_(int baud) {
        if (uart_installed_) { uart_driver_delete(T330_UART_PORT); uart_installed_ = false; }
        uart_config_t cfg{};
        cfg.baud_rate  = baud;
        cfg.data_bits  = UART_DATA_8_BITS;
        cfg.parity     = UART_PARITY_EVEN;
        cfg.stop_bits  = UART_STOP_BITS_1;
        cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
        cfg.source_clk = UART_SCLK_APB;
        uart_param_config(T330_UART_PORT, &cfg);
        uart_set_pin(T330_UART_PORT, tx_pin_, rx_pin_, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        uart_driver_install(T330_UART_PORT, T330_RX_BUF_SIZE, 0, 0, nullptr, 0);
        uart_installed_ = true;
    }

    void uart_set_baud_(int baud) { uart_set_baudrate(T330_UART_PORT, baud); }
    void uart_flush_()            { uart_flush_input(T330_UART_PORT); }
    void uart_write_(const uint8_t *data, size_t len) {
        uart_write_bytes(T330_UART_PORT, (const char *)data, len);
    }
    int uart_read_(uint8_t *buf, size_t max_len, int timeout_ms) {
        return uart_read_bytes(T330_UART_PORT, buf, max_len, pdMS_TO_TICKS(timeout_ms));
    }

    std::vector<uint8_t> make_cmd_(const uint8_t *cmd, size_t len, size_t pre = 240) {
        std::vector<uint8_t> buf(pre, 0x00);
        buf.insert(buf.end(), cmd, cmd + len);
        return buf;
    }

    // ── Sequenz 1: Wake-up + Versionsanfrage ─────────────────────────────────
    bool seq1_wakeup_() {
        static const uint8_t C[] = {0x68,0x05,0x05,0x68,0x73,0xFE,0x51,0x0F,0x0F,0xE0,0x16};
        auto buf = make_cmd_(C, sizeof(C));
        uint8_t resp[64];
        for (int i = 1; i <= 10; i++) {
            uart_flush_();
            uart_write_(buf.data(), buf.size());
            int n = uart_read_(resp, sizeof(resp), 1500);
            if (n <= 0) { ESP_LOGI(TAG, "Seq1 #%d/10: keine Antwort (IR-Kopf korrekt aufgesetzt?)", i); continue; }
            if (n >= (int)sizeof(C)) {
                bool echo = true;
                for (size_t k = 0; k < sizeof(C) && echo; k++)
                    if (resp[n - sizeof(C) + k] != C[k]) echo = false;
                if (echo) { ESP_LOGW(TAG, "Seq1 #%d/10: Echo - IR-Kopf falsch platziert!", i); continue; }
            }
            for (int k = 0; k < n - 1; k++) {
                if (resp[k] == 'N' && resp[k+1] == 'b') {
                    ESP_LOGI(TAG, "Seq1 OK - Versionsstring empfangen (%d Bytes)", n);
                    return true;
                }
            }
        }
        ESP_LOGW(TAG, "Seq1 FEHLGESCHLAGEN");
        return false;
    }

    // ── Sequenz 2: Application Reset ─────────────────────────────────────────
    bool seq2_reset_() {
        static const uint8_t C[] = {0x68,0x04,0x04,0x68,0x53,0xFE,0x50,0x00,0xA1,0x16};
        auto buf = make_cmd_(C, sizeof(C));
        uint8_t resp[16];
        for (int i = 1; i <= 10; i++) {
            uart_flush_();
            uart_write_(buf.data(), buf.size());
            int n = uart_read_(resp, sizeof(resp), 1500);
            if (n <= 0) continue;
            for (int k = 0; k < n; k++) {
                if (resp[k] == 0xE5) { ESP_LOGI(TAG, "Seq2 OK - ACK (0xE5) empfangen"); return true; }
            }
        }
        ESP_LOGW(TAG, "Seq2 FEHLGESCHLAGEN - kein ACK");
        return false;
    }

    // ── Sequenz 3: Slave auswaehlen ───────────────────────────────────────────
    bool seq3_select_() {
        static const uint8_t C[] = {0x68,0x07,0x07,0x68,0x73,0xFE,0x51,0x0F,0x70,0x00,0x01,0x42,0x16};
        auto buf = make_cmd_(C, sizeof(C));
        uint8_t resp[64];
        for (int i = 1; i <= 10; i++) {
            uart_flush_();
            uart_write_(buf.data(), buf.size());
            int n = uart_read_(resp, sizeof(resp), 1500);
            if (n <= 0) continue;
            if (n == 11) { ESP_LOGI(TAG, "Seq3 OK - Bestaetigung (11 Bytes)"); return true; }
            for (int k = 0; k <= n - 7; k++) {
                if (resp[k]==0x68 && resp[k+1]==0x05 && resp[k+2]==0x05 &&
                    resp[k+3]==0x68 && resp[k+4]==0x08 && resp[k+6]==0x72) {
                    ESP_LOGI(TAG, "Seq3 OK - Frame erkannt (%d Bytes)", n);
                    return true;
                }
            }
        }
        ESP_LOGW(TAG, "Seq3 FEHLGESCHLAGEN");
        return false;
    }

    // ── Sequenz 4: Daten anfordern, zweiphasig lesen ─────────────────────────
    bool seq4_read_data_(std::vector<uint8_t> &out) {
        static const uint8_t C[] = {0x10,0x7C,0xFE,0x7A,0x16};
        auto buf = make_cmd_(C, sizeof(C));

        uart_flush_();
        uart_write_(buf.data(), buf.size());

        // Phase 1: 2400 Baud, 800 ms - Frame 0 mit stor=0 Aktualdaten empfangen
        {
            uint8_t p1[256];
            int n = uart_read_(p1, sizeof(p1), 800);
            if (n > 0) {
                ESP_LOGI(TAG, "Seq4 - Phase 1: %d Bytes @ 2400 Baud", n);
                out.insert(out.end(), p1, p1 + n);
            }
        }

        // Phase 2: 9600 Baud - Archiv-Frames (stor=1..25) empfangen
        // Nur Baud-Register umschalten - kein voller UART-Reset!
        // Der Meter streamt bereits, uart_init_() wuerde Bytes verlieren.
        uart_set_baud_(9600);
        {
            uint8_t chunk[512];
            int retries = 4, p2 = 0;
            do {
                int n = uart_read_(chunk, sizeof(chunk), 2000);
                if (n > 0) { out.insert(out.end(), chunk, chunk + n); p2 += n; }
                else retries--;
            } while (retries > 0);
            ESP_LOGI(TAG, "Seq4 - Phase 2: %d Bytes @ 9600 Baud", p2);
        }

        // Zurueck auf 2400 Baud
        uart_set_baud_(2400);
        int total = (int)out.size();
        if (total > 0) {
            ESP_LOGI(TAG, "Seq4 OK - %d Bytes gesamt empfangen", total);
            return true;
        }
        ESP_LOGW(TAG, "Seq4 FEHLGESCHLAGEN - keine Nutzdaten empfangen");
        return false;
    }

    // ── Haupt-Lesefunktion ────────────────────────────────────────────────────
    bool read_meter_(T330Data &out) {
        uart_set_baud_(2400);
        uart_flush_();
        ESP_LOGI(TAG, "── Starte M-Bus Kommunikation (4 Sequenzen) ──");
        if (!seq1_wakeup_()) { ESP_LOGW(TAG, "Abbruch nach Seq1"); return false; }
        if (!seq2_reset_())  { ESP_LOGW(TAG, "Abbruch nach Seq2"); return false; }
        if (!seq3_select_()) { ESP_LOGW(TAG, "Abbruch nach Seq3"); return false; }
        std::vector<uint8_t> raw;
        if (!seq4_read_data_(raw)) { ESP_LOGW(TAG, "Abbruch nach Seq4"); return false; }
        ESP_LOGI(TAG, "Alle Sequenzen erfolgreich - starte Parser");
        bool ok = parse_mbus_(raw, out);
        if (!ok) ESP_LOGW(TAG, "Parser: Keine Messwerte mit stor=0/tar=0/su=0 gefunden");
        return ok;
    }

    // ── M-Bus Frame-Parser ────────────────────────────────────────────────────
    bool parse_mbus_(const std::vector<uint8_t> &raw, T330Data &out) {
        size_t pos = 0; bool any = false; int cs_errors = 0;
        while (pos < raw.size()) {
            uint8_t ft = raw[pos++];
            if (ft == 0xE5) continue;
            if (ft == 0x10) { if (pos + 4 <= raw.size()) pos += 4; else break; continue; }
            if (ft != 0x68) continue;
            if (pos + 3 > raw.size()) break;
            uint8_t l1 = raw[pos], l2 = raw[pos+1]; pos += 3;
            if (l1 != l2 || pos + l1 + 2 > raw.size()) { pos += l1 + 2; continue; }
            uint8_t cs = 0;
            for (size_t i = pos; i < pos + l1; i++) cs += raw[i];
            if (raw[pos+l1] != cs || raw[pos+l1+1] != 0x16) { cs_errors++; pos += l1 + 2; continue; }
            const uint8_t *blk = &raw[pos]; pos += l1 + 2;
            if ((blk[0] & 0x0F) == 0x08)
                if (decode_rsp_ud_(blk, l1, out)) any = true;
        }
        if (cs_errors) ESP_LOGW(TAG, "Parser: %d CS-Fehler", cs_errors);
        return any;
    }

    bool decode_rsp_ud_(const uint8_t *b, size_t len, T330Data &out) {
        if (len < 15) return false;
        if (b[2] != 0x72 && b[2] != 0x76) return false;
        return decode_data_blocks_(&b[15], len - 15, out);
    }

    // ── DIF/VIF Datenblock-Decoder ────────────────────────────────────────────
    // Filter: stor=0, tar=0, su=0  (aequivalent zu Perl MQTT-Topic /storage/0/0/0/)
    bool decode_data_blocks_(const uint8_t *data, size_t len, T330Data &out) {
        size_t pos = 0;
        while (pos < len) {
            uint8_t dif  = data[pos++];
            bool dif_ext = (dif >> 7) & 1;
            uint8_t  df  =  dif & 0x0F;
            uint32_t stor = (dif >> 6) & 1;
            uint32_t tar = 0, su = 0; int dc = 0;

            while (dif_ext && dc < 10) {
                if (pos >= len) break;
                uint8_t de = data[pos++];
                dif_ext = (de >> 7) & 1;
                su   += ((de >> 6) & 1) << dc;
                tar  += ((de >> 4) & 3) << (dc * 2);
                stor +=  (de & 0x0F) << (1 + dc * 4);
                dc++;
            }

            if (df == 0x0F) {
                if (dif == 0x0F || dif == 0x1F) break;
                continue;  // 0x2F = Idle-Filler
            }

            if (pos >= len) break;
            uint8_t vr = data[pos++]; bool ve = (vr >> 7) & 1; uint8_t v = vr & 0x7F;
            std::string unit; double sc = 1.0; int vt = 0;
            decode_vif_(v, unit, sc, vt);

            while (ve) { if (pos >= len) break; ve = (data[pos++] >> 7) & 1; }

            if (vt == 1) {
                if (pos >= len) break;
                uint8_t tl = data[pos++];
                if (pos + tl > len) break;
                unit = std::string((const char *)&data[pos], tl); pos += tl; sc = 1.0;
            }

            double reading = 0; bool has = false;
            switch (df) {
                case 0:  break;
                case 1:  if(pos>=len)   goto done; reading=data[pos++]; has=true; break;
                case 2:  if(pos+2>len)  goto done; reading=data[pos]|(data[pos+1]<<8); pos+=2; has=true; break;
                case 3:  if(pos+3>len)  goto done; reading=data[pos]|(data[pos+1]<<8)|(data[pos+2]<<16); pos+=3; has=true; break;
                case 4:  if(pos+4>len)  goto done;
                    reading=(uint32_t)(data[pos]|(data[pos+1]<<8)|(data[pos+2]<<16)|(data[pos+3]<<24));
                    pos+=4; has=true; break;
                case 5:  { if(pos+4>len) goto done; float fv; memcpy(&fv,&data[pos],4); reading=fv; pos+=4; has=true; break; }
                case 6:  if(pos+6>len)  goto done; reading=0; for(int i=0;i<6;i++) reading+=data[pos+i]*pow(256.0,i); pos+=6; has=true; break;
                case 7:  if(pos+8>len)  goto done; reading=0; for(int i=0;i<8;i++) reading+=data[pos+i]*pow(256.0,i); pos+=8; has=true; break;
                case 9:  if(pos>=len)   goto done; reading=bcd_(data+pos,1); pos+=1; has=true; break;
                case 10: if(pos+2>len)  goto done; reading=bcd_(data+pos,2); pos+=2; has=true; break;
                case 11: if(pos+3>len)  goto done; reading=bcd_(data+pos,3); pos+=3; has=true; break;
                case 12: if(pos+4>len)  goto done; reading=bcd_(data+pos,4); pos+=4; has=true; break;
                case 14: if(pos+6>len)  goto done; reading=bcd_(data+pos,6); pos+=6; has=true; break;
                default: pos++; break;
            }

            if (has && vt == 0 && stor == 0 && tar == 0 && su == 0) {
                double sv = (sc != 0.0) ? reading * sc : reading;
                assign_(unit, sv, out);
            }
            continue;
done:       break;
        }
        return !std::isnan(out.energy_kwh)          || !std::isnan(out.volume_qm)        ||
               !std::isnan(out.power_w)             || !std::isnan(out.volume_flow_qm_h) ||
               !std::isnan(out.flow_temp_c)         || !std::isnan(out.return_temp_c)    ||
               !std::isnan(out.temp_diff_k)         || !std::isnan(out.operating_time_h) ||
               !std::isnan(out.activity_duration_s) || !out.fabrication_number.empty();
    }

    // ── VIF-Tabelle (EN 13757-3) ──────────────────────────────────────────────
    void decode_vif_(uint8_t v, std::string &u, double &s, int &t) {
        t=0; s=1.0; u="unknown";
        if(v==0x7C){u="ascii";t=1;return;}
        if(v==0x7B||v==0x7D){t=2;return;} if(v==0x7E){t=3;return;} if(v==0x7F){t=4;return;}
        if(v<=0x07){u="energy_kwh";       s=pow(10,(int)(v&7)-6); return;}
        if(v<=0x0F){u="energy_j";         s=pow(10,(int)(v&7));   return;}
        if(v<=0x17){u="volume_qm";        s=pow(10,(int)(v&7)-6); return;}
        if(v<=0x1F){u="mass_kg";          s=pow(10,(int)(v&7)-3); return;}
        if(v>=0x20&&v<=0x23){static const char*a[]={"on_time_sec","on_time_min","on_time_hours","on_time_days"};u=a[v&3];return;}
        if(v>=0x24&&v<=0x27){static const char*a[]={"operating_time_sec","operating_time_min","operating_time_hours","operating_time_days"};u=a[v&3];return;}
        if(v<=0x2F){u="power_w";          s=pow(10,(int)(v&7)-3); return;}
        if(v<=0x37){u="power_j_h";        s=pow(10,(int)(v&7));   return;}
        if(v<=0x3F){u="volume_flow_qm_h"; s=pow(10,(int)(v&7)-6); return;}
        if(v<=0x47){u="volume_flow_qm_min";s=pow(10,(int)(v&7)-7);return;}
        if(v<=0x4F){u="volume_flow_qm_s"; s=pow(10,(int)(v&7)-9); return;}
        if(v<=0x57){u="mass_flow_kgh";    s=pow(10,(int)(v&7)-3); return;}
        if(v<=0x5B){u="flow_temp_c";      s=pow(10,(int)(v&3)-3); return;}
        if(v<=0x5F){u="return_temp_c";    s=pow(10,(int)(v&3)-3); return;}
        if(v<=0x63){u="temp_diff_k";      s=pow(10,(int)(v&3)-3); return;}
        if(v<=0x67){u="ext_temp_c";       s=pow(10,(int)(v&3)-3); return;}
        if(v<=0x6B){u="pressure_bar";     s=pow(10,(int)(v&3)-3); return;}
        if(v==0x6C){u="date";s=0;return;} if(v==0x6D){u="date_time";s=0;return;}
        if(v==0x6E){u="units_hca";s=0;return;}
        if(v>=0x70&&v<=0x73){static const char*a[]={"avg_duration_sec","avg_duration_min","avg_duration_hours","avg_duration_days"};u=a[v&3];return;}
        if(v>=0x74&&v<=0x77){static const char*a[]={"activity_duration_sec","activity_duration_min","activity_duration_hours","activity_duration_days"};u=a[v&3];return;}
        if(v==0x78){u="fabrication_number";s=0;return;}
        if(v==0x79){u="enhanced";s=0;return;}
        if(v==0x7A){u="bus_address";s=0;return;}
    }

    double bcd_(const uint8_t *b, int n) {
        double r=0, m=1;
        for(int i=0;i<n;i++){r+=(b[i]&0x0F)*m;m*=10;r+=((b[i]>>4)&0x0F)*m;m*=10;}
        return r;
    }

    // ── Sensorzuweisung ───────────────────────────────────────────────────────
    bool assign_(const std::string &unit, double sv, T330Data &out) {
        if (unit == "energy_kwh") {
            if (std::isnan(out.energy_kwh)) { out.energy_kwh = (float)sv; ESP_LOGI(TAG, "  energy_kwh            = %.3f kWh", sv); return true; }
        } else if (unit == "volume_qm") {
            if (std::isnan(out.volume_qm)) { out.volume_qm = (float)sv; ESP_LOGI(TAG, "  volume_qm             = %.4f m3", sv); return true; }
        } else if (unit == "power_w") {
            if (std::isnan(out.power_w)) { out.power_w = (float)sv; ESP_LOGI(TAG, "  power_w               = %.1f W", sv); return true; }
        } else if (unit == "volume_flow_qm_h") {
            if (std::isnan(out.volume_flow_qm_h)) { out.volume_flow_qm_h = (float)sv; ESP_LOGI(TAG, "  volume_flow_qm_h      = %.4f m3/h", sv); return true; }
        } else if (unit == "flow_temp_c") {
            if (std::isnan(out.flow_temp_c)) { out.flow_temp_c = (float)sv; ESP_LOGI(TAG, "  flow_temp_c           = %.2f C", sv); return true; }
        } else if (unit == "return_temp_c") {
            if (std::isnan(out.return_temp_c)) { out.return_temp_c = (float)sv; ESP_LOGI(TAG, "  return_temp_c         = %.2f C", sv); return true; }
        } else if (unit == "temp_diff_k") {
            if (std::isnan(out.temp_diff_k)) { out.temp_diff_k = (float)sv; ESP_LOGI(TAG, "  temp_diff_k           = %.2f K", sv); return true; }
        } else if (unit == "operating_time_hours") {
            if (std::isnan(out.operating_time_h)) { out.operating_time_h = (float)sv; ESP_LOGI(TAG, "  operating_time_hours  = %.0f h", sv); return true; }
        } else if (unit == "operating_time_sec") {
            if (std::isnan(out.operating_time_h)) { out.operating_time_h = (float)(sv/3600.0); ESP_LOGI(TAG, "  operating_time_sec    = %.0f s -> %.2f h", sv, sv/3600.0); return true; }
        } else if (unit == "operating_time_min") {
            if (std::isnan(out.operating_time_h)) { out.operating_time_h = (float)(sv/60.0); ESP_LOGI(TAG, "  operating_time_min    = %.0f min -> %.2f h", sv, sv/60.0); return true; }
        } else if (unit == "operating_time_days") {
            if (std::isnan(out.operating_time_h)) { out.operating_time_h = (float)(sv*24.0); ESP_LOGI(TAG, "  operating_time_days   = %.0f d -> %.0f h", sv, sv*24.0); return true; }
        } else if (unit == "activity_duration_sec") {
            if (std::isnan(out.activity_duration_s)) { out.activity_duration_s = (float)sv; ESP_LOGI(TAG, "  activity_duration_sec = %.0f s", sv); return true; }
        } else if (unit == "fabrication_number") {
            if (out.fabrication_number.empty()) {
                char buf[32]; snprintf(buf, sizeof(buf), "%.0f", sv);
                out.fabrication_number = buf;
                ESP_LOGI(TAG, "  fabrication_number    = %s", buf);
                return true;
            }
        }
        return false;
    }

    // ── Werte an Home Assistant publizieren ───────────────────────────────────
    void publish_(const T330Data &d) {
        ESP_LOGI(TAG, "══════════════════════════════════════════");
        ESP_LOGI(TAG, "  Publizierte Werte (stor=0/tar=0):");
        if(!std::isnan(d.energy_kwh))         ESP_LOGI(TAG, "  Waermeenergie       : %.4f kWh",  d.energy_kwh);
        else                                   ESP_LOGW(TAG, "  Waermeenergie       : NICHT GEFUNDEN");
        if(!std::isnan(d.volume_qm))           ESP_LOGI(TAG, "  Wasservolumen       : %.4f m3",   d.volume_qm);
        else                                   ESP_LOGW(TAG, "  Wasservolumen       : NICHT GEFUNDEN");
        if(!std::isnan(d.power_w))             ESP_LOGI(TAG, "  Heizleistung        : %.1f W",    d.power_w);
        else                                   ESP_LOGW(TAG, "  Heizleistung        : NICHT GEFUNDEN");
        if(!std::isnan(d.volume_flow_qm_h))    ESP_LOGI(TAG, "  Volumenstrom        : %.4f m3/h", d.volume_flow_qm_h);
        else                                   ESP_LOGW(TAG, "  Volumenstrom        : NICHT GEFUNDEN");
        if(!std::isnan(d.flow_temp_c))         ESP_LOGI(TAG, "  Vorlauftemperatur   : %.4f C",    d.flow_temp_c);
        else                                   ESP_LOGW(TAG, "  Vorlauftemperatur   : NICHT GEFUNDEN");
        if(!std::isnan(d.return_temp_c))       ESP_LOGI(TAG, "  Ruecklauftemperatur : %.4f C",    d.return_temp_c);
        else                                   ESP_LOGW(TAG, "  Ruecklauftemperatur : NICHT GEFUNDEN");
        if(!std::isnan(d.temp_diff_k))         ESP_LOGI(TAG, "  Temperaturspreizung : %.4f K",    d.temp_diff_k);
        if(!std::isnan(d.operating_time_h))    ESP_LOGI(TAG, "  Betriebszeit        : %.0f h",    d.operating_time_h);
        if(!std::isnan(d.activity_duration_s)) ESP_LOGI(TAG, "  Aktivitaetsdauer    : %.0f s",    d.activity_duration_s);
        if(!d.fabrication_number.empty())      ESP_LOGI(TAG, "  Fabriknummer        : %s",        d.fabrication_number.c_str());
        ESP_LOGI(TAG, "══════════════════════════════════════════");

        if(energy_kwh_sensor_        &&!std::isnan(d.energy_kwh))          energy_kwh_sensor_->publish_state(d.energy_kwh);
        if(volume_qm_sensor_         &&!std::isnan(d.volume_qm))           volume_qm_sensor_->publish_state(d.volume_qm);
        if(power_w_sensor_           &&!std::isnan(d.power_w))             power_w_sensor_->publish_state(d.power_w);
        if(volume_flow_sensor_       &&!std::isnan(d.volume_flow_qm_h))    volume_flow_sensor_->publish_state(d.volume_flow_qm_h);
        if(flow_temp_sensor_         &&!std::isnan(d.flow_temp_c))         flow_temp_sensor_->publish_state(d.flow_temp_c);
        if(return_temp_sensor_       &&!std::isnan(d.return_temp_c))       return_temp_sensor_->publish_state(d.return_temp_c);
        if(temp_diff_sensor_         &&!std::isnan(d.temp_diff_k))         temp_diff_sensor_->publish_state(d.temp_diff_k);
        if(operating_time_sensor_    &&!std::isnan(d.operating_time_h))    operating_time_sensor_->publish_state(d.operating_time_h);
        if(activity_duration_sensor_ &&!std::isnan(d.activity_duration_s)) activity_duration_sensor_->publish_state(d.activity_duration_s);
        if(fabrication_sensor_       &&!d.fabrication_number.empty())      fabrication_sensor_->publish_state(d.fabrication_number);

        // Zeitstempel der letzten erfolgreichen Ablesung
        if (last_read_sensor_ && time_) {
            auto now = time_->now();
            if (now.is_valid()) {
                char buf[25];
                snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d",
                         now.year, now.month, now.day_of_month,
                         now.hour, now.minute, now.second);
                last_read_sensor_->publish_state(buf);
                ESP_LOGI(TAG, "  Letzte Ablesung     : %s", buf);
            }
        }
    }
};

}  // namespace t330_meter
}  // namespace esphome
