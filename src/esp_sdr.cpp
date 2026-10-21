// ESP32 receive-only raw I/Q capture driver.
// Hardware register sequence informed by ESPARGOS/esp-sdr.

#include "esp_sdr.h"
#include <Arduino.h>

extern "C" {
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "nvs_flash.h"
#include "soc/soc.h"
#include "soc/dport_reg.h"
#include "soc/dport_access.h"
#include "soc/rtc.h"
#include "heap_memory_layout.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

#include <string.h>

// Hardware dump DMA owns this entire aperture, including its IRAM alias.
SOC_RESERVE_MEMORY_REGION(0x3ffe8000, 0x3fff8000, spectrum_capture);

// ------- Undocumented register addresses (from receiver.c) -------------------
static constexpr uint32_t RX_GAIN     = 0x3ff5c02cu;    // AGC + manual gain
static constexpr uint32_t DUMP_CTRL   = 0x60033d90u;    // hardware capture
static constexpr uint32_t DUMP_STATUS = 0x60033d94u;    // hardware capture
static constexpr uint32_t DUMP_BYTES  = 0x60033dc4u;    // hardware capture

// ------- Undocumented ROM symbols (resolved by Arduino-ESP32's rom.ld) -------
// These live in the ESP32 bootROM and are resolved by the SDK's rom.ld.
extern "C" {
    void rom_pbus_workmode(void);
    void rom_pbus_xpd_rx_on(int);
    void rom_pbus_xpd_tx_off(void);
    void rom_set_rxclk_en(int);
    void set_chanfreq(unsigned mhz, unsigned mode);
    void rom_set_rf_freq_offset(unsigned crystal, unsigned mhz, int offset);
    unsigned ram_chip_i2c_readReg(unsigned block, unsigned host, unsigned reg);
    void ram_chip_i2c_writeReg(unsigned block, unsigned host, unsigned reg, unsigned value);
    unsigned rom_chip_i2c_readReg(unsigned block, unsigned host, unsigned reg);
    void rom_chip_i2c_writeReg(unsigned block, unsigned host, unsigned reg, unsigned value);
}

static bool    g_ready     = false;
static const char* g_lastError = "RX starting";
static int32_t g_errorDetail = 0;
const char* espSdrLastError() { return g_lastError; }
int32_t espSdrErrorDetail() { return g_errorDetail; }
static uint8_t g_gainMax   = 0;
static uint32_t g_frequencyMHz = 2412;
static bool g_directTuned = false;
static bool    g_agcOn     = true;      // mirrors RX_GAIN bit 23 (clear = AGC)
static uint8_t g_manGain   = 0;         // current manual gain value

static void applyReceivePll() {
    // A normal Wi-Fi channel setup pins the PLL capacitor code. Release
    // that override before a direct ROM retune (ESPARGOS ESP32 backend).
    const unsigned cap = ram_chip_i2c_readReg(0x62, 1, 0);
    ram_chip_i2c_writeReg(0x62, 1, 0, cap & ~0x80u);
    const unsigned xtal = rtc_clk_xtal_freq_get();
    const unsigned crystal = xtal == 26 ? 1 : xtal == 24 ? 2 : 0;
    // This ESP32 ROM uses 1/1024 MHz offsets. Whole-MHz target: offset 0.
    rom_set_rf_freq_offset(crystal, g_frequencyMHz, 0);
    esp_rom_delay_us(100);
}

// Reassert the receive path after initialization, retuning, or timeout.
static void prepare_rx() {
    rom_pbus_workmode();
    rom_pbus_xpd_tx_off();
    rom_pbus_xpd_rx_on(1);
    rom_set_rxclk_en(1);
    if (g_directTuned) applyReceivePll();
    // PBUS setup can release forced gain; restore it after preparing RX.
    if (g_ready) {
        if (g_agcOn) REG_CLR_BIT(RX_GAIN, BIT(23));
        else REG_WRITE(RX_GAIN, (REG_READ(RX_GAIN) & 0x007fffffu)
                                | ((uint32_t)g_manGain << 24) | BIT(23));
    }
}

// Guarded ESP_ERROR_CHECK equivalent -- logs failure and returns false
// instead of aborting, so a bad init can be visible on the UI.
static bool okOrLog(const char* what, esp_err_t err) {
    if (err == ESP_OK) return true;
    g_errorDetail = (int32_t)err;
    g_lastError = what;
    Serial.printf("[sdr] %s FAILED rc=%d (%s)\n", what, (int)err, esp_err_to_name(err));
    return false;
}

bool espSdrBegin() {
    Serial.println("[sdr] espSdrBegin: starting");

    // --- NVS: ESP-IDF needs this; Arduino-ESP32 usually init'd it already
    //    but calling again is safe (returns ESP_OK fast-path). On a brand-new
    //    partition we may get NO_FREE_PAGES / NEW_VERSION — erase and retry.
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        Serial.println("[sdr] NVS reinit: erase + retry");
        nvs_flash_erase();
        e = nvs_flash_init();
    }
    if (!okOrLog("nvs_flash_init", e)) return false;

    // --- Default event loop (idempotent — Arduino may have created it)
    e = esp_event_loop_create_default();
    if (e == ESP_ERR_INVALID_STATE) {
        Serial.println("[sdr] event loop already exists (ok)");
    } else if (!okOrLog("esp_event_loop_create_default", e)) {
        return false;
    }

    // --- Wi-Fi stack init in NULL mode + promiscuous (parks radio in RX)
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    // This is a capture-only receiver, not a Wi-Fi access point. The default
    // packet pools compete with the reserved 64 KiB hardware capture bank.
    cfg.static_rx_buf_num = 4;
    cfg.dynamic_rx_buf_num = 8;
    cfg.tx_buf_type = 1;  // Allocate TX packet buffers dynamically, only if used.
    cfg.static_tx_buf_num = 0;
    cfg.dynamic_tx_buf_num = 4;
    cfg.cache_tx_buf_num = 4;
    cfg.ampdu_rx_enable = 0;
    cfg.ampdu_tx_enable = 0;
    cfg.rx_ba_win = 4;
    Serial.printf("[sdr] heap before Wi-Fi init: free=%u largest=%u\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    if (!okOrLog("esp_wifi_init",            esp_wifi_init(&cfg))) return false;
    if (!okOrLog("esp_wifi_set_storage",     esp_wifi_set_storage(WIFI_STORAGE_RAM))) return false;
    if (!okOrLog("esp_wifi_set_mode(NULL)",  esp_wifi_set_mode(WIFI_MODE_NULL))) return false;
    if (!okOrLog("esp_wifi_start",           esp_wifi_start())) return false;
    if (!okOrLog("esp_wifi_set_ps(NONE)",    esp_wifi_set_ps(WIFI_PS_NONE))) return false;
    if (!okOrLog("esp_wifi_set_promiscuous", esp_wifi_set_promiscuous(true))) return false;
    // Establish normal PHY calibration at channel 1 before direct retuning.
    if (!okOrLog("esp_wifi_set_channel(1)", esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE))) return false;

    // --- ROM sequence: power up receive path
    prepare_rx();

    // --- Read gain_max from RX_GAIN register. If this reads 0 or 0xFF the
    //     register layout changed or the chip isn't initialized the way we
    //     think. Expected: a small positive integer (e.g. 72–127).
    g_gainMax = (uint8_t)((REG_READ(RX_GAIN) >> 8) & 127);
    Serial.printf("[sdr] RX_GAIN raw=0x%08lx gain_max=%u\n",
                  (unsigned long)REG_READ(RX_GAIN), g_gainMax);

    // Hardware AGC is the default (bit 23 of RX_GAIN clear means AGC on)
    REG_CLR_BIT(RX_GAIN, (1u << 23));
    g_agcOn   = true;
    g_manGain = g_gainMax;    // sensible default if the UI switches to MAN

    g_ready = true;
    g_lastError = "RX ready";
    g_errorDetail = 0;
    Serial.println("[sdr] espSdrBegin: READY");
    return true;
}

uint8_t espSdrGainMax() { return g_gainMax; }
bool    espSdrReady()   { return g_ready; }

// AGC: bit 23 of RX_GAIN. CLEAR = AGC on (hardware default), SET = manual.
bool espSdrSetAgc(bool on) {
    if (!g_ready) { Serial.println("[sdr] setAgc: not ready"); return false; }
    if (on) {
        REG_CLR_BIT(RX_GAIN, (1u << 23));
    } else {
        REG_SET_BIT(RX_GAIN, (1u << 23));
        // When switching to manual, make sure the gain field reflects the
        // last manual value (don't leave the chip at zero).
        uint32_t v = REG_READ(RX_GAIN) & 0x007fffffu;
        REG_WRITE(RX_GAIN, v | ((uint32_t)g_manGain << 24) | BIT(23));
    }
    g_agcOn = on;
    Serial.printf("[sdr] AGC %s\n", on ? "ON" : "OFF (manual)");
    return true;
}

// Manual gain is the PHY table index in bits 24..30, enabled by bit 23.
bool espSdrSetGain(uint8_t gain) {
    if (!g_ready) { Serial.println("[sdr] setGain: not ready"); return false; }
    if (g_gainMax && gain > g_gainMax) gain = g_gainMax;
    g_manGain = gain;
    if (!g_agcOn) {
        uint32_t v = REG_READ(RX_GAIN) & 0x007fffffu;
        REG_WRITE(RX_GAIN, v | ((uint32_t)gain << 24) | BIT(23));
    }
    return true;
}

bool espSdrSetFrequencyMHz(uint32_t mhz) {
    if (!g_ready) return false;
    // This application only targets the 2.4 GHz receive path.
    if (mhz < 2400 || mhz > 2484) {
        g_lastError = "RX frequency out of range";
        g_errorDetail = mhz;
        return false;
    }
    // Use the PHY entry point: the Wi-Fi API may skip a channel it thinks
    // is already selected after a direct PLL retune.
    REG_CLR_BIT(RX_GAIN, BIT(23));
    set_chanfreq(2412, 0);
    g_frequencyMHz = mhz;
    g_directTuned = true;
    prepare_rx();
    Serial.printf("[sdr] direct receive tune requested: %lu MHz (RF verification required)\n",
                  (unsigned long)mhz);
    return true;
}

// =============================================================================
// Snapshot capture
//
// The fixed 64 KiB capture bank is reserved before heap initialization.
// Linker assertions also prevent application code/data overlapping it.
// =============================================================================

static constexpr unsigned CAPACITY    = 16384u;
static constexpr unsigned MAX_SAMPLES = CAPACITY - 4;
static constexpr uint32_t SENTINEL    = 0xa55aa55au;
static uint32_t* const    g_samples   = (uint32_t*)0x3ffe8000u;

bool espSdrAcquireBurst(uint32_t* out, size_t n, uint8_t clockIdx, uint32_t* elapsedUs) {
    if (!g_ready) { Serial.println("[sdr] acquire: not ready"); return false; }
    if (n < 64 || n > MAX_SAMPLES || clockIdx > 2 || !out) {
        Serial.printf("[sdr] acquire: bad args n=%u clk=%u\n", (unsigned)n, (unsigned)clockIdx);
        return false;
    }

    // Background PHY activity can release RX settings or restore its Wi-Fi
    // channel. Reassert the requested receive LO and gain before sampling.
    if (g_directTuned) prepare_rx();

    REG_WRITE(DUMP_CTRL, 0);
    for (size_t j = 0; j < n; ++j) g_samples[j] = SENTINEL;

    // 16 MS/s needs a narrower analog path to reduce out-of-span aliases.
    // ESPARGOS characterizes code 127 as about 12 MHz total bandwidth on
    // the original ESP32. Preserve calibration and restore after capture.
    unsigned filterSaved[2] = {};
    if (clockIdx == 0) {
        for (unsigned j = 0; j < 2; ++j) {
            filterSaved[j] = rom_chip_i2c_readReg(0x67, 1, 1 + j);
            rom_chip_i2c_writeReg(0x67, 1, 1 + j, (filterSaved[j] & ~127u) | 127u);
        }
        esp_rom_delay_us(10);
    }

    // --- Switch SRAM bank 2 into MAC dump mode 3 (full 64 KB aperture).
    //     Preserve the previous owner + byte-select so Wi-Fi doesn't choke
    //     after we hand them back.
    uint32_t ownerSave = DPORT_REG_READ(DPORT_IRAM_DRAM_AHB_SEL_REG);
    uint32_t byteSave  = REG_READ(DUMP_BYTES);
    DPORT_REG_WRITE(DPORT_IRAM_DRAM_AHB_SEL_REG,
        (ownerSave & ~DPORT_MAC_DUMP_MODE_M) | (3u << DPORT_MAC_DUMP_MODE_S));
    REG_WRITE(DUMP_BYTES, 0x03020100u);         // Four distinct byte lanes

    // --- Program CTRL: count | source=0 | clock bits. BIT(31) = enable.
    //     clockIdx 0 -> 16 MS/s (BIT 16 set); 1 -> 80 MS/s (both clear);
    //     clockIdx 2 -> 40 MS/s (BIT 15 set).
    uint32_t ctrl = (uint32_t)n
                  | (0u << 20)
                  | (((uint32_t)clockIdx == 0 ? 1u : 0u) << 16)
                  | (((uint32_t)clockIdx == 2 ? 1u : 0u) << 15);
    REG_WRITE(DUMP_CTRL, ctrl | BIT(31));

    // --- Kick the capture (BIT 19 is a one-shot trigger).
    int64_t t0 = esp_timer_get_time();
    REG_SET_BIT(DUMP_CTRL, BIT(19));
    REG_CLR_BIT(DUMP_CTRL, BIT(19));

    // --- Spin on done bit (BIT 18). 20 ms upper bound; at 16 MS/s a
    //     512 samples take 32 us at 16 MS/s, plus trigger overhead.
    while (!(REG_READ(DUMP_CTRL) & BIT(18)) && esp_timer_get_time() - t0 < 20000) {}
    uint32_t result = REG_READ(DUMP_CTRL);
    uint32_t status = REG_READ(DUMP_STATUS);
    uint32_t elapsed = (uint32_t)(esp_timer_get_time() - t0);

    // --- Tear down: disable DUMP, hand SRAM mode + byte-lane back to Wi-Fi.
    REG_WRITE(DUMP_CTRL, 0);
    esp_rom_delay_us(10);
    DPORT_REG_WRITE(DPORT_IRAM_DRAM_AHB_SEL_REG, ownerSave);
    REG_WRITE(DUMP_BYTES, byteSave);
    if (clockIdx == 0)
        for (unsigned j = 0; j < 2; ++j)
            rom_chip_i2c_writeReg(0x67, 1, 1 + j, filterSaved[j]);

    if (!(result & BIT(18)) || (status & 0x7fffu) != n) {
        g_lastError = (result & BIT(18)) ? "RX short capture" : "RX timeout";
        g_errorDetail = status & 0x7fffu;
        Serial.printf("[sdr] acquire TIMEOUT result=0x%08lx status=0x%08lx elapsed=%luus\n",
                      (unsigned long)result, (unsigned long)status, (unsigned long)elapsed);
        // Reassert RX and forced gain in case PHY activity released them.
        prepare_rx();
        espSdrSetAgc(g_agcOn);
        return false;
    }

    for (size_t j = 0; j < n; ++j) {
        if (g_samples[j] == SENTINEL) {
            g_lastError = "RX memory unchanged";
            g_errorDetail = (int32_t)j;
            Serial.printf("[sdr] incomplete capture at %u\n", (unsigned)j);
            return false;
        }
    }
    // --- Copy out to the caller's buffer immediately.
    memcpy(out, g_samples, n * sizeof(uint32_t));
    g_lastError = "RX ready";
    g_errorDetail = 0;
    if (elapsedUs) *elapsedUs = elapsed;
    return true;
}
