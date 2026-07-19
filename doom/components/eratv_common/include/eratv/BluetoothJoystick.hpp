#ifndef MAIN_BLUETOOTHJOYSTICK_HPP_
#define MAIN_BLUETOOTHJOYSTICK_HPP_

#include <cstring>
#include <cinttypes>
#include <cstdlib>
#include <cstdio>
#include <array>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"

#if CONFIG_BT_ENABLED
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_bt_defs.h"
#endif

namespace EraTV {

class BluetoothJoystick {
public:
    static constexpr BaseType_t VIDEO_DECODE_CORE = 1; // Keep in sync with VideoPlayer decode core.

    enum class Button : uint8_t {
        DPAD_UP = 0,
        DPAD_DOWN,
        DPAD_LEFT,
        DPAD_RIGHT,
        DPAD_UP_LEFT,
        DPAD_UP_RIGHT,
        DPAD_DOWN_LEFT,
        DPAD_DOWN_RIGHT,
        BTN_A,
        BTN_B,
        BTN_X,
        BTN_Y,
        BTN_L1,
        BTN_L2,
        BTN_R1,
        BTN_R2,
        BTN_PLUS,
        BTN_MINUS,
        BTN_HOME,
        COUNT,
    };

    static constexpr size_t BUTTON_COUNT = static_cast<size_t>(Button::COUNT);

    bool isButtonPressed(Button button) const {
        if (button == Button::COUNT) {
            return false;
        }
        return button_state[static_cast<uint8_t>(button)] != 0;
    }

    uint32_t getButtonMask() const {
        uint32_t mask = 0;
        for (size_t i = 0; i < BUTTON_COUNT; ++i) {
            if (button_state[i] != 0) {
                mask |= (1u << i);
            }
        }
        return mask;
    }

    std::array<bool, BUTTON_COUNT> getButtonStates() const {
        std::array<bool, BUTTON_COUNT> out = {};
        for (size_t i = 0; i < BUTTON_COUNT; ++i) {
            out[i] = button_state[i] != 0;
        }
        return out;
    }

    void clearButtonStates() {
        for (size_t i = 0; i < BUTTON_COUNT; ++i) {
            button_state[i] = 0;
        }
    }

    bool begin() {
        clearButtonStates();
#if CONFIG_BT_ENABLED
        instance = this;

          // Suppress high-frequency info logs to reduce console overhead/latency.
          esp_log_level_set("EraTV::BTJoy", ESP_LOG_WARN);
#if CONFIG_BT_CTRL_PINNED_TO_CORE == VIDEO_DECODE_CORE
    ESP_LOGW(TAG, "BT controller pinned to decode core (%d)", VIDEO_DECODE_CORE);
#endif
#if CONFIG_BT_BLUEDROID_PINNED_TO_CORE == VIDEO_DECODE_CORE
    ESP_LOGW(TAG, "Bluedroid pinned to decode core (%d)", VIDEO_DECODE_CORE);
#endif
    ESP_LOGI(TAG,
         "Core assignment: decode=%d bt_ctrl=%d bluedroid=%d begin_core=%d",
         VIDEO_DECODE_CORE,
         CONFIG_BT_CTRL_PINNED_TO_CORE,
         CONFIG_BT_BLUEDROID_PINNED_TO_CORE,
         xPortGetCoreID());

        esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Failed to release Classic BT memory: %s", esp_err_to_name(err));
            return false;
        }

        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        err = esp_bt_controller_init(&bt_cfg);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "esp_bt_controller_init failed: %s", esp_err_to_name(err));
            return false;
        }

        err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "esp_bt_controller_enable failed: %s", esp_err_to_name(err));
            return false;
        }

        err = esp_bluedroid_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "esp_bluedroid_init failed: %s", esp_err_to_name(err));
            return false;
        }

        err = esp_bluedroid_enable();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "esp_bluedroid_enable failed: %s", esp_err_to_name(err));
            return false;
        }

        configureSecurity();

        esp_ble_gap_register_callback(gapCallbackStatic);
        esp_ble_gattc_register_callback(gattcCallbackStatic);
        esp_ble_gattc_app_register(APP_ID);

        ESP_LOGI(TAG, "Bluetooth joystick listener started (BLE HID)");
        return true;
#else
        ESP_LOGW(TAG, "Bluetooth is disabled. Enable CONFIG_BT_ENABLED in menuconfig to use joystick listener.");
        return false;
#endif
    }

private:
    static_assert(BUTTON_COUNT <= 32, "Button state mask supports up to 32 buttons");

    volatile uint8_t button_state[BUTTON_COUNT] = {};

    void setButtonState(Button button, bool pressed) {
        if (button == Button::COUNT) {
            return;
        }
        button_state[static_cast<uint8_t>(button)] = pressed ? 1u : 0u;
    }

#if CONFIG_BT_ENABLED
    static constexpr uint16_t APP_ID = 0x42;
    static constexpr uint16_t HID_SERVICE_UUID = 0x1812;
    static constexpr uint16_t HID_REPORT_UUID = 0x2A4D;
    static constexpr uint16_t HID_REPORT_MAP_UUID = 0x2A4B;
    static constexpr uint16_t CCCD_UUID = 0x2902;

    inline static BluetoothJoystick *instance = nullptr;

    esp_gatt_if_t gattc_if = ESP_GATT_IF_NONE;
    esp_bd_addr_t peer_bda = {0};
    esp_ble_addr_type_t peer_addr_type = BLE_ADDR_TYPE_PUBLIC;
    uint16_t conn_id = 0;

    bool connecting = false;
    bool connected = false;
    bool scan_params_set = false;
    bool scanning = false;
    bool encrypted = false;
    bool hid_service_found = false;
    bool logged_callback_core = false;

    uint16_t service_start_handle = 0;
    uint16_t service_end_handle = 0;
    static constexpr size_t MAX_HID_SERVICES = 4;
    uint16_t hid_service_starts[MAX_HID_SERVICES] = {0};
    uint16_t hid_service_ends[MAX_HID_SERVICES] = {0};
    size_t hid_service_count = 0;
    uint16_t report_map_handle = 0;
    uint16_t report_char_handle = 0;
    uint16_t last_report_handle = 0;
    uint16_t last_report_len = 0;
    uint8_t last_report[64] = {0};

    // State tracking for 10-byte Android HID state reports.
    // These reports carry current input state, not discrete events.
    struct AndroidHidState {
        uint8_t hat     = 0xFF; // byte[4]: HAT switch, 0xFF = neutral
        uint8_t btns1   = 0x00; // byte[5]: first button bitmask
        uint8_t btns2   = 0x00; // byte[6]: second button bitmask (if any)
        bool valid = false;     // false until first report received
    };
    AndroidHidState android_state;

    // State tracking for 2-byte Android Android HID mode 2 (consumer keycode reports).
    // byte[0] = active HID consumer keycode, 0x00 = all released.
    uint8_t android2_active_key = 0x00;

    static constexpr size_t MAX_SUBSCRIBED_REPORT_HANDLES = 12;
    uint16_t subscribed_report_handles[MAX_SUBSCRIBED_REPORT_HANDLES] = {0};
    uint16_t subscribed_cccd_values[MAX_SUBSCRIBED_REPORT_HANDLES] = {0};
    size_t subscribed_report_count = 0;

    static void gapCallbackStatic(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
        if (instance) {
            instance->gapCallback(event, param);
        }
    }

    static void gattcCallbackStatic(esp_gattc_cb_event_t event,
                                    esp_gatt_if_t gattc_if_param,
                                    esp_ble_gattc_cb_param_t *param) {
        if (instance) {
            instance->gattcCallback(event, gattc_if_param, param);
        }
    }

    void configureSecurity() {
        esp_ble_auth_req_t auth_req = ESP_LE_AUTH_BOND;
        esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
        uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
        uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
        uint8_t key_size = 16;

        esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req));
        esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(iocap));
        esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(init_key));
        esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(rsp_key));
        esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size));
    }

    void configureScanParams() {
        esp_ble_scan_params_t scan_params = {
            .scan_type = BLE_SCAN_TYPE_ACTIVE,
            .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
            .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
            .scan_interval = 0x50,
            .scan_window = 0x30,
            .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
        };
        esp_err_t err = esp_ble_gap_set_scan_params(&scan_params);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ble_gap_set_scan_params failed: %s", esp_err_to_name(err));
        }
    }

    void startScanIfIdle() {
        if (connected || connecting || scanning) {
            return;
        }
        if (!scan_params_set) {
            configureScanParams();
            return;
        }

        esp_err_t err = esp_ble_gap_start_scanning(0); // continuous scan
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Scanning for BLE HID controllers...");
        } else if (err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "esp_ble_gap_start_scanning failed: %s", esp_err_to_name(err));
        }
    }

    static bool hasHidServiceInAdv(const uint8_t *adv_data, uint8_t adv_len) {
        uint8_t *nc_adv = const_cast<uint8_t *>(adv_data); // esp_ble_resolve_adv_data API is not const-correct
        uint8_t uuid_len = 0;
        const uint8_t *uuid16 = esp_ble_resolve_adv_data(
            nc_adv,
            ESP_BLE_AD_TYPE_16SRV_CMPL,
            &uuid_len);
        for (uint8_t i = 0; uuid16 && (i + 1) < uuid_len; i += 2) {
            uint16_t uuid = static_cast<uint16_t>(uuid16[i] | (uuid16[i + 1] << 8));
            if (uuid == HID_SERVICE_UUID) {
                return true;
            }
        }

        uuid16 = esp_ble_resolve_adv_data(nc_adv, ESP_BLE_AD_TYPE_16SRV_PART, &uuid_len);
        for (uint8_t i = 0; uuid16 && (i + 1) < uuid_len; i += 2) {
            uint16_t uuid = static_cast<uint16_t>(uuid16[i] | (uuid16[i + 1] << 8));
            if (uuid == HID_SERVICE_UUID) {
                return true;
            }
        }
        return false;
    }

    static bool nameLooksLikeController(const uint8_t *adv_data) {
        uint8_t *nc_adv = const_cast<uint8_t *>(adv_data); // esp_ble_resolve_adv_data API is not const-correct
        uint8_t name_len = 0;
        const uint8_t *name = esp_ble_resolve_adv_data(nc_adv, ESP_BLE_AD_TYPE_NAME_CMPL, &name_len);
        if (!name) {
            name = esp_ble_resolve_adv_data(nc_adv, ESP_BLE_AD_TYPE_NAME_SHORT, &name_len);
        }
        if (!name || name_len == 0) {
            return false;
        }

        char nbuf[40] = {0};
        uint8_t copy_len = (name_len < sizeof(nbuf) - 1) ? name_len : (sizeof(nbuf) - 1);
        memcpy(nbuf, name, copy_len);
        for (uint8_t i = 0; i < copy_len; ++i) {
            if (nbuf[i] >= 'A' && nbuf[i] <= 'Z') {
                nbuf[i] = static_cast<char>(nbuf[i] - 'A' + 'a');
            }
        }

        return strstr(nbuf, "joy") || strstr(nbuf, "gamepad") || strstr(nbuf, "controller");
    }

    static const char *reportTypeName(uint8_t report_id) {
        switch (report_id & 0x0F) {
        case 0x03:
            return "down";
        case 0x02:
            return "up";
        default:
            return "report";
        }
    }

    static const char *buttonName(Button button) {
        switch (button) {
        case Button::DPAD_UP: return "DPAD_UP";
        case Button::DPAD_DOWN: return "DPAD_DOWN";
        case Button::DPAD_LEFT: return "DPAD_LEFT";
        case Button::DPAD_RIGHT: return "DPAD_RIGHT";
        case Button::DPAD_UP_LEFT: return "DPAD_UP_LEFT";
        case Button::DPAD_UP_RIGHT: return "DPAD_UP_RIGHT";
        case Button::DPAD_DOWN_LEFT: return "DPAD_DOWN_LEFT";
        case Button::DPAD_DOWN_RIGHT: return "DPAD_DOWN_RIGHT";
        case Button::BTN_A: return "BTN_A";
        case Button::BTN_B: return "BTN_B";
        case Button::BTN_X: return "BTN_X";
        case Button::BTN_Y: return "BTN_Y";
        case Button::BTN_L1: return "BTN_L1";
        case Button::BTN_L2: return "BTN_L2";
        case Button::BTN_R1: return "BTN_R1";
        case Button::BTN_R2: return "BTN_R2";
        case Button::BTN_PLUS: return "BTN_PLUS";
        case Button::BTN_MINUS: return "BTN_MINUS";
        case Button::BTN_HOME: return "BTN_HOME";
        default: return nullptr;
        }
    }

    void applyButtonEventByReportId(uint8_t report_id, Button button) {
        if (button == Button::COUNT) {
            return;
        }

        const uint8_t report_type = static_cast<uint8_t>(report_id & 0x0F);
        if (report_type == 0x03) {
            setButtonState(button, true);
        } else if (report_type == 0x02) {
            setButtonState(button, false);
        }
    }

    static void formatButtonDescFromFields(uint8_t sensor, uint8_t aux, uint8_t flag, char *buf, size_t buf_size) {
        const char *name = buttonName(classifyButton(sensor, flag, aux));
        if (name) {
            snprintf(buf, buf_size, "button=%s", name);
        } else {
            snprintf(buf, buf_size, "UNKNOWN(s=%02X,a=%02X,f=%02X)", sensor, aux, flag);
        }
    }

    // byte[3] is an analog sensor reading that jitters ±2 per press; use range checks.
    // byte[len-1] (flag) disambiguates axes. Returns Button::COUNT if unrecognised.
    static Button classifyButton(uint8_t sensor, uint8_t flag, uint8_t aux) {
        if (flag == 0x01) {
            const uint8_t aux_nibble = static_cast<uint8_t>(aux & 0x0F);

            if (sensor >= 0x6F && sensor <= 0x73) return Button::DPAD_LEFT;
            if (sensor >= 0x90 && sensor <= 0x96) return Button::DPAD_RIGHT;
            if (sensor >= 0xA8 && sensor <= 0xAD) return Button::DPAD_UP;

            // Face buttons inferred from captured logs.
            // These are intentionally range-based because sensor values jitter per press.
            // A and X overlap near 0xEE, so use aux nibble to disambiguate:
            // observed A uses aux low nibble 0x4, observed X uses 0x1.
            if (sensor == 0xEE) {
                if (aux_nibble == 0x04) return Button::BTN_A;
                if (aux_nibble == 0x01) return Button::BTN_X;
            }

            // Shoulder buttons seen in logs.
            if (sensor == 0xC7) return Button::BTN_L1;
            if (sensor >= 0x16 && sensor <= 0x18) return Button::BTN_R2;

            // A and L2 share EF/F0 neighborhood, split by aux low nibble.
            if (sensor >= 0xEF && sensor <= 0xF1) {
                if (aux_nibble == 0x04) return Button::BTN_A;
                if (aux_nibble == 0x06) return Button::BTN_L2;
            }

            if (sensor >= 0xD5 && sensor <= 0xD8) return Button::BTN_B;
            if (sensor >= 0xF2 && sensor <= 0xF6) return Button::BTN_Y;
            if (sensor >= 0xEC && sensor <= 0xED) return Button::BTN_X;
        } else if (flag == 0x02) {
            if (sensor >= 0xAC && sensor <= 0xB0) return Button::DPAD_DOWN;
        }
        return Button::COUNT;
    }

    static void formatButtonDesc(const uint8_t *data, uint16_t len, char *buf, size_t buf_size) {
        if (len < 5) {
            snprintf(buf, buf_size, "sensor=SHORT");
            return;
        }
        const uint8_t sensor = data[3];
        const uint8_t aux    = data[2];
        const uint8_t flag   = data[len - 1];
        formatButtonDescFromFields(sensor, aux, flag, buf, buf_size);
    }

    bool isDuplicateReport(uint16_t handle, const uint8_t *data, uint16_t len) const {
        if (handle != last_report_handle || len != last_report_len || len > sizeof(last_report)) {
            return false;
        }
        return memcmp(last_report, data, len) == 0;
    }

    void rememberReport(uint16_t handle, const uint8_t *data, uint16_t len) {
        last_report_handle = handle;
        last_report_len = (len <= sizeof(last_report)) ? len : sizeof(last_report);
        memcpy(last_report, data, last_report_len);
    }

    void gapCallback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
        switch (event) {
        case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
            if (param->scan_param_cmpl.status == ESP_BT_STATUS_SUCCESS) {
                scan_params_set = true;
                ESP_LOGI(TAG, "BLE scan params set");
                startScanIfIdle();
            } else {
                ESP_LOGE(TAG, "Scan param set failed: %d", param->scan_param_cmpl.status);
            }
            break;

        case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
            if (param->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGE(TAG, "Scan start failed: %d", param->scan_start_cmpl.status);
                scanning = false;
            } else {
                scanning = true;
            }
            break;

        case ESP_GAP_BLE_SCAN_RESULT_EVT: {
            if (param->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) {
                break;
            }
            if (connecting || connected) {
                break;
            }

            const uint8_t *adv = param->scan_rst.ble_adv;
            bool hid = hasHidServiceInAdv(adv, param->scan_rst.adv_data_len + param->scan_rst.scan_rsp_len);
            bool name_match = nameLooksLikeController(adv);
            if (!hid && !name_match) {
                break;
            }

            connecting = true;
            memcpy(peer_bda, param->scan_rst.bda, sizeof(peer_bda));
            peer_addr_type = static_cast<esp_ble_addr_type_t>(param->scan_rst.ble_addr_type);

            ESP_LOGI(TAG, "Joystick candidate found, connecting...");
            esp_ble_gap_stop_scanning();
            if (gattc_if != ESP_GATT_IF_NONE) {
                esp_ble_gattc_open(gattc_if, peer_bda, peer_addr_type, true);
            } else {
                ESP_LOGW(TAG, "GATTC interface not ready yet, resuming scan");
                connecting = false;
                startScanIfIdle();
            }
            break;
        }

        case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
            scanning = false;
            if (!connected && !connecting) {
                startScanIfIdle();
            }
            break;

        case ESP_GAP_BLE_SEC_REQ_EVT:
            ESP_LOGI(TAG, "Security request received");
            esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
            break;

        case ESP_GAP_BLE_AUTH_CMPL_EVT:
            encrypted = param->ble_security.auth_cmpl.success;
            ESP_LOGI(TAG, "Authentication complete success=%d reason=0x%x",
                param->ble_security.auth_cmpl.success,
                param->ble_security.auth_cmpl.success ? 0 : param->ble_security.auth_cmpl.fail_reason);
            break;

        case ESP_GAP_BLE_PASSKEY_REQ_EVT:
            ESP_LOGI(TAG, "Passkey requested by controller");
            break;

        case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
            ESP_LOGI(TAG, "Passkey notify: %" PRIu32, param->ble_security.key_notif.passkey);
            break;

        case ESP_GAP_BLE_NC_REQ_EVT:
            ESP_LOGI(TAG, "Numeric comparison request: %" PRIu32, param->ble_security.key_notif.passkey);
            esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, true);
            break;

        default:
            break;
        }
    }

    void gattcCallback(esp_gattc_cb_event_t event,
                       esp_gatt_if_t gattc_if_param,
                       esp_ble_gattc_cb_param_t *param) {
        if (!logged_callback_core) {
            logged_callback_core = true;
            ESP_LOGI(TAG,
                     "Joystick callback core=%d (decode core=%d)",
                     xPortGetCoreID(),
                     VIDEO_DECODE_CORE);
        }

        switch (event) {
        case ESP_GATTC_REG_EVT:
            if (param->reg.status != ESP_GATT_OK) {
                ESP_LOGE(TAG, "GATTC register failed: %d", param->reg.status);
                break;
            }
            gattc_if = gattc_if_param;
            configureScanParams();
            break;

        case ESP_GATTC_OPEN_EVT:
            if (param->open.status != ESP_GATT_OK) {
                ESP_LOGE(TAG, "Open failed, status=%d", param->open.status);
                connecting = false;
                startScanIfIdle();
                break;
            }

            connecting = false;
            connected = true;
            encrypted = false;
            conn_id = param->open.conn_id;
            hid_service_found = false;
            service_start_handle = 0;
            service_end_handle = 0;
            hid_service_count = 0;
            report_map_handle = 0;
            report_char_handle = 0;
            subscribed_report_count = 0;
            clearButtonStates();
            android_state = {};
            android2_active_key = 0x00;

            ESP_LOGI(TAG, "Joystick connected, requesting encryption and MTU...");
            esp_ble_set_encryption(peer_bda, ESP_BLE_SEC_ENCRYPT_NO_MITM);
            if (esp_ble_gattc_send_mtu_req(gattc_if, conn_id) != ESP_OK) {
                ESP_LOGW(TAG, "MTU request failed, searching services immediately");
                esp_ble_gattc_search_service(gattc_if, conn_id, nullptr);
            }
            break;

        case ESP_GATTC_CFG_MTU_EVT:
            ESP_LOGI(TAG, "MTU configured status=%d mtu=%d", param->cfg_mtu.status, param->cfg_mtu.mtu);
            esp_ble_gattc_search_service(gattc_if, param->cfg_mtu.conn_id, nullptr);
            break;

        case ESP_GATTC_SEARCH_RES_EVT:
            if (param->search_res.srvc_id.uuid.len == ESP_UUID_LEN_16 &&
                param->search_res.srvc_id.uuid.uuid.uuid16 == HID_SERVICE_UUID) {
                hid_service_found = true;
                if (hid_service_count < MAX_HID_SERVICES) {
                    hid_service_starts[hid_service_count] = param->search_res.start_handle;
                    hid_service_ends[hid_service_count] = param->search_res.end_handle;
                    ++hid_service_count;
                }
                // Keep legacy fields populated with the first discovered HID service.
                if (service_start_handle == 0 || service_end_handle == 0) {
                    service_start_handle = param->search_res.start_handle;
                    service_end_handle = param->search_res.end_handle;
                }
                ESP_LOGI(TAG, "HID service found handles [%u..%u] (idx=%u)",
                    param->search_res.start_handle,
                    param->search_res.end_handle,
                    static_cast<unsigned>(hid_service_count));
            }
            break;

        case ESP_GATTC_SEARCH_CMPL_EVT:
            if (!hid_service_found) {
                ESP_LOGW(TAG, "Connected device has no HID service");
                esp_ble_gattc_close(gattc_if, conn_id);
                break;
            }
            if (hid_service_count == 0) {
                findAndSubscribeReportChars(service_start_handle, service_end_handle);
            } else {
                for (size_t i = 0; i < hid_service_count; ++i) {
                    findAndSubscribeReportChars(hid_service_starts[i], hid_service_ends[i]);
                }
            }
            break;

        case ESP_GATTC_REG_FOR_NOTIFY_EVT:
            if (param->reg_for_notify.status == ESP_GATT_OK) {
                enableNotificationCccd(param->reg_for_notify.handle);
            } else {
                ESP_LOGE(TAG, "Register for notify failed: %d", param->reg_for_notify.status);
            }
            break;

        case ESP_GATTC_WRITE_DESCR_EVT:
            ESP_LOGI(TAG, "CCCD write status=%d handle=%u", param->write.status, param->write.handle);
            break;

        case ESP_GATTC_READ_CHAR_EVT:
            if (param->read.status == ESP_GATT_OK && param->read.handle == report_map_handle) {
                logReportMap(param->read.value, param->read.value_len);
            }
            break;

        case ESP_GATTC_NOTIFY_EVT:
            handleNotifyReport(param->notify.handle, param->notify.value, param->notify.value_len);
            break;

        case ESP_GATTC_DISCONNECT_EVT:
            ESP_LOGW(TAG, "Joystick disconnected, restarting scan");
            connecting = false;
            connected = false;
            encrypted = false;
            hid_service_found = false;
            hid_service_count = 0;
            scanning = false;
            clearButtonStates();
            android_state = {};
            android2_active_key = 0x00;
            startScanIfIdle();
            break;

        default:
            break;
        }
    }

    void findAndSubscribeReportChars(uint16_t svc_start_handle, uint16_t svc_end_handle) {
        uint16_t count = 0;
        esp_gatt_status_t status = esp_ble_gattc_get_attr_count(
            gattc_if,
            conn_id,
            ESP_GATT_DB_CHARACTERISTIC,
            svc_start_handle,
            svc_end_handle,
            0,
            &count);
        if (status != ESP_GATT_OK || count == 0) {
            ESP_LOGW(TAG, "No characteristics in HID service [%u..%u]", svc_start_handle, svc_end_handle);
            return;
        }

        esp_gattc_char_elem_t *char_elems = static_cast<esp_gattc_char_elem_t *>(
            malloc(sizeof(esp_gattc_char_elem_t) * count));
        if (!char_elems) {
            ESP_LOGE(TAG, "malloc failed for char list");
            return;
        }

        status = esp_ble_gattc_get_all_char(
            gattc_if,
            conn_id,
            svc_start_handle,
            svc_end_handle,
            char_elems,
            &count,
            0);
        if (status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "esp_ble_gattc_get_all_char failed: %d", status);
            free(char_elems);
            return;
        }

        bool subscribed = false;
        for (uint16_t i = 0; i < count; ++i) {
            const esp_gattc_char_elem_t &elem = char_elems[i];
            if (elem.uuid.len != ESP_UUID_LEN_16) {
                continue;
            }

            if (elem.uuid.uuid.uuid16 == HID_REPORT_MAP_UUID) {
                report_map_handle = elem.char_handle;
                ESP_LOGI(TAG, "HID report map handle=%u", report_map_handle);
                esp_ble_gattc_read_char(gattc_if, conn_id, report_map_handle, ESP_GATT_AUTH_REQ_NONE);
                continue;
            }

            if (elem.uuid.uuid.uuid16 != HID_REPORT_UUID) {
                continue;
            }

            ESP_LOGI(TAG, "HID report char handle=%u props=0x%02X", elem.char_handle, elem.properties);
            const bool can_notify = (elem.properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY) != 0;
            const bool can_indicate = (elem.properties & ESP_GATT_CHAR_PROP_BIT_INDICATE) != 0;
            if (can_notify || can_indicate) {
                report_char_handle = elem.char_handle;
                if (subscribed_report_count < MAX_SUBSCRIBED_REPORT_HANDLES) {
                    subscribed_report_handles[subscribed_report_count] = elem.char_handle;
                    subscribed_cccd_values[subscribed_report_count] = can_notify ? 0x0001 : 0x0002;
                    ++subscribed_report_count;
                }
                esp_ble_gattc_register_for_notify(gattc_if, peer_bda, elem.char_handle);
                subscribed = true;
            }
        }

        free(char_elems);

        if (!subscribed) {
            ESP_LOGW(TAG, "No subscribable HID report characteristics found");
        }
    }

    uint16_t cccdValueForHandle(uint16_t char_handle) const {
        for (size_t i = 0; i < subscribed_report_count; ++i) {
            if (subscribed_report_handles[i] == char_handle) {
                return subscribed_cccd_values[i];
            }
        }
        return 0x0001;
    }

    void enableNotificationCccd(uint16_t char_handle) {
        uint16_t count = 1;
        esp_bt_uuid_t descr_uuid = {
            .len = ESP_UUID_LEN_16,
            .uuid = {.uuid16 = CCCD_UUID},
        };

        esp_gattc_descr_elem_t descr_elem;
        esp_ble_gattc_get_descr_by_char_handle(
            gattc_if,
            conn_id,
            char_handle,
            descr_uuid,
            &descr_elem,
            &count);

        if (count == 0) {
            ESP_LOGW(TAG, "CCCD not found for report characteristic");
            return;
        }

        uint16_t notify_en = cccdValueForHandle(char_handle);
        esp_ble_gattc_write_char_descr(
            gattc_if,
            conn_id,
            descr_elem.handle,
            sizeof(notify_en),
            reinterpret_cast<uint8_t *>(&notify_en),
            ESP_GATT_WRITE_TYPE_RSP,
            encrypted ? ESP_GATT_AUTH_REQ_NONE : ESP_GATT_AUTH_REQ_NO_MITM);
    }

    // ---- Android HID (10-byte state report) decoder -------------------------

    // Standard HID HAT switch encoding: 0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW, 0xFF=neutral.
    static Button hatButton(uint8_t hat) {
        switch (hat) {
        case 0x00: return Button::DPAD_UP;
        case 0x01: return Button::DPAD_UP_RIGHT;
        case 0x02: return Button::DPAD_RIGHT;
        case 0x03: return Button::DPAD_DOWN_RIGHT;
        case 0x04: return Button::DPAD_DOWN;
        case 0x05: return Button::DPAD_DOWN_LEFT;
        case 0x06: return Button::DPAD_LEFT;
        case 0x07: return Button::DPAD_UP_LEFT;
        default: return Button::COUNT;
        }
    }

    // Button bit names for Android HID mode 1 (10-byte reports).
    // byte_idx 0 = byte[5], byte_idx 1 = byte[6]
    static Button androidBitButton(uint8_t byte_idx, uint8_t bit) {
        if (byte_idx == 0) {
            switch (bit) {
            case 0: return Button::BTN_A;
            case 1: return Button::BTN_B;
            case 3: return Button::BTN_X;
            case 4: return Button::BTN_Y;
            case 6: return Button::BTN_L1;
            case 7: return Button::BTN_R1;
            default: return Button::COUNT;
            }
        } else if (byte_idx == 1) {
            switch (bit) {
            case 0: return Button::BTN_L2;
            case 1: return Button::BTN_R2;
            case 2: return Button::BTN_MINUS;
            case 3: return Button::BTN_PLUS;
            default: return Button::COUNT;
            }
        }
        return Button::COUNT;
    }

    void decodeAndEmitReport10(uint16_t handle, const uint8_t *data, uint16_t len) {
        if (len < 6) {
            logReport(handle, data, len);
            return;
        }
        const uint8_t hat   = data[4];
        const uint8_t btns1 = data[5];
        const uint8_t btns2 = (len >= 7) ? data[6] : 0;

        if (!android_state.valid) {
            // First report: just record state, no events to diff against yet.
            android_state.hat   = hat;
            android_state.btns1 = btns1;
            android_state.btns2 = btns2;
            android_state.valid = true;
            logReport(handle, data, len);
            return;
        }

        // HAT switch changes.
        if (hat != android_state.hat) {
            if (android_state.hat != 0xFF) {
                const Button btn = hatButton(android_state.hat);
                const char *nm = buttonName(btn);
                if (btn != Button::COUNT && nm) {
                    ESP_LOGI(TAG, "Joystick event handle=%u type=up button=%s", handle, nm);
                    setButtonState(btn, false);
                }
            }
            if (hat != 0xFF) {
                const Button btn = hatButton(hat);
                const char *nm = buttonName(btn);
                if (btn != Button::COUNT && nm) {
                    ESP_LOGI(TAG, "Joystick event handle=%u type=down button=%s", handle, nm);
                    setButtonState(btn, true);
                }
            }
            android_state.hat = hat;
        }

        // Button bitmask byte[5] changes.
        const uint8_t changed1 = btns1 ^ android_state.btns1;
        for (uint8_t bit = 0; bit < 8; ++bit) {
            if (changed1 & (1u << bit)) {
                const bool pressed = (btns1 >> bit) & 1u;
                const Button btn = androidBitButton(0, bit);
                const char *nm = buttonName(btn);
                if (btn != Button::COUNT && nm) {
                    ESP_LOGI(TAG, "Joystick event handle=%u type=%s button=%s",
                        handle, pressed ? "down" : "up", nm);
                    setButtonState(btn, pressed);
                } else {
                    ESP_LOGI(TAG, "Joystick event handle=%u type=%s ANDROID_B1_bit%u",
                        handle, pressed ? "down" : "up", bit);
                }
            }
        }
        android_state.btns1 = btns1;

        // Button bitmask byte[6] changes.
        const uint8_t changed2 = btns2 ^ android_state.btns2;
        for (uint8_t bit = 0; bit < 8; ++bit) {
            if (changed2 & (1u << bit)) {
                const bool pressed = (btns2 >> bit) & 1u;
                const Button btn = androidBitButton(1, bit);
                const char *nm = buttonName(btn);
                if (btn != Button::COUNT && nm) {
                    ESP_LOGI(TAG, "Joystick event handle=%u type=%s button=%s",
                        handle, pressed ? "down" : "up", nm);
                    setButtonState(btn, pressed);
                } else {
                    ESP_LOGI(TAG, "Joystick event handle=%u type=%s ANDROID_B2_bit%u",
                        handle, pressed ? "down" : "up", bit);
                }
            }
        }
        android_state.btns2 = btns2;

        logReport(handle, data, len);
    }

    // ---- Android HID mode 2 (2-byte consumer keycode report) decoder ------

    // Maps HID Consumer page keycodes to button names.
    // The controller sends media control keycodes to represent D-pad directions in mode 2.
    static Button consumerKeyButton(uint8_t key) {
        switch (key) {
        case 0xB6: return Button::DPAD_LEFT;   // Scan Previous Track
        case 0xB5: return Button::DPAD_RIGHT;  // Scan Next Track
        case 0xEA: return Button::DPAD_DOWN;   // Volume Decrement
        case 0xE9: return Button::DPAD_UP;     // Volume Increment
        default:   return Button::COUNT;
        }
    }

    void decodeAndEmitReport2(uint16_t handle, const uint8_t *data) {
        const uint8_t key = data[0];
        if (key == android2_active_key) {
            return; // no change
        }
        // Emit up for previously held key.
        if (android2_active_key != 0x00) {
            const Button btn = consumerKeyButton(android2_active_key);
            const char *nm = buttonName(btn);
            if (btn != Button::COUNT && nm) {
                ESP_LOGI(TAG, "Joystick event handle=%u type=up button=%s", handle, nm);
                setButtonState(btn, false);
            } else {
                ESP_LOGI(TAG, "Joystick event handle=%u type=up CONSUMER_0x%02X", handle, android2_active_key);
            }
        }
        // Emit down for newly pressed key.
        if (key != 0x00) {
            const Button btn = consumerKeyButton(key);
            const char *nm = buttonName(btn);
            if (btn != Button::COUNT && nm) {
                ESP_LOGI(TAG, "Joystick event handle=%u type=down button=%s", handle, nm);
                setButtonState(btn, true);
            } else {
                ESP_LOGI(TAG, "Joystick event handle=%u type=down CONSUMER_0x%02X", handle, key);
            }
        }
        android2_active_key = key;
        logReport(handle, data, 2);
    }

    // ---- End Android HID decoder --------------------------------------------

    static void logReportMap(const uint8_t *data, uint16_t len) {
        char line[256] = {0};
        int off = 0;
        for (uint16_t i = 0; i < len && i < 48; ++i) {
            off += snprintf(&line[off], sizeof(line) - off, "%02X ", data[i]);
            if (off >= static_cast<int>(sizeof(line))) {
                break;
            }
        }
        ESP_LOGI("EraTV::BTJoy", "ReportMap len=%u: %s", len, line);
    }

    void handleNotifyReport(uint16_t handle, const uint8_t *data, uint16_t len) {
        if (!data || len == 0) {
            return;
        }
        // 2-byte reports: Android consumer keycode packets (mode 2).
        if (len == 2) {
            decodeAndEmitReport2(handle, data);
            return;
        }
        // 10-byte reports are standard HID state packets; decode via state-diff, not event-type.
        if (len == 10) {
            decodeAndEmitReport10(handle, data, len);
            return;
        }
        if (isDuplicateReport(handle, data, len)) {
            return;
        }
        rememberReport(handle, data, len);

        const uint8_t report_id = data[0];
        char btn_desc[48];
        formatButtonDesc(data, len, btn_desc, sizeof(btn_desc));
        const Button primary_button = classifyButton(data[3], data[len - 1], data[2]);

        const bool primary_unknown = (strncmp(btn_desc, "UNKNOWN", 7) == 0);

        // Some controllers multiplex a second button event in bytes [4..7]
        // while preserving the primary event in bytes [0..3]. Decode that path too.
        bool has_secondary = false;
        uint8_t sec_report_id = 0;
        char sec_btn_desc[48] = {0};
        Button secondary_button = Button::COUNT;
        if (len >= 8 && data[4] != 0 && data[7] != 0) {
            has_secondary = true;
            sec_report_id = data[4];
            const uint8_t sec_aux = data[6];
            const uint8_t sec_sensor = data[7];
            // Embedded mini-report appears to carry face/shoulder keys (flag domain 0x01).
            formatButtonDescFromFields(sec_sensor, sec_aux, 0x01, sec_btn_desc, sizeof(sec_btn_desc));
            secondary_button = classifyButton(sec_sensor, 0x01, sec_aux);
        }

        const bool secondary_known = has_secondary && (strncmp(sec_btn_desc, "UNKNOWN", 7) != 0);

        if (primary_unknown && secondary_known) {
            const char *event_type = reportTypeName(sec_report_id);
            applyButtonEventByReportId(sec_report_id, secondary_button);
            ESP_LOGI(TAG, "Joystick event handle=%u type=%s %s len=%u",
                handle,
                event_type,
                sec_btn_desc,
                len);
        } else {
            const char *event_type = reportTypeName(report_id);
            applyButtonEventByReportId(report_id, primary_button);
            ESP_LOGI(TAG, "Joystick event handle=%u type=%s %s len=%u",
                handle,
                event_type,
                btn_desc,
                len);
        }

        if (has_secondary && (!primary_unknown || !secondary_known)) {
            ESP_LOGI(TAG, "Joystick subevent handle=%u type=%s %s len=%u",
                handle,
                reportTypeName(sec_report_id),
                sec_btn_desc,
                len);
        }
        logReport(handle, data, len);
    }

    static void logReport(uint16_t handle, const uint8_t *data, uint16_t len) {
        char line[256] = {0};
        int off = 0;
        for (uint16_t i = 0; i < len && i < 32; ++i) {
            off += snprintf(&line[off], sizeof(line) - off, "%02X ", data[i]);
            if (off >= static_cast<int>(sizeof(line))) {
                break;
            }
        }
        ESP_LOGI("EraTV::BTJoy", "Report handle=%u: %s", handle, line);
    }
#endif

    const char *TAG = "EraTV::BTJoy";
};

} // namespace EraTV

#endif /* MAIN_BLUETOOTHJOYSTICK_HPP_ */
