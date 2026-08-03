#include "support/timing_fakes.h"
#include "support/timing_store_contract.h"

int main() {
    voicelife::test::InMemoryTimingTaskStore store;
    voicelife::test::RunTimingStoreContract(store, "memory-contract");
    return 0;
}
