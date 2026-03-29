/*
 * @Author: Detach0-0 detach0-0@outlook.com
 * @Date: 2026-01-03 01:19:27
 * @LastEditors: Detach0-0 detach0-0@outlook.com
 * @LastEditTime: 2026-01-07 13:30:34
 * @FilePath: \EGoTouchRev-vsc\HimaxChipCore\header\HimaxChip.h
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#pragma once
#include "Device.h"
#include "HimaxProtocol.h"
#include "HimaxRegisters.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <array>
#include <string>
#include <thread>
#include <winnt.h>

// Note: The legacy HIMAX_LOG is removed. We use LOG_INFO, LOG_ERROR, etc. from Logger.h

namespace Himax {

    enum class ConnectionState {
        Unconnected = 0,    // 未连接/句柄未打开
        Connected = 1,      // 通信就绪
        Error = 2           // 通信断开错误
    };

    enum class THP_INSPECTION_ENUM {
        HX_RAWDATA = 0,
        HX_ACT_IDLE_RAWDATA = 1,
        HX_LP_IDLE_RAWDATA = 2,
        HX_BACK_NORMAL = 3,
        EGO_RAWDATA = 4,
    };

    class Chip {
        private:
            std::unique_ptr<HalDevice> m_master;
            std::unique_ptr<HalDevice> m_slave;
            std::unique_ptr<HalDevice> m_interrupt;
            
            uint8_t current_slot;
            uint32_t m_zeroFrameCount = 0;   // 连续无输入帧计数（idle 进入判断）
            uint32_t m_frameCount = 0;       // 帧计数器（2:1 交错用）
            
            ic_operation        pic_op{};
            fw_operation        pfw_op{};
            flash_operation     pflash_op{};
            sram_operation      psram_op{};
            driver_operation    pdriver_op{};
            zf_operation        pzf_op{};
        
            
            ChipResult<HalDevice*> SelectDevice(DeviceType type);
            ChipResult<> init_buffers_and_register(void);
            
            ChipResult<> hx_hw_reset_ahb_intf(DeviceType type);
            ChipResult<> hx_sw_reset_ahb_intf(DeviceType type); 
            ChipResult<> hx_is_reload_done_ahb(void);
            ChipResult<> himax_mcu_reload_disable(uint8_t disable);
            ChipResult<> himax_mcu_read_FW_status(void);

            ChipResult<> hx_sense_on(bool isHwReset);
            ChipResult<> hx_sense_off(bool check_en);
            ChipResult<> himax_mcu_power_on_init(void);

            ChipResult<> himax_mcu_assign_sorting_mode(uint8_t* tmp_data);
            ChipResult<> himax_switch_data_type(DeviceType device, THP_INSPECTION_ENUM mode);
            ChipResult<> himax_switch_mode_inspection(THP_INSPECTION_ENUM mode);
            ChipResult<> himax_mcu_interface_on(void);
            ChipResult<> hx_set_N_frame(uint8_t nFrame);

            ChipResult<> thp_afe_enter_idle(uint8_t param = 0);
            ChipResult<> thp_afe_force_exit_idle(void);
            ChipResult<> thp_afe_start_calibration(uint8_t param = 0);
            ChipResult<> thp_afe_enable_freq_shift(void);
            ChipResult<> thp_afe_disable_freq_shift(void);
            ChipResult<> thp_afe_clear_status(uint8_t cmd_val);
            ChipResult<> thp_afe_force_to_freq_point(uint8_t freq_idx);
            ChipResult<> thp_afe_force_to_scan_rate(uint8_t rate_idx);
        public:
            // ── 手写笔生命周期管理 ────────────────────────────────
            /// 手写笔连接初始化：EnableFreqShift + 绑定 FreqPair + 设置 connected
            ChipResult<> InitStylus(uint8_t pen_id = 5);
            /// 手写笔断连清理：DisableFreqShift + 重置 StylusState
            ChipResult<> DisconnectStylus();
            /// 每帧调用：从 master 状态表读取噪声计数，超阈值时切换频点
            void ProcessStylusStatus();
            alignas(64) std::array<uint8_t, 6000> back_data{};
            THP_INSPECTION_ENUM m_inspection_mode;
            std::atomic<THP_AFE_MODE> afe_mode{THP_AFE_MODE::Normal};
            StylusState m_stylus;    // 手写笔运行时状态（连接/频点/切换策略）

            // 触点/手写笔检测：从帧数据判断是否有输入
            bool isFingerDetected() const;
            bool isStylusDetected() const;

            
            // 主机的连接状态机
            std::atomic<ConnectionState> m_connState{ConnectionState::Unconnected};

            // 用于日志的前缀转换辅助函数
            const char* GetStateStr() const {
                switch(m_connState.load()) {
                    case ConnectionState::Unconnected: return "Unconnected";
                    case ConnectionState::Connected: return "Connected";
                    case ConnectionState::Error: return "Error";
                    default: return "Unknown";
                }
            }

            ChipResult<> SetFrameReadPolicy(bool block, uint8_t timeoutMs);
            ChipResult<> SetFrameReadNormalPolicy();
            ChipResult<> SetFrameReadIdlePolicy();
            ChipResult<> afe_sendCommand(command cmd);
            ChipResult<> NotifyTouchWakeup();

            Chip(const std::wstring& master_path, const std::wstring& slave_path, const std::wstring& interrupt_path);
            ~Chip(); // Add destructor for explicit cleanup
            
            bool IsReady(DeviceType type) const;
            ConnectionState GetConnectionState() const { return m_connState.load(); }
            
            ChipResult<> Init(void);
            ChipResult<> Deinit(bool check_en = true); // Replaces Stop
            ChipResult<> check_bus(void);
            ChipResult<> GetFrame(void);
            void CancelPendingFrameRead();
    };
}
