#pragma once

#include <cstddef>
#include <cstring>

namespace audiox {

struct DiagnosticLogSummary {
    unsigned lines = 0;
    unsigned errors = 0;
    unsigned warnings = 0;
    unsigned xruns = 0;
};

// Never export original log text: it can contain scripts, device names or keys.
inline DiagnosticLogSummary summarizeDiagnosticLogs(const char *text, size_t length) {
    DiagnosticLogSummary result{};
    const char *line = text;
    const char *end = text + length;
    while (line < end) {
        const char *newline = static_cast<const char *>(memchr(line, '\n', end - line));
        const char *stop = newline ? newline : end;
        ++result.lines;
        const auto contains = [&](const char *word) {
            const size_t size = strlen(word);
            for (const char *p = line; p + size <= stop; ++p)
                if (memcmp(p, word, size) == 0) return true;
            return false;
        };
        if (contains("[ERR]") || contains("[CRIT]") || contains("[ERROR]")) ++result.errors;
        if (contains("[WARN]")) ++result.warnings;
        if (contains("xrun") || contains("underrun") || contains("overrun")) ++result.xruns;
        line = newline ? newline + 1 : end;
    }
    return result;
}

}
