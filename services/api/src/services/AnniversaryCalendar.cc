// =====================================================================
// services/api/src/services/AnniversaryCalendar.cc
// =====================================================================
#include "services/AnniversaryCalendar.h"

#include <cstdio>
#include <tuple>

namespace usapp {

namespace {

bool digits(const std::string &s, size_t from, size_t count, int &out) {
    int value = 0;
    for (size_t i = from; i < from + count; ++i) {
        const char c = s[i];
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
    }
    out = value;
    return true;
}

// Distance used by automatic selection: the next occurrence of an anniversary, the
// target of a countdown; nullopt for a past countdown.
std::optional<int64_t> upcomingDistance(const AnniversaryCalendar::Candidate &c,
                                        const AnniversaryCalendar::Civil &today) {
    const auto computed = AnniversaryCalendar::compute(c.kind, c.date, today);
    if (c.kind == AnniversaryCalendar::Kind::Anniversary) return computed.nextDaysUntil;
    if (computed.daysUntil >= 0) return computed.daysUntil;
    return std::nullopt;
}

}  // namespace

std::optional<AnniversaryCalendar::Civil> AnniversaryCalendar::parse(const std::string &text) {
    if (text.size() != 10u || text[4] != '-' || text[7] != '-') return std::nullopt;
    Civil c;
    if (!digits(text, 0, 4, c.year) || !digits(text, 5, 2, c.month) || !digits(text, 8, 2, c.day)) {
        return std::nullopt;
    }
    if (c.year < 1 || c.month < 1 || c.month > 12 || c.day < 1 || c.day > daysInMonth(c.year, c.month)) {
        return std::nullopt;
    }
    return c;
}

bool AnniversaryCalendar::inInputRange(const Civil &date) {
    return date.year >= kMinInputYear && date.year <= kMaxInputYear;
}

std::string AnniversaryCalendar::format(const Civil &date) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", date.year, date.month, date.day);
    return buffer;
}

std::optional<AnniversaryCalendar::Kind> AnniversaryCalendar::parseKind(const std::string &text) {
    if (text == "anniversary") return Kind::Anniversary;
    if (text == "countdown") return Kind::Countdown;
    return std::nullopt;
}

const char *AnniversaryCalendar::kindName(Kind kind) {
    return kind == Kind::Anniversary ? "anniversary" : "countdown";
}

bool AnniversaryCalendar::isLeap(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int AnniversaryCalendar::daysInMonth(int year, int month) {
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && isLeap(year)) return 29;
    return kDays[month - 1];
}

// days_from_civil / civil_from_days (H. Hinnant), exact for the proleptic Gregorian calendar.
int64_t AnniversaryCalendar::toDays(const Civil &date) {
    const int64_t y = static_cast<int64_t>(date.year) - (date.month <= 2 ? 1 : 0);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t mp = (date.month + 9) % 12;
    const int64_t doy = (153 * mp + 2) / 5 + date.day - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

AnniversaryCalendar::Civil AnniversaryCalendar::fromDays(int64_t days) {
    const int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    Civil c;
    c.day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    c.month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    c.year = static_cast<int>(yoe + era * 400 + (c.month <= 2 ? 1 : 0));
    return c;
}

AnniversaryCalendar::Civil AnniversaryCalendar::addYears(const Civil &date, int years) {
    Civil c{date.year + years, date.month, date.day};
    if (c.month == 2 && c.day == 29 && !isLeap(c.year)) c.day = 28;
    return c;
}

AnniversaryCalendar::Computed AnniversaryCalendar::compute(Kind kind, const Civil &date, const Civil &today) {
    Computed out;
    out.kind = kind;
    const int64_t t = toDays(today);
    const int64_t d = toDays(date);
    if (kind == Kind::Countdown) {
        out.daysUntil = d - t;
        return out;
    }
    out.daysSince = t - d;
    // Start one year before T's year; add_years moves by whole years, so at most two
    // steps reach the first occurrence on or after T.
    int k = today.year - date.year - 1;
    if (k < 0) k = 0;
    while (toDays(addYears(date, k)) < t) ++k;
    out.years = k;
    out.next = addYears(date, k);
    out.nextDaysUntil = toDays(out.next) - t;
    return out;
}

const char *AnniversaryCalendar::sourceName(Source source) {
    switch (source) {
        case Source::Pinned: return "pinned";
        case Source::Auto: return "auto";
        case Source::AutoPast: return "auto_past";
        case Source::None: break;
    }
    return "none";
}

AnniversaryCalendar::Selection AnniversaryCalendar::select(const std::vector<Candidate> &candidates,
                                                           const Civil &today) {
    Selection out;
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (candidates[i].pinned) {
            out.source = Source::Pinned;
            out.index = i;
            return out;
        }
    }
    std::optional<size_t> best;
    int64_t bestDistance = 0;
    for (size_t i = 0; i < candidates.size(); ++i) {
        const auto distance = upcomingDistance(candidates[i], today);
        if (!distance) continue;
        const auto &c = candidates[i];
        if (!best || std::tie(*distance, c.createdAt, c.id) <
                         std::tie(bestDistance, candidates[*best].createdAt, candidates[*best].id)) {
            best = i;
            bestDistance = *distance;
        }
    }
    if (best) {
        out.source = Source::Auto;
        out.index = best;
        return out;
    }
    // Only past countdowns remain: the most recently passed one.
    for (size_t i = 0; i < candidates.size(); ++i) {
        const auto &c = candidates[i];
        if (!best) {
            best = i;
            continue;
        }
        const auto &b = candidates[*best];
        const int64_t cd = toDays(c.date);
        const int64_t bd = toDays(b.date);
        if (cd > bd || (cd == bd && std::tie(c.createdAt, c.id) < std::tie(b.createdAt, b.id))) best = i;
    }
    if (best) {
        out.source = Source::AutoPast;
        out.index = best;
    }
    return out;
}

}  // namespace usapp
