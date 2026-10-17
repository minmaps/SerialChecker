#pragma once
#include <string>
#include <vector>

namespace hw {

// Result of comparing a field with a previously exported report.
enum class Diff { None, Changed, Added };

// One "label : value" row. placeholder = empty, OEM default string or unsupported read.
struct Field {
    std::string label;
    std::string value;
    bool placeholder = false;
    Diff diff = Diff::None;
    std::string previous;  // baseline value when diff == Changed
};

// A titled block of fields; badge names where the data comes from (SMBIOS, IOCTL, EDID...).
struct Card {
    std::string title;
    std::string badge;
    std::vector<Field> fields;
    int changes = 0;
};

struct Section {
    std::string name;
    std::vector<Card> cards;
    int changes = 0;
};

struct Report {
    std::string computerName;
    std::string capturedAt;  // local time, "YYYY-MM-DD HH:MM:SS"
    bool elevated = false;
    std::vector<Section> sections;
};

} // namespace hw
