#pragma once

#include "voicelife/timing/timing_task_store.h"

namespace voicelife::timing {

class TimingTaskRunner {
   public:
    TimingTaskRunner(TimingTaskStorePort& store, TimingClockPort& clock, TimingIdGeneratorPort& ids,
                     TimingEventPort& events)
        : store_(store), clock_(clock), ids_(ids), events_(events) {}

    // Called by an ESP-IDF/FreeRTOS task adapter. This core runner does not own a thread.
    Status PollDue();

   private:
    TimingTaskStorePort& store_;
    TimingClockPort& clock_;
    TimingIdGeneratorPort& ids_;
    TimingEventPort& events_;
};

}  // namespace voicelife::timing
