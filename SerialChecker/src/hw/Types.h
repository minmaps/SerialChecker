#pragma once
#include <string>
#include <vector>

namespace hw {

// One "label : value" row. placeholder = empty, OEM default string or unsupported read.
struct Field {
    std::string label;
    std::string value;
    bool placeholder = false;
};

// A titled block of fields; badge names where the data comes from (SMBIOS, IOCTL, EDID...).
struct Card {
    std::string title;
    std::string badge;
    std::vector<Field> fields;
};

struct Section {
    std::string name;
    std::vector<Card> cards;
};

struct Report {
    std::string computerName;
    std::string capturedAt;  // local time, "YYYY-MM-DD HH:MM:SS"
    bool elevated = false;
    std::vector<Section> sections;
};

} // namespace hw
