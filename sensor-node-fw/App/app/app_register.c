/* App层：设备寄存器地址映射；STM32F103 / QV，不直接访问外设或其他AO的测量状态。 */
#include "app_register.h"
#include "report_period.h"
#include "store.h"
#include <string.h>
#include <limits.h>

/** @brief 正常数值必须能以协议16位编码表达，越界不可截断成看似有效的数据。 */
static SvcRegisterStatus signedValue(int32_t input, uint16_t *value) {
    if (input < INT16_MIN || input > INT16_MAX) { return SVC_REG_XX; }
    *value = (uint16_t)(int16_t)input;
    return SVC_REG_OK;
}

/** @brief 从唯一版本字符串提取主/次版本，补丁版本不进入主×100＋次编码。 */
static SvcRegisterStatus versionValue(uint16_t *value) {
    char const *p = APP_CFG_FW_VERSION;
    uint32_t parts[2] = {0U, 0U};
    unsigned i;
    for (i = 0U; i < 2U; ++i) {
        unsigned count = 0U;
        while (*p >= '0' && *p <= '9') {
            parts[i] = parts[i] * 10U + (unsigned)(*p++ - '0');
            if (++count > 3U) { return SVC_REG_XX; }
        }
        if (count == 0U || (i == 0U && *p++ != '.')) { return SVC_REG_XX; }
    }
    if ((*p != '\0' && *p != '.') || parts[1] > 99U || parts[0] * 100U + parts[1] > UINT16_MAX) {
        return SVC_REG_XX;
    }
    *value = (uint16_t)(parts[0] * 100U + parts[1]);
    return SVC_REG_OK;
}

/** @brief 只读取调用AO的完整批次快照和配置影子，故障不返回旧测量。 */
SvcRegisterStatus App_Register_Read(void *context, uint8_t address, uint16_t *value) {
    AppRegisterSnapshot const *s = (AppRegisterSnapshot const *)context;
    if (value == NULL) { return SVC_REG_XX; }
    if (address >= 0x10U && address <= 0x17U) {
        if (s == NULL) { return SVC_REG_XX; }
        if (address == 0x14U) {
            if (!s->battery_valid) { return SVC_REG_XX; }
            *value = s->battery_mV; return SVC_REG_OK;
        }
        if (!s->valid) { return SVC_REG_XX; }
        if (address <= 0x12U) {
            int32_t angle = s->angle_cdeg[address - 0x10U];
            if (angle < -18000 || angle > 18000) { return SVC_REG_XX; }
            return signedValue(angle, value);
        }
        if (address == 0x13U) { return signedValue(s->temperature_cC, value); }
        return signedValue(s->acc_mg[address - 0x15U], value);
    }
    if (address == 0x20U) { return versionValue(value); }
    if (address >= 0x01U && address <= 0x03U) {
        if (!App_Store_ConfigResolved()) { return SVC_REG_XX; }
        *value = address == 1U ? (uint16_t)App_ReportPeriod_Minutes() :
                 address == 2U ? APP_CFG_SCL3300_MODE : APP_CFG_REGISTER_REPORT_MASK;
        return SVC_REG_OK;
    }
    if ((address >= 0x40U && address <= 0x4BU) || (address >= 0x50U && address <= 0x55U)) {
        char const *text;
        size_t length, index;
        if (!App_Store_ConfigResolved()) { return SVC_REG_XX; }
        if (address <= 0x4BU) {
            text = App_Store_Value(SVC_PARAM_BLE_SN); length = strlen(text);
            if (length == 0U || length >= SVC_PARAM_BLE_SN_CAP) { return SVC_REG_XX; }
            index = (size_t)(address - 0x40U) * 2U;
        } else {
            size_t i, prefix = sizeof(APP_CFG_REGISTER_UUID_PREFIX) - 1U;
            text = App_Store_Value(SVC_PARAM_BLE_UUID);
            if (strlen(text) != prefix + 12U || strncmp(text, APP_CFG_REGISTER_UUID_PREFIX, prefix) != 0) {
                return SVC_REG_XX;
            }
            text += prefix; length = 12U; index = (size_t)(address - 0x50U) * 2U;
            for (i = 0U; i < length; ++i) {
                char c = text[i];
                if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) {
                    return SVC_REG_XX;
                }
            }
        }
        *value = (uint16_t)((index < length ? (uint32_t)(uint8_t)text[index] << 8U : 0U) |
                            (index + 1U < length ? (uint8_t)text[index + 1U] : 0U));
        return SVC_REG_OK;
    }
    return SVC_REG_XY;
}

/** @brief 周期复用Store异步保存；未接入运行控制的配置拒绝写入。 */
SvcRegisterStatus App_Register_Write(void *context, uint8_t address, uint16_t value) {
    AppStoreInfo info;
    (void)context;
    if (address >= 1U && address <= 3U) {
        if ((address == 1U && (value < APP_CFG_MODEM_TRANS_INTERVAL_MIN || value > APP_CFG_MODEM_TRANS_INTERVAL_MAX)) ||
            (address == 2U && (value < 1U || value > 4U)) ||
            (address == 3U && (value == 0U || (value & 0xFFF0U) != 0U))) { return SVC_REG_OV; }
        if (address != 1U) { return SVC_REG_NP; }
        App_Store_Info(&info);
        if (!info.resolved || !info.present || APP_CFG_STORE_ENABLE == 0U) { return SVC_REG_SE; }
        return App_ReportPeriod_SetMinutes(value) == SVC_OK ? SVC_REG_OK : SVC_REG_SE;
    }
    if ((address >= 0x10U && address <= 0x17U) || address == 0x20U ||
        (address >= 0x40U && address <= 0x4BU) || (address >= 0x50U && address <= 0x55U)) {
        return SVC_REG_RO;
    }
    return SVC_REG_XY;
}
