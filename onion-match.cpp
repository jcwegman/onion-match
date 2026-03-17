#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_set>
#include <unistd.h>
#include <vector>

namespace {

// This program filters candidate .onion hostnames. It combines:
// - prefixes loaded from one file,
// - dictionary words loaded from another file,
// - candidate hostnames streamed on stdin.
//
// For each candidate line, we try to match:
//   prefix + exactly N valid segments
//
// The helpers in this file are organized top-down:
// parsing utilities, matching utilities, rendering utilities, and finally
// main() to orchestrate the end-to-end workflow.

// Use a small set of selective std imports instead of "using namespace std;" so
// common types stay readable without losing all qualification context.
using std::binary_search;
using std::cerr;
using std::cin;
using std::cout;
using std::exit;
using std::getline;
using std::ifstream;
using std::istream;
using std::min;
using std::move;
using std::numeric_limits;
using std::size_t;
using std::sort;
using std::string;
using std::unique;
using std::unordered_set;
using std::vector;

// Shared constants for "unbounded" lengths and ANSI color output.
constexpr size_t kUnlimitedLength = numeric_limits<size_t>::max();
constexpr const char* kPrefixColor = "\033[1;31m";
constexpr const char* kSuffixColors[] = {
    "\033[1;34m",
    "\033[1;33m",
    "\033[1;35m",
    "\033[1;36m",
};
constexpr const char* kSingleColor = "\033[1;31m";
constexpr const char* kColorEnd = "\033[0m";

// Parsing helpers distinguish "this flag does not apply" from "this flag
// applies but contains an invalid value" so argv can be scanned once.
enum class ParseStatus {
    kNoMatch,
    kSuccess,
    kInvalid,
};

// Output coloring modes supported by --color=...
enum class ColorMode {
    kAuto,
    kYes,
    kNo,
    kMulti,
};

// One allowed segment-length window. When the chain is longer than the number
// of configured ranges, the final range is reused for the remaining segments.
struct LengthRange {
    size_t min_len = 1;
    size_t max_len = kUnlimitedLength;
};

// Memoized answer for one recursive search state.
// next_length stores the chosen segment length at this step, and final_end
// stores how far the chosen full chain reaches in the line.
struct SearchState {
    bool computed = false;
    bool matched = false;
    size_t next_length = 0;
    size_t final_end = 0;
};

// Best match chosen for a candidate line.
struct MatchResult {
    size_t prefix_end = 0;
    vector<size_t> segment_ends;

    bool matched() const {
        return !segment_ends.empty();
    }

    size_t matched_end() const {
        return segment_ends.back();
    }
};

// Match coordinates rewritten for the rendered output string. This matters when
// --separator inserts '+' characters and shifts later character positions.
struct RenderedMatch {
    string line;
    size_t prefix_end = 0;
    size_t matched_end = 0;
    vector<size_t> segment_ends;
};

// Dictionary words plus their distinct lengths, which act as a quick prefilter
// before substring hash lookups.
struct Dictionary {
    unordered_set<string> words;
    vector<size_t> lengths;
};

// Parsed command-line options.
struct Options {
    string prefix_file_path;
    string words_file_path;
    vector<LengthRange> ranges{LengthRange{3, kUnlimitedLength}};
    size_t chain_length = 1;
    size_t min_total_length = 0;
    ColorMode color_mode = ColorMode::kAuto;
    bool use_separator = false;
    bool allow_numbers = false;
    bool strict_v3 = false;
};

// Normalize input to handle both LF and CRLF line endings.
string trim_line_endings(string line) {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
    }
    return line;
}

// Simple helper used in both option parsing and prefix matching.
bool starts_with(const string& text, const string& prefix) {
    return text.size() >= prefix.size() &&
           text.compare(0, prefix.size(), prefix) == 0;
}

// Parse a non-negative decimal integer into size_t while rejecting overflow.
bool parse_size_value(const string& text, size_t& value) {
    if (text.empty()) {
        return false;
    }

    value = 0;
    for (unsigned char ch : text) {
        if (!std::isdigit(ch)) {
            return false;
        }

        const size_t digit = static_cast<size_t>(ch - '0');
        if (value > (numeric_limits<size_t>::max() - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }

    return true;
}

// Parse options shaped like "--flag=123".
ParseStatus parse_size_option(const string& text,
                              const string& flag_name,
                              size_t& value,
                              bool require_positive = false) {
    if (!starts_with(text, flag_name)) {
        return ParseStatus::kNoMatch;
    }
    if (!parse_size_value(text.substr(flag_name.size()), value)) {
        return ParseStatus::kInvalid;
    }
    if (require_positive && value == 0) {
        return ParseStatus::kInvalid;
    }
    return ParseStatus::kSuccess;
}

// Parse either "5" or "3-7" into a length interval.
bool parse_length_filter(const string& text,
                         size_t& min_len,
                         size_t& max_len) {
    const size_t dash = text.find('-');
    if (dash == string::npos) {
        return parse_size_value(text, min_len) &&
               (max_len = min_len, true) &&
               min_len > 0;
    }

    if (text.find('-', dash + 1) != string::npos) {
        return false;
    }

    string left = text.substr(0, dash);
    string right = text.substr(dash + 1);
    return parse_size_value(left, min_len) &&
           parse_size_value(right, max_len) &&
           min_len > 0 &&
           min_len <= max_len;
}

// Parse a comma-separated list of ranges used by --range=R1,R2,...
ParseStatus parse_range_list_argument(const string& text,
                                      vector<LengthRange>& ranges) {
    ranges.clear();

    size_t start = 0;
    while (start <= text.size()) {
        const size_t comma = text.find(',', start);
        const string token = text.substr(start, comma - start);
        LengthRange range;
        if (!parse_length_filter(token, range.min_len, range.max_len)) {
            return ParseStatus::kInvalid;
        }
        ranges.push_back(range);

        if (comma == string::npos) {
            break;
        }
        start = comma + 1;
    }

    return ranges.empty() ? ParseStatus::kInvalid : ParseStatus::kSuccess;
}

// Convenience wrapper that only parses when the flag prefix matches.
ParseStatus parse_range_list_option(const string& text,
                                    const string& flag_name,
                                    vector<LengthRange>& ranges) {
    if (!starts_with(text, flag_name)) {
        return ParseStatus::kNoMatch;
    }
    return parse_range_list_argument(text.substr(flag_name.size()), ranges);
}

// Parse --color=auto|yes|no|multi.
ParseStatus parse_color_mode(const string& text, ColorMode& mode) {
    constexpr const char* kFlag = "--color=";
    if (!starts_with(text, kFlag)) {
        return ParseStatus::kNoMatch;
    }

    const string value = text.substr(8);
    if (value == "auto") {
        mode = ColorMode::kAuto;
        return ParseStatus::kSuccess;
    }
    if (value == "yes") {
        mode = ColorMode::kYes;
        return ParseStatus::kSuccess;
    }
    if (value == "no") {
        mode = ColorMode::kNo;
        return ParseStatus::kSuccess;
    }
    if (value == "multi") {
        mode = ColorMode::kMulti;
        return ParseStatus::kSuccess;
    }

    return ParseStatus::kInvalid;
}

// Optional numeric segments must contain digits only.
bool is_all_digits(const string& text,
                   size_t start,
                   size_t length) {
    for (size_t i = 0; i < length; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[start + i]))) {
            return false;
        }
    }
    return true;
}

// v3 onion hostnames permit lowercase letters plus digits 2-7.
bool is_v3_char(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= '2' && ch <= '7');
}

// Check whether a substring stays within the v3 character alphabet.
bool is_v3_compatible(const string& text,
                      size_t start,
                      size_t length) {
    for (size_t i = 0; i < length; ++i) {
        if (!is_v3_char(text[start + i])) {
            return false;
        }
    }
    return true;
}

// Whole-string overload for the helper above.
bool is_v3_compatible(const string& text) {
    return is_v3_compatible(text, 0, text.size());
}

// Keep only dictionary lengths that are usable for a given range. This avoids
// wasted substring/hash work for segment lengths that can never match.
vector<size_t> filter_lengths(const vector<size_t>& lengths,
                              size_t min_len,
                              size_t max_len) {
    vector<size_t> filtered;
    for (size_t len : lengths) {
        if (len >= min_len && len <= max_len) {
            filtered.push_back(len);
        }
    }
    return filtered;
}

// Decide whether text[start, start + length) can be used as one segment.
//
// A segment is valid when:
// - it stays inside the v3 alphabet if strict mode is enabled, and
// - it is either all digits (when --numbers is enabled) or an exact dictionary
//   word of an allowed length.
bool matches_segment(const string& text,
                     size_t start,
                     size_t length,
                     const unordered_set<string>& words,
                     const vector<size_t>& word_lengths,
                     bool allow_numbers,
                     bool strict_v3) {
    if (strict_v3 && !is_v3_compatible(text, start, length)) {
        return false;
    }
    if (allow_numbers && is_all_digits(text, start, length)) {
        return true;
    }
    if (!binary_search(word_lengths.begin(), word_lengths.end(), length)) {
        return false;
    }
    return words.find(text.substr(start, length)) != words.end();
}

// Recursive, memoized search for the best segment chain from one position.
//
// "Best" means:
// - consume as much of the input line as possible
// - if tied, prefer the longer current segment
//
// The recursion advances one segment at a time from left to right. Memoization
// is important because multiple prefixes can lead to the same suffix state.
const SearchState& find_best_chain(
    const string& text,
    size_t start,
    size_t chain_index,
    const vector<LengthRange>& ranges,
    const vector<vector<size_t>>& range_lengths,
    size_t chain_length,
    const unordered_set<string>& words,
    bool allow_numbers,
    bool strict_v3,
    vector<vector<SearchState>>& memo) {
    SearchState& state = memo[chain_index][start];
    if (state.computed) {
        return state;
    }

    state.computed = true;

    const size_t range_index = min(chain_index, ranges.size() - 1);
    const LengthRange& range = ranges[range_index];
    const vector<size_t>& allowed_lengths = range_lengths[range_index];
    const size_t available = text.size() - start;
    const size_t limit = min(range.max_len, available);
    if (limit < range.min_len) {
        return state;
    }

    // Try longer segments first so a successful chain naturally prefers them.
    // The explicit break at the minimum avoids unsigned underflow on size_t.
    for (size_t len = limit; len >= range.min_len; --len) {
        if (!matches_segment(
                text, start, len, words, allowed_lengths, allow_numbers, strict_v3)) {
            if (len == range.min_len) {
                break;
            }
            continue;
        }

        size_t final_end = start + len;
        if (chain_index + 1 < chain_length) {
            // Only recurse when more required segments remain.
            const SearchState& child = find_best_chain(
                text,
                start + len,
                chain_index + 1,
                ranges,
                range_lengths,
                chain_length,
                words,
                allow_numbers,
                strict_v3,
                memo);
            if (!child.matched) {
                if (len == range.min_len) {
                    break;
                }
                continue;
            }
            final_end = child.final_end;
        }

        // Retain whichever chain reaches farthest to the right, breaking ties
        // in favor of the longer immediate segment.
        if (!state.matched || final_end > state.final_end ||
            (final_end == state.final_end && len > state.next_length)) {
            state.matched = true;
            state.next_length = len;
            state.final_end = final_end;
        }

        if (len == range.min_len) {
            break;
        }
    }

    return state;
}

void print_usage(const char* program_name) {
    cerr << "Usage: " << program_name
         << " <prefix-file> <words-file> [options]\n\n"
         << "Read candidate hostnames from stdin and print the lines that start with:\n"
         << "  prefix from <prefix-file> + one or more matching segments\n\n"
         << "Segments are matched left-to-right from the start of each line.\n"
         << "They come from <words-file>, or can be digits if --numbers is enabled.\n\n"
         << "Options:\n"
         << "  --range=R1[,R2,...]\n"
         << "      Comma-separated segment length ranges. Example: 5-7,3-5,2-4\n"
         << "      With --chain=N, segment 1 uses R1, segment 2 uses R2, and once the\n"
         << "      range list runs out, the last range is reused. Default: 3-unlimited\n"
         << "  --chain=N\n"
         << "      Require exactly N matched segments after the prefix. Default: 1\n"
         << "  --min-total-length=N\n"
         << "      Require the total matched length of prefix + all segments to be\n"
         << "      at least N characters\n"
         << "  --numbers\n"
         << "      Allow a segment to be digits only, in addition to dictionary words\n"
         << "  --onion-v3\n"
         << "      Restrict prefixes and segments to characters valid in v3 onion\n"
         << "      addresses: a-z and 2-7\n"
         << "  --separator\n"
         << "      Insert '+' between matched segments, including after the last one\n"
         << "  --color=auto|yes|no|multi\n"
         << "      auto: single-color highlight on terminals only\n"
         << "      yes: always highlight the full matched prefix+segment span in red\n"
         << "      no: plain output with no color\n"
         << "      multi: color prefix and each segment separately\n"
         << "  --help\n"
         << "      Show this help message\n\n"
         << "Example:\n"
         << "  ls ./onions/ | " << program_name
         << " prefix.txt words_alpha.txt --range=5-7,3-5 --chain=2 --separator\n";
}

// Parse command-line arguments into the Options struct. The parser is designed
// to stay strict: anything unknown or malformed is reported immediately.
bool parse_arguments(int argc, char* argv[], Options& options) {
    if (argc == 2 && string(argv[1]) == "--help") {
        print_usage(argv[0]);
        exit(0);
    }

    if (argc < 3) {
        print_usage(argv[0]);
        return false;
    }

    options.prefix_file_path = argv[1];
    options.words_file_path = argv[2];

    for (int i = 3; i < argc; ++i) {
        const string arg = argv[i];

        if (arg == "--help") {
            print_usage(argv[0]);
            exit(0);
        }

        switch (parse_color_mode(arg, options.color_mode)) {
            case ParseStatus::kSuccess:
                continue;
            case ParseStatus::kInvalid:
                cerr << "Invalid color mode: " << arg << '\n';
                return false;
            case ParseStatus::kNoMatch:
                break;
        }

        if (arg == "--numbers") {
            options.allow_numbers = true;
            continue;
        }

        if (arg == "--separator") {
            options.use_separator = true;
            continue;
        }

        switch (parse_size_option(arg, "--min-total-length=", options.min_total_length)) {
            case ParseStatus::kSuccess:
                continue;
            case ParseStatus::kInvalid:
                cerr << "Invalid minimum total length: " << arg << '\n';
                return false;
            case ParseStatus::kNoMatch:
                break;
        }

        if (arg == "--onion-v3") {
            options.strict_v3 = true;
            continue;
        }

        vector<LengthRange> parsed_ranges;
        switch (parse_range_list_option(arg, "--range=", parsed_ranges)) {
            case ParseStatus::kSuccess:
                options.ranges = move(parsed_ranges);
                continue;
            case ParseStatus::kInvalid:
                cerr << "Invalid range: " << arg << '\n';
                return false;
            case ParseStatus::kNoMatch:
                break;
        }

        switch (parse_size_option(arg, "--chain=", options.chain_length, true)) {
            case ParseStatus::kSuccess:
                continue;
            case ParseStatus::kInvalid:
                cerr << "Invalid chain length: " << arg << '\n';
                return false;
            case ParseStatus::kNoMatch:
                break;
        }

        if (starts_with(arg, "--")) {
            cerr << "Unknown option: " << arg << '\n';
            return false;
        }

        cerr << "Unexpected positional argument: " << arg << '\n';
        return false;
    }

    return true;
}

// Load a list of non-empty lines, optionally filtering out values that are
// incompatible with v3 onion characters. Returned lines are sorted and deduped
// so the downstream matching code can assume a clean input set.
vector<string> load_unique_lines(istream& input, bool strict_v3) {
    vector<string> lines;
    string line;
    while (getline(input, line)) {
        line = trim_line_endings(move(line));
        if (line.empty() || (strict_v3 && !is_v3_compatible(line))) {
            continue;
        }
        lines.push_back(move(line));
    }

    sort(lines.begin(), lines.end());
    lines.erase(unique(lines.begin(), lines.end()), lines.end());
    return lines;
}

// Load the dictionary into a hash set for fast exact lookup and separately keep
// a deduplicated list of word lengths for cheap pre-filtering.
Dictionary load_dictionary(istream& input, bool strict_v3) {
    Dictionary dictionary;
    dictionary.words.reserve(400000);

    string line;
    while (getline(input, line)) {
        line = trim_line_endings(move(line));
        if (line.empty() || (strict_v3 && !is_v3_compatible(line))) {
            continue;
        }

        if (dictionary.words.insert(line).second) {
            dictionary.lengths.push_back(line.size());
        }
    }

    sort(dictionary.lengths.begin(), dictionary.lengths.end());
    dictionary.lengths.erase(
        unique(dictionary.lengths.begin(), dictionary.lengths.end()),
        dictionary.lengths.end());
    return dictionary;
}

// Build the allowed dictionary lengths for each configured segment range.
vector<vector<size_t>> build_range_lengths(
    const vector<LengthRange>& ranges,
    const vector<size_t>& word_lengths) {
    vector<vector<size_t>> range_lengths;
    range_lengths.reserve(ranges.size());
    for (const auto& range : ranges) {
        range_lengths.push_back(filter_lengths(word_lengths, range.min_len, range.max_len));
    }
    return range_lengths;
}

// Reconstruct the chosen segment boundaries from the memo table produced by
// find_best_chain().
vector<size_t> build_segment_ends(
    size_t prefix_end,
    size_t chain_length,
    const vector<vector<SearchState>>& memo) {
    vector<size_t> segment_ends;
    segment_ends.reserve(chain_length);

    size_t current_end = prefix_end;
    for (size_t chain_index = 0; chain_index < chain_length; ++chain_index) {
        const SearchState& step = memo[chain_index][current_end];
        current_end += step.next_length;
        segment_ends.push_back(current_end);
    }

    return segment_ends;
}

// Choose between two candidate line matches using the same ranking policy as
// the recursive search: farther right wins, then longer prefix wins.
bool is_better_match(const MatchResult& current,
                     size_t candidate_prefix_end,
                     const vector<size_t>& candidate_segment_ends) {
    if (!current.matched()) {
        return true;
    }
    if (candidate_segment_ends.back() != current.matched_end()) {
        return candidate_segment_ends.back() > current.matched_end();
    }
    return candidate_prefix_end > current.prefix_end;
}

// Check every prefix against one input line and keep only the best resulting
// match. A single memo table is reused because the search state depends only on
// position and chain index, not on which prefix led us there.
MatchResult find_best_match_for_line(
    const string& line,
    const vector<string>& prefixes,
    const vector<LengthRange>& ranges,
    const vector<vector<size_t>>& range_lengths,
    size_t chain_length,
    const unordered_set<string>& words,
    bool allow_numbers,
    bool strict_v3) {
    MatchResult best_match;
    vector<vector<SearchState>> memo(
        chain_length, vector<SearchState>(line.size() + 1));

    for (const auto& prefix : prefixes) {
        if (!starts_with(line, prefix)) {
            continue;
        }

        const SearchState& best_chain = find_best_chain(
            line,
            prefix.size(),
            0,
            ranges,
            range_lengths,
            chain_length,
            words,
            allow_numbers,
            strict_v3,
            memo);
        if (!best_chain.matched) {
            continue;
        }

        vector<size_t> segment_ends =
            build_segment_ends(prefix.size(), chain_length, memo);
        if (!is_better_match(best_match, prefix.size(), segment_ends)) {
            continue;
        }

        best_match.prefix_end = prefix.size();
        best_match.segment_ends = move(segment_ends);
    }

    return best_match;
}

// Produce the exact string that will be printed. When --separator is enabled,
// this inserts '+' markers between matched pieces and recomputes the segment
// end positions to match the rendered text rather than the source line.
RenderedMatch render_match(const string& line,
                           const MatchResult& match,
                           bool use_separator) {
    RenderedMatch rendered{line, match.prefix_end, match.matched_end(), match.segment_ends};
    if (!use_separator) {
        return rendered;
    }

    rendered.line.clear();
    rendered.line.append(line, 0, match.prefix_end);

    size_t segment_start = match.prefix_end;
    for (size_t segment_end : match.segment_ends) {
        rendered.line.push_back('+');
        rendered.line.append(line, segment_start, segment_end - segment_start);
        segment_start = segment_end;
    }
    rendered.line.push_back('+');
    rendered.line.append(line, match.matched_end(), string::npos);

    // Translate segment boundaries from the original line into positions in the
    // separator-augmented output string.
    rendered.segment_ends.clear();
    size_t rendered_end = match.prefix_end;
    for (size_t i = 0; i < match.segment_ends.size(); ++i) {
        rendered_end += 1;
        rendered_end += match.segment_ends[i] -
                        (i == 0 ? match.prefix_end : match.segment_ends[i - 1]);
        rendered.segment_ends.push_back(rendered_end);
    }
    rendered.matched_end = rendered_end + 1;

    return rendered;
}

// Multi-color mode colors the prefix and each segment independently while
// leaving the optional '+' separators uncolored.
void print_multi_color_match(const RenderedMatch& rendered, bool use_separator) {
    cout << kPrefixColor
         << rendered.line.substr(0, rendered.prefix_end)
         << kColorEnd;

    size_t segment_start = rendered.prefix_end;
    if (use_separator) {
        cout << '+';
        ++segment_start;
    }

    for (size_t i = 0; i < rendered.segment_ends.size(); ++i) {
        const char* segment_color =
            kSuffixColors[i % (sizeof(kSuffixColors) / sizeof(kSuffixColors[0]))];
        cout << segment_color
             << rendered.line.substr(segment_start,
                                     rendered.segment_ends[i] - segment_start)
             << kColorEnd;
        segment_start = rendered.segment_ends[i];
        if (use_separator) {
            cout << '+';
            ++segment_start;
        }
    }

    cout << rendered.line.substr(segment_start) << '\n';
}

// Single-color mode highlights the matched pieces using one color. When
// separators are enabled, the '+' characters themselves remain uncolored.
void print_single_color_match(const RenderedMatch& rendered, bool use_separator) {
    if (!use_separator) {
        cout << kSingleColor
             << rendered.line.substr(0, rendered.matched_end)
             << kColorEnd
             << rendered.line.substr(rendered.matched_end)
             << '\n';
        return;
    }

    cout << kSingleColor
         << rendered.line.substr(0, rendered.prefix_end)
         << kColorEnd;

    size_t segment_start = rendered.prefix_end + 1;
    for (size_t segment_end : rendered.segment_ends) {
        cout << '+'
             << kSingleColor
             << rendered.line.substr(segment_start, segment_end - segment_start)
             << kColorEnd;
        segment_start = segment_end + 1;
    }

    cout << '+' << rendered.line.substr(rendered.matched_end) << '\n';
}

// Central output helper shared by main().
void print_match(const string& line,
                 const MatchResult& match,
                 bool use_separator,
                 bool use_color,
                 bool use_multi_color) {
    const RenderedMatch rendered = render_match(line, match, use_separator);
    if (!use_color) {
        cout << rendered.line << '\n';
        return;
    }

    if (use_multi_color) {
        print_multi_color_match(rendered, use_separator);
        return;
    }

    print_single_color_match(rendered, use_separator);
}

}  // namespace

int main(int argc, char* argv[]) {
    // Step 1: parse CLI options.
    Options options;
    if (!parse_arguments(argc, argv, options)) {
        return 1;
    }

    // Step 2: open the prefix and dictionary files.
    ifstream prefix_file(options.prefix_file_path);
    if (!prefix_file) {
        cerr << "Failed to open prefix file: " << options.prefix_file_path << '\n';
        return 1;
    }

    ifstream words_file(options.words_file_path);
    if (!words_file) {
        cerr << "Failed to open words file: " << options.words_file_path << '\n';
        return 1;
    }

    // Step 3: sanity-check configured ranges.
    for (const auto& range : options.ranges) {
        if (range.min_len == 0 || range.min_len > range.max_len) {
            cerr << "Invalid range configuration\n";
            return 1;
        }
    }

    // Step 4: load prefixes and dictionary data.
    const vector<string> prefixes =
        load_unique_lines(prefix_file, options.strict_v3);
    if (prefixes.empty()) {
        return 0;
    }

    const Dictionary dictionary = load_dictionary(words_file, options.strict_v3);
    if (dictionary.words.empty() && !options.allow_numbers) {
        return 0;
    }

    const vector<vector<size_t>> range_lengths =
        build_range_lengths(options.ranges, dictionary.lengths);

    // Step 5: decide whether output should contain ANSI colors.
    const bool stdout_is_terminal = isatty(STDOUT_FILENO);
    const bool use_color = options.color_mode == ColorMode::kYes ||
                           options.color_mode == ColorMode::kMulti ||
                           (options.color_mode == ColorMode::kAuto && stdout_is_terminal);
    const bool use_multi_color = options.color_mode == ColorMode::kMulti;

    // Step 6: stream candidate lines from stdin, keep only matches, and print
    // them in the requested format.
    string line;
    while (getline(cin, line)) {
        line = trim_line_endings(move(line));

        const MatchResult match = find_best_match_for_line(
            line,
            prefixes,
            options.ranges,
            range_lengths,
            options.chain_length,
            dictionary.words,
            options.allow_numbers,
            options.strict_v3);
        if (!match.matched() || match.matched_end() < options.min_total_length) {
            continue;
        }

        print_match(
            line, match, options.use_separator, use_color, use_multi_color);
    }

    return 0;
}
