#include "Coordinator.h"
#include "Logger.h"
#include "MasterFrameParser.h"
#include "BaselineSubtraction.h"
#include "CMFProcessor.h"
#include "GridIIRProcessor.h"
#include "FeatureExtractor.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace App {

// --- 设备路径 ---
const std::wstring DEVICE_PATH_INTERRUPT = L"\\\\.\\Global\\SPBTESTTOOL_MASTER";
const std::wstring DEVICE_PATH_MASTER = L"\\\\.\\Global\\SPBTESTTOOL_MASTER";
const std::wstring DEVICE_PATH_SLAVE = L"\\\\.\\Global\\SPBTESTTOOL_SLAVE";

Coordinator::Coordinator() {
    LOG_INFO("App", "Coordinator::Coordinator", "Unconnected", "Initializing Himax Device Instance (Unconnected)...");
    // 初始化 Hardware 层对象，此时只分配资源，不拉起 I2C 通信
    m_device = std::make_unique<Himax::Chip>(DEVICE_PATH_MASTER, DEVICE_PATH_SLAVE, DEVICE_PATH_INTERRUPT);
    m_dvrBuffer = std::make_unique<RingBuffer<Engine::HeatmapFrame, 480>>();

    // Initialise Engine Pipeline
    m_pipeline.AddProcessor(std::make_unique<Engine::MasterFrameParser>());
    m_pipeline.AddProcessor(std::make_unique<Engine::BaselineSubtraction>());
    m_pipeline.AddProcessor(std::make_unique<Engine::CMFProcessor>());
    m_pipeline.AddProcessor(std::make_unique<Engine::GridIIRProcessor>());
    m_pipeline.AddProcessor(std::make_unique<Engine::FeatureExtractor>());
    // m_pipeline.AddProcessor(std::make_unique<Engine::CentroidExtractor>());

    
    // Load config on startup
    LoadConfig();
}

Coordinator::~Coordinator() {
    Stop();
}

bool Coordinator::Start() {
    if (m_running.exchange(true)) return false; // Already running

    LOG_INFO("App", "Coordinator::Start", "Unknown", "Starting background threads...");
    
    // 启动 Himax AFE (假设它已经在构造里或外面调了，或者在这里调)
    // 也可以留给 GUI 去手动点击 "Start AFE"
    
    m_acquisitionThread = std::thread(&Coordinator::AcquisitionThreadFunc, this);
    m_processingThread = std::thread(&Coordinator::ProcessingThreadFunc, this);
    m_systemStateThread = std::thread(&Coordinator::SystemStateThreadFunc, this);

    return true;
}

void Coordinator::Stop() {
    if (!m_running.exchange(false)) return;

    LOG_INFO("App", "Coordinator::Stop", "Unknown", "Stopping background threads...");

    if (m_acquisitionThread.joinable()) m_acquisitionThread.join();
    if (m_processingThread.joinable()) m_processingThread.join();
    if (m_systemStateThread.joinable()) m_systemStateThread.join();
}

bool Coordinator::GetLatestFrame(Engine::HeatmapFrame& outFrame) {
    std::lock_guard<std::mutex> lock(m_latestFrameMutex);
    outFrame = m_latestFrame;
    return true;
}

void Coordinator::AcquisitionThreadFunc() {
    LOG_INFO("App", "Coordinator::AcquisitionThreadFunc", "Unknown", "Acquisition Thread started.");
    
    while (m_running) {
        if (m_device->GetConnectionState() != Himax::ConnectionState::Connected || !m_isAcquiring.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        if (auto res = m_device->GetFrame(); !res) {
            // Handle error, maybe logging is enough for now as Device does it
            continue;
        }
        
        // 提取采集的数据到帧对象
        Engine::HeatmapFrame frame;
        
        // m_device->back_data is std::array<uint8_t, ...> inside the HAL. 
        // We copy it out for processing. We take the 5063 master bytes (+339 slave)
        frame.rawData.assign(m_device->back_data.begin(), m_device->back_data.end());

        // Push Raw data into Processing Thread
        m_frameBuffer.Push(frame);

        std::this_thread::sleep_for(std::chrono::milliseconds(2)); // Polling Interval
    }
    LOG_INFO("App", "Coordinator::AcquisitionThreadFunc", "Unknown", "Acquisition Thread stopped.");
}

void Coordinator::ProcessingThreadFunc() {
    LOG_INFO("App", "Coordinator::ProcessingThreadFunc", "Unknown", "Processing Thread started.");
    while (m_running) {
        Engine::HeatmapFrame frame;
        // 阻塞等待采集线程 push 原始帧
        if (m_frameBuffer.WaitForData(frame, std::chrono::milliseconds(100))) {
            
            // Execute the pipeline (MasterFrameParser -> BaselineSubtraction -> ...)
            if (m_pipeline.Execute(frame)) {

                // 如果处理成功, 写回给 GUI 
                {
                    std::lock_guard<std::mutex> lock(m_latestFrameMutex);
                    m_latestFrame = frame;
                }

                // Push to DVR buffer (automatically overwrites old frames)
                m_dvrBuffer->PushOverwriting(frame);

                // TODO: (Stage 3) 交给 Host::VhfInjector 发送 HID Report
            }
        }
    }
    LOG_INFO("App", "Coordinator::ProcessingThreadFunc", "Unknown", "Processing Thread stopped.");
}

void Coordinator::SystemStateThreadFunc() {
    LOG_INFO("App", "Coordinator::SystemStateThreadFunc", "Unknown", "SystemState Thread started.");
    // TODO: (Stage 3) 使用 Host/SystemMonitor 监听亮屏/息屏，并调用 m_device->thp_afe_enter_idle()
    while (m_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    LOG_INFO("App", "Coordinator::SystemStateThreadFunc", "Unknown", "SystemState Thread stopped.");
}

void Coordinator::TriggerDVRExport(bool exportHeatmap, bool exportMasterStatus, bool exportSlaveStatus) {
    if (!m_dvrBuffer) {
        LOG_ERROR("App", "Coordinator::TriggerDVRExport", "Unknown", "DVR buffer is not initialized.");
        return;
    }

    auto snapshot = m_dvrBuffer->GetSnapshot();
    if (snapshot.empty()) {
        LOG_WARN("App", "Coordinator::TriggerDVRExport", "Unknown", "DVR buffer is empty, nothing to export.");
        return;
    }

    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    struct tm time_info;
    localtime_s(&time_info, &time_t_now);
    
    std::filesystem::path dir("exports/dvr");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        LOG_ERROR("App", "Coordinator::TriggerDVRExport", "Unknown", "Failed to create directory: exports/dvr");
    }

    char filename[128];
    sprintf_s(filename, "dvr_backtrack_%04d%02d%02d_%02d%02d%02d.csv",
              time_info.tm_year + 1900, time_info.tm_mon + 1, time_info.tm_mday,
              time_info.tm_hour, time_info.tm_min, time_info.tm_sec);
              
    std::filesystem::path fullPath = dir / filename;

    FILE* fp = nullptr;
    fopen_s(&fp, fullPath.string().c_str(), "w");
    if (!fp) {
        LOG_ERROR("App", "Coordinator::TriggerDVRExport", "Unknown", "Failed to create DVR export file: %s", fullPath.string().c_str());
        return;
    }

    LOG_INFO("App", "Coordinator::TriggerDVRExport", "Unknown", "Exporting %zu frames to %s...", snapshot.size(), fullPath.string().c_str());

    for (size_t i = 0; i < snapshot.size(); ++i) {
        const auto& f = snapshot[i];
        fprintf(fp, "--- Frame [%zu] --- TS: %llu\n", i, f.timestamp);
        
        // Contacts are always printed
        fprintf(fp, "Contacts: %zu\n", f.contacts.size());
        for (const auto& c : f.contacts) {
            fprintf(fp, "ID:%d, X:%.3f, Y:%.3f, State:%d, Area:%d\n", 
                    c.id, c.x, c.y, c.state, c.area);
        }
        
        if (exportHeatmap) {
            fprintf(fp, "Heatmap:\n");
            for (int y = 0; y < 40; ++y) {
                for (int x = 0; x < 60; ++x) {
                    fprintf(fp, "%d%s", f.heatmapMatrix[y][x], (x == 59 ? "" : ","));
                }
                fprintf(fp, "\n");
            }
        }

        if (exportMasterStatus) {
            fprintf(fp, "Master Status Suffix:\n");
            if (f.rawData.size() >= 5063) {
                const uint8_t* ptr = f.rawData.data() + 4807;
                for (int j = 0; j < 128; ++j) {
                    uint16_t val = static_cast<uint16_t>(ptr[j * 2] | (ptr[j * 2 + 1] << 8));
                    fprintf(fp, "%d%s", val, (j == 127 ? "" : ","));
                }
                fprintf(fp, "\n");
            } else {
                fprintf(fp, "Data unavailable\n");
            }
        }

        if (exportSlaveStatus) {
            fprintf(fp, "Slave Status Suffix:\n");
            if (f.rawData.size() >= 5402) {
                const uint8_t* ptr = f.rawData.data() + 5070;
                for (int j = 0; j < 166; ++j) {
                    uint16_t val = static_cast<uint16_t>(ptr[j * 2] | (ptr[j * 2 + 1] << 8));
                    fprintf(fp, "%d%s", val, (j == 165 ? "" : ","));
                }
                fprintf(fp, "\n");
            } else {
                fprintf(fp, "Data unavailable\n");
            }
        }
        
        fprintf(fp, "\n");
    }

    fclose(fp);
    LOG_INFO("App", "Coordinator::TriggerDVRExport", "Unknown", "DVR Export Complete: %s", fullPath.string().c_str());
}

void Coordinator::SaveConfig() {
    std::ofstream out("config.ini");
    if (!out.is_open()) {
        LOG_ERROR("App", "Coordinator::SaveConfig", "System", "Failed to open config.ini for writing.");
        return;
    }
    for (const auto& p : m_pipeline.GetProcessors()) {
        out << "[" << p->GetName() << "]\n";
        p->SaveConfig(out);
        out << "\n";
    }
    out.close();
    LOG_INFO("App", "Coordinator::SaveConfig", "System", "Successfully saved global parameters to config.ini");
}

void Coordinator::LoadConfig() {
    std::ifstream in("config.ini");
    if (!in.is_open()) {
        LOG_INFO("App", "Coordinator::LoadConfig", "System", "No config.ini found. Using default parameters.");
        return;
    }
    
    std::string line;
    Engine::IFrameProcessor* currentProcessor = nullptr;
    
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back(); // Handle Windows CRLF
        if (line.empty() || line[0] == ';') continue;
        
        if (line[0] == '[') {
            size_t endBracket = line.find(']');
            if (endBracket != std::string::npos) {
                std::string section = line.substr(1, endBracket - 1);
                currentProcessor = nullptr;
                for (const auto& p : m_pipeline.GetProcessors()) {
                    if (p->GetName() == section) {
                        currentProcessor = p.get();
                        break;
                    }
                }
            }
            continue;
        }
        
        auto pos = line.find('=');
        if (pos != std::string::npos && currentProcessor) {
            std::string key = line.substr(0, pos);
            std::string val = line.substr(pos + 1);
            
            // Explicitly handle "Enabled" since it's in the base class IFrameProcessor
            if (key == "Enabled") {
                currentProcessor->SetEnabled(val == "1" || val == "true");
            } else {
                currentProcessor->LoadConfig(key, val);
            }
        }
    }
    in.close();
    LOG_INFO("App", "Coordinator::LoadConfig", "System", "Successfully loaded global parameters from config.ini");
}

} // namespace App
