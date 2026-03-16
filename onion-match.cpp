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

std::string trim_line_endings(std::string line) {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
    }
    return line;
}

bool starts_with(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() &&
           text.compare(0, prefix.size(), prefix) == 0;
}

bool parse_size_value(const std::string& text, std::size_t& value) {
    if (text.empty()) {
        return false;
    }

    value = 0;
    for (unsigned char ch : text) {
        if (!std::isdigit(ch)) {
            return false;
        }

        const std::size_t digit = static_cast<std::size_t>(ch - '0');
        if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }

    return true;
}

bool parse_length_filter(const std::string& text,
                         std::size_t& min_len,
                         std::size_t& max_len) {
    const std::size_t dash = text.find('-');
    if (dash == std::string::npos) {
        return parse_size_value(text, min_len) &&
               (max_len = min_len, true) &&
               min_len > 0;
    }

    if (text.find('-', dash + 1) != std::string::npos) {
        return false;
    }

    std::string left = text.substr(0, dash);
    std::string right = text.substr(dash + 1);

    if (!parse_size_value(left, min_len) || !parse_size_value(right, max_len)) {
        return false;
    }

    return min_len > 0 && min_len <= max_len;
}

std::vector<std::size_t> filter_lengths(const std::vector<std::size_t>& lengths,
                                        std::size_t min_len,
                                        std::size_t max_len) {
    std::vector<std::size_t> filtered;
    for (std::size_t len : lengths) {
        if (len >= min_len && len <= max_len) {
            filtered.push_back(len);
        }
    }
    return filtered;
}

bool is_all_digits(const std::string& text,
                   std::size_t start,
                   std::size_t length) {
    for (std::size_t i = 0; i < length; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[start + i]))) {
            return false;
        }
    }
    return true;
}

bool is_v3_char(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= '2' && ch <= '7');
}

bool is_v3_compatible(const std::string& text) {
    for (char ch : text) {
        if (!is_v3_char(ch)) {
            return false;
        }
    }
    return true;
}

struct LengthRange {
    std::size_t min_len = 1;
    std::size_t max_len = std::numeric_limits<std::size_t>::max();
};

bool matches_segment(const std::string& text,
                     std::size_t start,
                     std::size_t length,
                     const std::unordered_set<std::string>& words,
                     const std::vector<std::size_t>& word_lengths,
                     bool allow_numbers,
                     bool strict_v3) {
    const std::string candidate = text.substr(start, length);
    if (strict_v3 && !is_v3_compatible(candidate)) {
        return false;
    }

    if (allow_numbers && is_all_digits(text, start, length)) {
        return true;
    }

    return std::binary_search(word_lengths.begin(), word_lengths.end(), length) &&
           words.find(candidate) != words.end();
}

struct SearchState {
    bool computed = false;
    bool matched = false;
    std::size_t next_length = 0;
    std::size_t final_end = 0;
};

const SearchState& find_best_chain(
    const std::string& text,
    std::size_t start,
    std::size_t chain_index,
    const std::vector<LengthRange>& ranges,
    const std::vector<std::vector<std::size_t>>& range_lengths,
    std::size_t chain_length,
    const std::unordered_set<std::string>& words,
    bool allow_numbers,
    bool strict_v3,
    std::vector<std::vector<SearchState>>& memo) {
    SearchState& state = memo[chain_index][start];
    if (state.computed) {
        return state;
    }

    state.computed = true;

    const std::size_t range_index = std::min(chain_index, ranges.size() - 1);
    const LengthRange& range = ranges[range_index];
    const std::vector<std::size_t>& allowed_lengths = range_lengths[range_index];
    const std::size_t available = text.size() - start;
    const std::size_t limit = std::min(range.max_len, available);
    if (limit < range.min_len) {
        return state;
    }

    for (std::size_t len = limit; len >= range.min_len; --len) {
        if (!matches_segment(
                text, start, len, words, allowed_lengths, allow_numbers, strict_v3)) {
            if (len == range.min_len) {
                break;
            }
            continue;
        }

        std::size_t final_end = start + len;
        if (chain_index + 1 < chain_length) {
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

enum class ColorMode {
    kAuto,
    kYes,
    kNo,
    kMulti,
};

struct Options {
    std::string prefix_file_path;
    std::string words_file_path;
    std::vector<LengthRange> ranges{
        LengthRange{3, std::numeric_limits<std::size_t>::max()}};
    std::size_t chain_length = 1;
    std::size_t min_total_length = 0;
    ColorMode color_mode = ColorMode::kAuto;
    bool use_separator = false;
    bool allow_numbers = false;
    bool strict_v3 = false;
};

bool parse_color_mode(const std::string& text, ColorMode& mode) {
    if (!starts_with(text, "--color=")) {
        return false;
    }

    const std::string value = text.substr(8);
    if (value == "auto") {
        mode = ColorMode::kAuto;
        return true;
    }
    if (value == "yes") {
        mode = ColorMode::kYes;
        return true;
    }
    if (value == "no") {
        mode = ColorMode::kNo;
        return true;
    }
    if (value == "multi") {
        mode = ColorMode::kMulti;
        return true;
    }

    return false;
}

bool parse_chain_length_argument(const std::string& text, std::size_t& chain_length) {
    if (!starts_with(text, "--chain=")) {
        return false;
    }
    return parse_size_value(text.substr(8), chain_length) && chain_length > 0;
}

bool parse_min_total_length_argument(const std::string& text,
                                     std::size_t& min_total_length) {
    if (!starts_with(text, "--min-total-length=")) {
        return false;
    }
    return parse_size_value(text.substr(19), min_total_length);
}

void print_usage(const char* program_name) {
    std::cerr << "Usage: " << program_name
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

bool parse_range_argument(const std::string& text,
                          LengthRange& range) {
    return parse_length_filter(text, range.min_len, range.max_len);
}

bool parse_range_list_argument(const std::string& text,
                               std::vector<LengthRange>& ranges) {
    ranges.clear();

    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        const std::string token = text.substr(start, comma - start);
        LengthRange range;
        if (!parse_range_argument(token, range)) {
            return false;
        }
        ranges.push_back(range);

        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }

    return !ranges.empty();
}

bool parse_prefixed_range_list_argument(const std::string& text,
                                        const std::string& flag_name,
                                        std::vector<LengthRange>& ranges) {
    if (!starts_with(text, flag_name)) {
        return false;
    }
    return parse_range_list_argument(text.substr(flag_name.size()), ranges);
}

bool parse_arguments(int argc, char* argv[], Options& options) {
    if (argc == 2 && std::string(argv[1]) == "--help") {
        print_usage(argv[0]);
        std::exit(0);
    }

    if (argc < 3) {
        print_usage(argv[0]);
        return false;
    }

    options.prefix_file_path = argv[1];
    options.words_file_path = argv[2];

    std::vector<std::string> legacy_range_args;
    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--help") {
            print_usage(argv[0]);
            std::exit(0);
        }

        ColorMode parsed_color_mode;
        if (parse_color_mode(arg, parsed_color_mode)) {
            options.color_mode = parsed_color_mode;
            continue;
        }

        if (arg == "--numbers") {
            options.allow_numbers = true;
            continue;
        }

        if (arg == "--separator") {
            options.use_separator = true;
            continue;
        }

        std::size_t min_total_length = 0;
        if (parse_min_total_length_argument(arg, min_total_length)) {
            options.min_total_length = min_total_length;
            continue;
        }

        if (starts_with(arg, "--min-total-length=")) {
            std::cerr << "Invalid minimum total length: " << arg << '\n';
            return false;
        }

        if (arg == "--onion-v3") {
            options.strict_v3 = true;
            continue;
        }

        std::vector<LengthRange> ranges;
        if (parse_prefixed_range_list_argument(arg, "--range=", ranges)) {
            options.ranges = std::move(ranges);
            continue;
        }

        if (starts_with(arg, "--range=")) {
            std::cerr << "Invalid range: " << arg << '\n';
            return false;
        }

        std::size_t chain_length = 0;
        if (parse_chain_length_argument(arg, chain_length)) {
            options.chain_length = chain_length;
            continue;
        }

        if (starts_with(arg, "--chain=")) {
            std::cerr << "Invalid chain length: " << arg << '\n';
            return false;
        }

        if (starts_with(arg, "--color=")) {
            std::cerr << "Invalid color mode: " << arg << '\n';
            return false;
        }

        if (starts_with(arg, "--")) {
            std::cerr << "Unknown option: " << arg << '\n';
            return false;
        }

        legacy_range_args.push_back(arg);
    }

    if (!legacy_range_args.empty()) {
        if (legacy_range_args.size() > 1) {
            std::cerr << "Too many positional ranges provided\n";
            return false;
        }

        if (!parse_range_list_argument(legacy_range_args[0], options.ranges)) {
            std::cerr << "Invalid word length filter: " << legacy_range_args[0] << '\n';
            return false;
        }
    }

    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    Options options;
    if (!parse_arguments(argc, argv, options)) {
        return 1;
    }

    std::ifstream prefix_file(options.prefix_file_path);
    if (!prefix_file) {
        std::cerr << "Failed to open prefix file: " << options.prefix_file_path << '\n';
        return 1;
    }

    std::ifstream words_file(options.words_file_path);
    if (!words_file) {
        std::cerr << "Failed to open words file: " << options.words_file_path << '\n';
        return 1;
    }

    const std::vector<LengthRange>& ranges = options.ranges;
    const std::size_t chain_length = options.chain_length;
    const std::size_t min_total_length = options.min_total_length;
    const bool use_separator = options.use_separator;
    const bool allow_numbers = options.allow_numbers;
    const bool strict_v3 = options.strict_v3;
    const ColorMode color_mode = options.color_mode;

    for (const auto& range : ranges) {
        if (range.min_len == 0 || range.min_len > range.max_len) {
            std::cerr << "Invalid range configuration\n";
            return 1;
        }
    }

    std::vector<std::string> prefixes;
    std::string line;
    while (std::getline(prefix_file, line)) {
        line = trim_line_endings(std::move(line));
        if (!line.empty() && (!strict_v3 || is_v3_compatible(line))) {
            prefixes.push_back(std::move(line));
        }
    }

    if (prefixes.empty()) {
        return 0;
    }

    std::sort(prefixes.begin(), prefixes.end());
    prefixes.erase(std::unique(prefixes.begin(), prefixes.end()), prefixes.end());

    std::unordered_set<std::string> words;
    words.reserve(400000);

    std::vector<std::size_t> all_lengths;
    while (std::getline(words_file, line)) {
        line = trim_line_endings(std::move(line));
        if (line.empty()) {
            continue;
        }

        if (strict_v3 && !is_v3_compatible(line)) {
            continue;
        }

        if (words.insert(line).second) {
            all_lengths.push_back(line.size());
        }
    }

    if (words.empty() && !allow_numbers) {
        return 0;
    }

    std::sort(all_lengths.begin(), all_lengths.end());
    all_lengths.erase(std::unique(all_lengths.begin(), all_lengths.end()),
                      all_lengths.end());

    std::vector<std::vector<std::size_t>> range_lengths;
    range_lengths.reserve(ranges.size());
    for (const auto& range : ranges) {
        range_lengths.push_back(filter_lengths(all_lengths, range.min_len, range.max_len));
    }

    const bool stdout_is_terminal = isatty(STDOUT_FILENO);
    const bool use_color = color_mode == ColorMode::kYes ||
                           color_mode == ColorMode::kMulti ||
                           (color_mode == ColorMode::kAuto && stdout_is_terminal);
    const bool use_multi_color = color_mode == ColorMode::kMulti;
    constexpr const char* kPrefixColor = "\033[1;31m";
    constexpr const char* kSuffixColors[] = {
        "\033[1;34m",
        "\033[1;33m",
        "\033[1;35m",
        "\033[1;36m",
    };
    constexpr const char* kSingleColor = "\033[1;31m";
    constexpr const char* kColorEnd = "\033[0m";

    while (std::getline(std::cin, line)) {
        line = trim_line_endings(std::move(line));

        bool matched = false;
        std::size_t matched_prefix_end = 0;
        std::vector<std::size_t> matched_segment_ends;
        std::vector<std::vector<SearchState>> memo(
            chain_length, std::vector<SearchState>(line.size() + 1));
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

            std::size_t current_end = prefix.size();
            std::vector<std::size_t> segment_ends;
            segment_ends.reserve(chain_length);
            for (std::size_t chain_index = 0; chain_index < chain_length; ++chain_index) {
                const SearchState& step = memo[chain_index][current_end];
                current_end += step.next_length;
                segment_ends.push_back(current_end);
            }

            matched = true;
            if (matched_segment_ends.empty() ||
                segment_ends.back() > matched_segment_ends.back() ||
                (segment_ends.back() == matched_segment_ends.back() &&
                 prefix.size() > matched_prefix_end)) {
                matched_prefix_end = prefix.size();
                matched_segment_ends = segment_ends;
            }
        }

        if (matched) {
            const std::size_t matched_end = matched_segment_ends.back();
            if (matched_end < min_total_length) {
                continue;
            }

            std::string output_line = line;
            std::size_t rendered_matched_end = matched_end;
            std::vector<std::size_t> rendered_segment_ends = matched_segment_ends;
            if (use_separator) {
                output_line.clear();
                output_line.append(line, 0, matched_prefix_end);

                std::size_t segment_start = matched_prefix_end;
                for (std::size_t i = 0; i < matched_segment_ends.size(); ++i) {
                    output_line.push_back('+');
                    output_line.append(
                        line, segment_start, matched_segment_ends[i] - segment_start);
                    segment_start = matched_segment_ends[i];
                }
                output_line.push_back('+');

                output_line.append(line, matched_end, std::string::npos);

                rendered_segment_ends.clear();
                std::size_t rendered_end = matched_prefix_end;
                for (std::size_t i = 0; i < matched_segment_ends.size(); ++i) {
                    rendered_end += 1;
                    rendered_end += matched_segment_ends[i] -
                                    (i == 0 ? matched_prefix_end
                                            : matched_segment_ends[i - 1]);
                    rendered_segment_ends.push_back(rendered_end);
                }
                rendered_matched_end = rendered_end + 1;
            }

            if (use_color) {
                if (use_multi_color) {
                    const std::string& rendered_line = use_separator ? output_line : line;
                    const std::size_t rendered_prefix_end = matched_prefix_end;
                    if (!use_separator) {
                        rendered_segment_ends = matched_segment_ends;
                    }

                    std::cout << kPrefixColor
                              << rendered_line.substr(0, rendered_prefix_end)
                              << kColorEnd;

                    std::size_t segment_start =
                        use_separator ? rendered_prefix_end + 1 : rendered_prefix_end;
                    for (std::size_t i = 0; i < rendered_segment_ends.size(); ++i) {
                        const char* segment_color =
                            kSuffixColors[i % (sizeof(kSuffixColors) / sizeof(kSuffixColors[0]))];
                        if (use_separator) {
                            std::cout << '+';
                        }
                        std::cout << segment_color
                                  << rendered_line.substr(segment_start,
                                                          rendered_segment_ends[i] - segment_start)
                                  << kColorEnd;
                        segment_start = rendered_segment_ends[i];
                        if (use_separator) {
                            ++segment_start;
                        }
                    }

                    if (use_separator) {
                        std::cout << '+';
                    }
                    std::cout << rendered_line.substr(segment_start) << '\n';
                } else {
                    if (use_separator) {
                        std::cout << kSingleColor
                                  << output_line.substr(0, matched_prefix_end)
                                  << kColorEnd;

                        std::size_t segment_start = matched_prefix_end;
                        for (std::size_t i = 0; i < matched_segment_ends.size(); ++i) {
                            std::cout << '+'
                                      << kSingleColor
                                      << line.substr(segment_start,
                                                     matched_segment_ends[i] - segment_start)
                                      << kColorEnd;
                            segment_start = matched_segment_ends[i];
                        }

                        std::cout << '+'
                                  << output_line.substr(rendered_matched_end) << '\n';
                    } else {
                        std::cout << kSingleColor
                                  << line.substr(0, matched_end)
                                  << kColorEnd
                                  << line.substr(matched_end)
                                  << '\n';
                    }
                }
            } else {
                std::cout << (use_separator ? output_line : line) << '\n';
            }
        }
    }

    return 0;
}
