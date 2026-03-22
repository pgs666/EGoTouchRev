#include <expected>
#include <queue>
#include "runtime/DeviceRuntime.h"

void Worker::pendCommand(command cmd) {
    if (currentState.load() == workerState::quit) {
        return;
    }
    std::lock_guard<std::mutex> lock(commandMutex);
    commandQueue.push(cmd);
}

void Worker::stop() {
    stopRequested.store(true);
}

ThreadResult Worker::workThread() {
    while (true) {
        if (stopRequested.load(std::memory_order_acquire)) {
            currentState.store(workerState::quit);
            stopRequested.store(false, std::memory_order_release);
            //清空命令队列
            std::lock_guard<std::mutex> lock(commandMutex);
            std::queue<command>().swap(commandQueue);
        }
        //如果外部有命令
        {
            std::lock_guard<std::mutex> lock(commandMutex);
            while (!commandQueue.empty()) {
                command cmd = commandQueue.front();
                if (auto res = chip.afe_sendCommand(cmd); !res) {
                    currentState.store(workerState::recover);
                    break;
                }
                commandQueue.pop();
            }

        }

        switch (currentState.load(std::memory_order_acquire)) {
        case workerState::ready:
            if (auto res = chip.Init(); !res) {
                currentState.store(workerState::recover);
                break;
            }
            currentState.store(workerState::streaming);
            break;

        case workerState::streaming:
            if (auto res = chip.GetFrame(); res == std::unexpected(ChipError::Timeout)) {
                continue;
            } else if (res == std::unexpected(ChipError::CommunicationError)) {
                currentState.store(workerState::recover);
                break;
            } else if (res == std::unexpected(ChipError::InternalError)) {
                currentState.store(workerState::recover);
                break;
            }
            break;

        case workerState::quit:
            if (auto res = chip.Deinit(false); !res) {
                return std::unexpected(result::error);
            }
            return ThreadResult();

        case workerState::recover:
            recover_count++;
            if (recover_count > 10) {
                currentState.store(workerState::quit);
                return std::unexpected(result::error);
            }
            if (auto res = chip.check_bus(); !res) {
                break;
            } else if (auto res = chip.Init(); !res) {
                break;
            } else {
                currentState.store(workerState::ready);
                recover_count = 0;
                break;
            }
        }
    }
    return ThreadResult();
}