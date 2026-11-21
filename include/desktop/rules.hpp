#pragma once

#include <charconv>
#include <cstdint>
#include <regex.h>
#include <string>
#include <string_view>
#include <vector>

namespace desktop {

inline constexpr size_t maxRules = 16;
inline constexpr size_t maxRuleName = 128;
inline constexpr const char *rulesHeader = "# audiox desktop routes v1\n";
inline constexpr const char *virtualChannelsOff = "# virtual-channels off\n";

inline bool virtualChannelsEnabled(std::string_view input) {
    const std::string_view header(rulesHeader);
    return input.substr(header.size(), std::string_view(virtualChannelsOff).size()) != virtualChannelsOff;
}

struct Rule {
    bool capture{};
    bool appRegex{};
    bool targetRegex{};
    unsigned pair{};
    std::string app, target;
};

inline std::string hexEncode(std::string_view input) {
    constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.reserve(input.size() * 2);
    for (unsigned char c : input) {
        output += digits[c >> 4];
        output += digits[c & 15];
    }
    return output;
}

inline bool hexDecode(std::string_view input, std::string &output) {
    if (input.empty() || input.size() > maxRuleName * 2 || input.size() % 2) return false;
    std::string result;
    for (size_t i = 0; i < input.size(); i += 2) {
        auto digit = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        int hi = digit(input[i]), lo = digit(input[i + 1]);
        if (hi < 0 || lo < 0 || (hi == 0 && lo == 0)) return false;
        result += static_cast<char>((hi << 4) | lo);
    }
    output.swap(result);
    return true;
}

inline bool validRegex(const std::string &pattern) {
    regex_t compiled{};
    if (regcomp(&compiled, pattern.c_str(), REG_EXTENDED | REG_NOSUB) != 0) return false;
    regfree(&compiled);
    return true;
}

inline bool validRule(const Rule &rule) {
    return !rule.app.empty() && rule.app.size() <= maxRuleName &&
        rule.app.find('\0') == std::string::npos && !rule.target.empty() &&
        rule.target.size() <= maxRuleName && rule.target.find('\0') == std::string::npos &&
        rule.pair < 16 && (!rule.appRegex || validRegex(rule.app)) &&
        (!rule.targetRegex || validRegex(rule.target));
}

inline std::string ruleLine(const Rule &rule) {
    return std::string(rule.capture ? "c\t" : "p\t") +
        (rule.appRegex ? "r\t" : "e\t") + (rule.targetRegex ? "r\t" : "e\t") +
        std::to_string(rule.pair) + "\t" + hexEncode(rule.app) + "\t" + hexEncode(rule.target) + "\n";
}

inline std::string rulesText(const std::vector<Rule> &rules) {
    std::string output = rulesHeader;
    for (const auto &rule : rules) output += ruleLine(rule);
    return output;
}

inline bool parseRules(std::string_view input, std::vector<Rule> &rules) {
    const std::string_view header(rulesHeader);
    if (input.substr(0, header.size()) != header) return false;
    input.remove_prefix(header.size());
    if (input.substr(0, std::string_view(virtualChannelsOff).size()) == virtualChannelsOff)
        input.remove_prefix(std::string_view(virtualChannelsOff).size());
    std::vector<Rule> parsed;
    while (!input.empty()) {
        const size_t end = input.find('\n');
        if (end == std::string_view::npos || parsed.size() == maxRules) return false;
        std::string_view line = input.substr(0, end);
        input.remove_prefix(end + 1);
        std::string_view fields[6];
        for (size_t i = 0; i < 5; ++i) {
            const size_t tab = line.find('\t');
            if (tab == std::string_view::npos) return false;
            fields[i] = line.substr(0, tab);
            line.remove_prefix(tab + 1);
        }
        fields[5] = line;
        Rule rule;
        if (fields[0] == "c") rule.capture = true;
        else if (fields[0] != "p") return false;
        if (fields[1] == "r") rule.appRegex = true;
        else if (fields[1] != "e") return false;
        if (fields[2] == "r") rule.targetRegex = true;
        else if (fields[2] != "e") return false;
        const auto number = std::from_chars(fields[3].data(), fields[3].data() + fields[3].size(), rule.pair);
        if (number.ec != std::errc{} || number.ptr != fields[3].data() + fields[3].size() ||
            !hexDecode(fields[4], rule.app) || !hexDecode(fields[5], rule.target) ||
            !validRule(rule)) return false;
        parsed.push_back(std::move(rule));
    }
    rules.swap(parsed);
    return true;
}

inline bool ruleMatches(const std::string &pattern, bool regex, const std::string &value) {
    if (!regex) return pattern == value;
    regex_t compiled{};
    if (regcomp(&compiled, pattern.c_str(), REG_EXTENDED | REG_NOSUB) != 0) return false;
    const bool match = regexec(&compiled, value.c_str(), 0, nullptr, 0) == 0;
    regfree(&compiled);
    return match;
}

} // namespace desktop
