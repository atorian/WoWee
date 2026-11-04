#include "core/thread_priority.hpp"

#include "core/logger.hpp"

#include <atomic>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread/qos.h>
#else
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace wowee::core {

void runBelowFrameThreads() {
#if defined(_WIN32)
    const bool lowered = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL) != 0;
#elif defined(__APPLE__)
    const bool lowered = pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0) == 0;
#else
    // A Linux thread has a nice value of its own, set through its id.
    const bool lowered = setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 5) == 0;
#endif
    static std::atomic<bool> said{false};
    if (!lowered && !said.exchange(true)) {
        LOG_WARNING("A loading thread could not be put below the frame threads; loads compete with the frame "
                    "for the cores");
    }
}

}  // namespace wowee::core
