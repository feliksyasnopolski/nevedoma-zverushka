#include <property_info_serializer/property_info_serializer.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using android::properties::BuildTrie;
using android::properties::ParsePropertyInfoFile;
using android::properties::PropertyInfoEntry;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: " << argv[0] << " OUTPUT CONTEXT_FILE...\n";
        return 2;
    }
    std::vector<PropertyInfoEntry> entries;
    size_t rejected = 0;
    for (int i = 2; i < argc; ++i) {
        std::ifstream input(argv[i]);
        if (!input) { std::cerr << "open " << argv[i] << " failed\n"; return 1; }
        std::ostringstream text; text << input.rdbuf();
        std::vector<std::string> errors;
        size_t before = entries.size();
        ParsePropertyInfoFile(text.str(), true, &entries, &errors);
        rejected += errors.size();
        std::cerr << "input=" << argv[i] << " entries=" << (entries.size() - before)
                  << " rejected=" << errors.size() << "\n";
        for (const auto& error : errors) std::cerr << "  " << error << "\n";
    }
    std::string serialized, error;
    if (!BuildTrie(entries, "u:object_r:default_prop:s0", "string", &serialized, &error)) {
        std::cerr << "BuildTrie failed: " << error << "\n";
        return 1;
    }
    std::ofstream output(argv[1], std::ios::binary | std::ios::trunc);
    if (!output) { std::cerr << "open output failed\n"; return 1; }
    output.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
    if (!output) { std::cerr << "write output failed\n"; return 1; }
    std::cerr << "serialized_bytes=" << serialized.size() << " entries=" << entries.size()
              << " rejected=" << rejected << " output=" << argv[1] << "\n";
}
