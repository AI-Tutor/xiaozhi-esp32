#include "system_info.h"

#include <freertos/task.h>
#include <esp_log.h>
#include <esp_flash.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_partition.h>
#include <esp_app_desc.h>
#include <esp_ota_ops.h>
#include <nvs.h>
#if CONFIG_IDF_TARGET_ESP32P4
#include "esp_wifi_remote.h"
#endif

#define TAG "SystemInfo"
namespace {
constexpr const char* kUdidNamespace = "device";
constexpr const char* kUdidKey = "udid";
constexpr const char* kWifiNamespace = "wifi";
constexpr int kMaxWifiSsidCount = 10;

void GetStaMac(uint8_t mac[6]) {
#if CONFIG_IDF_TARGET_ESP32P4
    esp_wifi_get_mac(WIFI_IF_STA, mac);
#else
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
#endif
}

std::string FormatMac(const uint8_t mac[6]) {
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return std::string(mac_str);
}

bool ReadUdidFromNvs(std::string* udid) {
    nvs_handle_t nvs_handle;
    if (nvs_open(kUdidNamespace, NVS_READONLY, &nvs_handle) != ESP_OK) {
        return false;
    }
    size_t length = 0;
    if (nvs_get_str(nvs_handle, kUdidKey, nullptr, &length) != ESP_OK || length == 0) {
        nvs_close(nvs_handle);
        return false;
    }
    std::string value;
    value.resize(length);
    if (nvs_get_str(nvs_handle, kUdidKey, value.data(), &length) != ESP_OK) {
        nvs_close(nvs_handle);
        return false;
    }
    nvs_close(nvs_handle);
    while (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    *udid = value;
    return true;
}

void WriteUdidToNvs(const std::string& udid) {
    nvs_handle_t nvs_handle;
    if (nvs_open(kUdidNamespace, NVS_READWRITE, &nvs_handle) != ESP_OK) {
        return;
    }
    if (nvs_set_str(nvs_handle, kUdidKey, udid.c_str()) == ESP_OK) {
        nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
}

bool HasWifiConfigInNvs() {
    nvs_handle_t nvs_handle;
    if (nvs_open(kWifiNamespace, NVS_READONLY, &nvs_handle) != ESP_OK) {
        return false;
    }

    for (int i = 0; i < kMaxWifiSsidCount; i++) {
        std::string ssid_key = "ssid";
        if (i > 0) {
            ssid_key += std::to_string(i);
        }
        size_t length = 0;
        if (nvs_get_str(nvs_handle, ssid_key.c_str(), nullptr, &length) == ESP_OK && length > 0) {
            nvs_close(nvs_handle);
            return true;
        }
    }
    nvs_close(nvs_handle);
    return false;
}
} // namespace

size_t SystemInfo::GetFlashSize() {
    uint32_t flash_size;
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get flash size");
        return 0;
    }
    return (size_t)flash_size;
}

size_t SystemInfo::GetMinimumFreeHeapSize() {
    return esp_get_minimum_free_heap_size();
}

size_t SystemInfo::GetFreeHeapSize() {
    return esp_get_free_heap_size();
}

void SystemInfo::InitializeUdid() {
    std::string udid;
    bool has_udid = ReadUdidFromNvs(&udid);

    uint8_t mac[6];
    GetStaMac(mac);
    std::string mac_str = FormatMac(mac);

    if (has_udid) {
        ESP_LOGI(TAG, "mac_address=%s device_id=%s device_id_source=udid_nvs", mac_str.c_str(), udid.c_str());
        return;
    }

    if (HasWifiConfigInNvs()) {
        WriteUdidToNvs(mac_str);
        if (ReadUdidFromNvs(&udid)) {
            ESP_LOGI(TAG, "mac_address=%s device_id=%s device_id_source=mac", mac_str.c_str(), udid.c_str());
        }
        return;
    }

    {
        uint8_t random_prefix[3];
        uint32_t r = esp_random();
        random_prefix[0] = static_cast<uint8_t>(r & 0xFF);
        random_prefix[1] = static_cast<uint8_t>((r >> 8) & 0xFF);
        random_prefix[2] = static_cast<uint8_t>((r >> 16) & 0xFF);
        mac[0] = random_prefix[0];
        mac[1] = random_prefix[1];
        mac[2] = random_prefix[2];
        WriteUdidToNvs(FormatMac(mac));
    }

    if (ReadUdidFromNvs(&udid)) {
        ESP_LOGI(TAG, "mac_address=%s device_id=%s device_id_source=randomized", mac_str.c_str(), udid.c_str());
    }
}

std::string SystemInfo::GetMacAddress() {
    std::string udid;
    if (ReadUdidFromNvs(&udid)) {
        return udid;
    }
    InitializeUdid();
    if (ReadUdidFromNvs(&udid)) {
        return udid;
    }
    uint8_t mac[6];
    GetStaMac(mac);
    return FormatMac(mac);
}

std::string SystemInfo::GetChipModelName() {
    return std::string(CONFIG_IDF_TARGET);
}

esp_err_t SystemInfo::PrintTaskCpuUsage(TickType_t xTicksToWait) {
    #define ARRAY_SIZE_OFFSET 5
    TaskStatus_t *start_array = NULL, *end_array = NULL;
    UBaseType_t start_array_size, end_array_size;
    configRUN_TIME_COUNTER_TYPE start_run_time, end_run_time;
    esp_err_t ret;
    uint32_t total_elapsed_time;

    //Allocate array to store current task states
    start_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
    start_array = (TaskStatus_t*)malloc(sizeof(TaskStatus_t) * start_array_size);
    if (start_array == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto exit;
    }
    //Get current task states
    start_array_size = uxTaskGetSystemState(start_array, start_array_size, &start_run_time);
    if (start_array_size == 0) {
        ret = ESP_ERR_INVALID_SIZE;
        goto exit;
    }

    vTaskDelay(xTicksToWait);

    //Allocate array to store tasks states post delay
    end_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
    end_array = (TaskStatus_t*)malloc(sizeof(TaskStatus_t) * end_array_size);
    if (end_array == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto exit;
    }
    //Get post delay task states
    end_array_size = uxTaskGetSystemState(end_array, end_array_size, &end_run_time);
    if (end_array_size == 0) {
        ret = ESP_ERR_INVALID_SIZE;
        goto exit;
    }

    //Calculate total_elapsed_time in units of run time stats clock period.
    total_elapsed_time = (end_run_time - start_run_time);
    if (total_elapsed_time == 0) {
        ret = ESP_ERR_INVALID_STATE;
        goto exit;
    }

    printf("| Task | Run Time | Percentage\n");
    //Match each task in start_array to those in the end_array
    for (int i = 0; i < start_array_size; i++) {
        int k = -1;
        for (int j = 0; j < end_array_size; j++) {
            if (start_array[i].xHandle == end_array[j].xHandle) {
                k = j;
                //Mark that task have been matched by overwriting their handles
                start_array[i].xHandle = NULL;
                end_array[j].xHandle = NULL;
                break;
            }
        }
        //Check if matching task found
        if (k >= 0) {
            uint32_t task_elapsed_time = end_array[k].ulRunTimeCounter - start_array[i].ulRunTimeCounter;
            uint32_t percentage_time = (task_elapsed_time * 100UL) / (total_elapsed_time * CONFIG_FREERTOS_NUMBER_OF_CORES);
            printf("| %-16s | %8lu | %4lu%%\n", start_array[i].pcTaskName, task_elapsed_time, percentage_time);
        }
    }

    //Print unmatched tasks
    for (int i = 0; i < start_array_size; i++) {
        if (start_array[i].xHandle != NULL) {
            printf("| %s | Deleted\n", start_array[i].pcTaskName);
        }
    }
    for (int i = 0; i < end_array_size; i++) {
        if (end_array[i].xHandle != NULL) {
            printf("| %s | Created\n", end_array[i].pcTaskName);
        }
    }
    ret = ESP_OK;

exit:    //Common return path
    free(start_array);
    free(end_array);
    return ret;
}

void SystemInfo::PrintTaskList() {
    char buffer[1000];
    vTaskList(buffer);
    ESP_LOGI(TAG, "Task list: \n%s", buffer);
}

void SystemInfo::PrintHeapStats() {
    int free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    int min_free_sram = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "free sram: %u minimal sram: %u", free_sram, min_free_sram);
}
