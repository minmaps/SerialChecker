// Marks the fields of a fresh report that differ from a previously exported one.
#include "hw/Collector.h"

#include <map>
#include <string>

namespace hw {
namespace {

// Cards are matched by section + title (+ occurrence, for repeated titles), fields by label.
std::string CardKey(const std::string& section, const std::string& title, int occurrence)
{
    return section + '\x1f' + title + '\x1f' + std::to_string(occurrence);
}

// A read that failed only because of missing rights is not a hardware change.
bool IsAccessNote(const std::string& v) { return v == "Requires administrator"; }

} // namespace

int ApplyDiff(Report& current, const Report& baseline)
{
    std::map<std::string, const Card*> before;
    for (const auto& s : baseline.sections) {
        std::map<std::string, int> seen;
        for (const auto& c : s.cards) before[CardKey(s.name, c.title, seen[c.title]++)] = &c;
    }

    int total = 0;
    for (auto& s : current.sections) {
        s.changes = 0;
        std::map<std::string, int> seen;
        for (auto& c : s.cards) {
            c.changes = 0;
            auto it = before.find(CardKey(s.name, c.title, seen[c.title]++));
            const Card* old = it == before.end() ? nullptr : it->second;
            for (auto& f : c.fields) {
                f.diff = Diff::None;
                f.previous.clear();
                const Field* prev = nullptr;
                if (old)
                    for (const auto& of : old->fields)
                        if (of.label == f.label) { prev = &of; break; }

                if (!prev) {
                    f.diff = Diff::Added;
                } else if (prev->value != f.value && !IsAccessNote(prev->value) && !IsAccessNote(f.value)) {
                    f.diff = Diff::Changed;
                    f.previous = prev->value;
                }
                if (f.diff != Diff::None) ++c.changes;
            }
            s.changes += c.changes;
        }
        total += s.changes;
    }
    return total;
}

void ClearDiff(Report& report)
{
    for (auto& s : report.sections) {
        s.changes = 0;
        for (auto& c : s.cards) {
            c.changes = 0;
            for (auto& f : c.fields) {
                f.diff = Diff::None;
                f.previous.clear();
            }
        }
    }
}

} // namespace hw
