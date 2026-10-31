// Marks the fields of a fresh report that differ from a previously exported one.
#include "hw/Collector.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace hw {
namespace {

// A read that failed only because of missing rights is not a hardware change.
bool IsAccessNote(const std::string& v) { return v == "Requires administrator"; }

// Number of fields holding the same real value in both cards; placeholders say nothing about identity.
int Similarity(const Card& a, const Card& b)
{
    int n = 0;
    for (const auto& fa : a.fields) {
        if (fa.placeholder) continue;
        for (const auto& fb : b.fields)
            if (fb.label == fa.label) {
                if (fb.value == fa.value) ++n;
                break;
            }
    }
    return n;
}

// Pairs each card of a section with its baseline card. Titles are not reliable identities (PhysicalDriveN shifts
// when a drive is added or re-enumerated), so cards sharing the most values are paired first, preferring the same
// title on ties. Cards that share nothing fall back to the same title, in order of occurrence.
std::vector<const Card*> MatchCards(const Section& current, const Section* baseline)
{
    std::vector<const Card*> match(current.cards.size(), nullptr);
    if (!baseline) return match;
    const auto& olds = baseline->cards;

    struct Pair {
        int score;
        bool sameTitle;
        size_t cur, old;
    };
    std::vector<Pair> pairs;
    for (size_t c = 0; c < current.cards.size(); ++c)
        for (size_t o = 0; o < olds.size(); ++o)
            if (int score = Similarity(current.cards[c], olds[o]); score > 0)
                pairs.push_back({ score, current.cards[c].title == olds[o].title, c, o });
    std::stable_sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) {
        return a.score != b.score ? a.score > b.score : a.sameTitle > b.sameTitle;
    });

    std::vector<bool> used(olds.size());
    for (const auto& p : pairs)
        if (!match[p.cur] && !used[p.old]) {
            match[p.cur] = &olds[p.old];
            used[p.old] = true;
        }
    for (size_t c = 0; c < current.cards.size(); ++c)
        for (size_t o = 0; !match[c] && o < olds.size(); ++o)
            if (!used[o] && olds[o].title == current.cards[c].title) {
                match[c] = &olds[o];
                used[o] = true;
            }
    return match;
}

} // namespace

int ApplyDiff(Report& current, const Report& baseline)
{
    std::map<std::string, const Section*> before;
    for (const auto& s : baseline.sections) before[s.name] = &s;

    int total = 0;
    for (auto& s : current.sections) {
        s.changes = 0;
        auto it = before.find(s.name);
        std::vector<const Card*> match = MatchCards(s, it == before.end() ? nullptr : it->second);
        for (size_t i = 0; i < s.cards.size(); ++i) {
            Card& c = s.cards[i];
            c.changes = 0;
            const Card* old = match[i];
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
