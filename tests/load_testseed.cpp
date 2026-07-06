// Standalone repro: load TestSeed.json through ootmm_core exactly like the port does
// (LoadSeedFromJson -> Runtime::LoadSeed) to isolate a core bug from a port ABI mismatch.
#include "ootmm/Runtime.hpp"
#include "ootmm/Seed.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : "TestSeed.json";
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "cannot open " << path << "\n";
        return 2;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string json = ss.str();
    std::cout << "read " << json.size() << " bytes\n";

    ootmm::Seed seed = ootmm::LoadSeedFromJson(json);
    std::cout << "parsed: placements=" << seed.placements.size()
              << " entrances=" << seed.entrances.size()
              << " startingItems=" << seed.startingItems.size() << "\n";

    ootmm::Runtime runtime;
    runtime.LoadSeed(std::move(seed));
    std::cout << "LoadSeed OK, completedChecks=" << runtime.CompletedCheckCount()
              << " queued=" << runtime.QueuedItemCount() << "\n";
    return 0;
}
