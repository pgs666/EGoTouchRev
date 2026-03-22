#include <atomic>
#include <cstdint>
#include <expected>
#include <mutex>
#include <queue>
#include "Device.h"
#include "himax/HimaxChip.h"


enum class result {
    error,
    timeout,
};

using ThreadResult = std::expected<void, result>;

enum class workerState{
    quit = -1,
    ready = 0,
    streaming,
    recover,
};

class Worker{
private:
    std::atomic<workerState> currentState;
    std::atomic<bool> stopRequested;
    Himax::Chip chip;
    uint8_t recover_count;
    std::mutex commandMutex;
    std::queue<command> commandQueue;

public:
    void pendCommand(command cmd);

    void stop();
    ThreadResult workThread();
    Worker();
};