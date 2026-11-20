// =====================================================================
// services/api/src/services/AnniversaryCalendar.h
//
// Phase 2 P2-2 (docs/phase2-anniversaries.md Sections 2-4): pure civil-date arithmetic
// for anniversaries and countdowns. No time zone, std::tm or clock use: "today" is the
// Beijing civil date the database reads once per statement and passes in.
//
// - add_years(D, k): same month/day k calendar years later; Feb 29 becomes Feb 28 in a
//   non-leap year, always computed from the original D (owner choice O4 default).
// - Anniversary: days_since = T - D; next = smallest add_years(D, k) >= T with k >= 0.
//   Derived dates may lie beyond the 1900-2199 input range.
// - Countdown: days_until = D - T; no recurrence.
// - Home selection: pinned > auto (distance ASC, created_at ASC, id ASC) > auto_past
//   (D DESC, created_at ASC, id ASC) > none, over the complete current dataset.
// =====================================================================
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace usapp {

class AnniversaryCalendar {
  public:
    static constexpr int kMinInputYear = 1900;
    static constexpr int kMaxInputYear = 2199;

    struct Civil {
        int year = 1970;
        int month = 1;
        int day = 1;
        bool operator==(const Civil &o) const { return year == o.year && month == o.month && day == o.day; }
        bool operator!=(const Civil &o) const { return !(*this == o); }
    };

    enum class Kind { Anniversary, Countdown };

    // Strict "YYYY-MM-DD" with a real calendar day, years 0001-9999. No range rule here.
    static std::optional<Civil> parse(const std::string &text);
    // A stored or submitted date: 1900-01-01 .. 2199-12-31.
    static bool inInputRange(const Civil &date);
    // "YYYY-MM-DD" (at least four year digits).
    static std::string format(const Civil &date);
    static std::optional<Kind> parseKind(const std::string &text);
    static const char *kindName(Kind kind);

    static bool isLeap(int year);
    static int daysInMonth(int year, int month);
    // Days since 1970-01-01 (proleptic Gregorian), and back.
    static int64_t toDays(const Civil &date);
    static Civil fromDays(int64_t days);
    static Civil addYears(const Civil &date, int years);

    struct Computed {
        Kind kind = Kind::Anniversary;
        // Anniversary only.
        int64_t daysSince = 0;  // T - D; negative before D
        Civil next;             // smallest add_years(D, k) >= T
        int years = 0;          // k
        int64_t nextDaysUntil = 0;
        // Countdown only.
        int64_t daysUntil = 0;  // D - T
    };
    static Computed compute(Kind kind, const Civil &date, const Civil &today);

    enum class Source { Pinned, Auto, AutoPast, None };
    static const char *sourceName(Source source);

    struct Candidate {
        std::string id;  // canonical lowercase uuid
        Kind kind = Kind::Anniversary;
        Civil date;
        std::string createdAt;  // fixed-width "YYYY-MM-DDTHH:MM:SS.ffffffZ" (UTC): orders as text
        bool pinned = false;    // the caller's current pin
    };
    struct Selection {
        Source source = Source::None;
        std::optional<size_t> index;  // into the candidates
    };
    static Selection select(const std::vector<Candidate> &candidates, const Civil &today);
};

}  // namespace usapp
