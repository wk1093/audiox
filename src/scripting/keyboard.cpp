#include "scripting/keyboard.hpp"

#include <cstdlib>
#include <cstring>

std::string scriptHexEncode(const char *text) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string hex;
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text);
       *p; ++p) {
    hex += digits[*p >> 4];
    hex += digits[*p & 15];
  }
  return hex;
}

bool scriptHexDecode(const char *hex, char *text, size_t capacity) {
  size_t length = strlen(hex);
  if (!capacity || length % 2 || length / 2 >= capacity)
    return false;
  auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < length; i += 2) {
    int hi = digit(hex[i]), lo = digit(hex[i + 1]);
    if (hi < 0 || lo < 0 || (hi == 0 && lo == 0))
      return false;
    text[i / 2] = static_cast<char>((hi << 4) | lo);
  }
  text[length / 2] = '\0';
  return true;
}

bool scriptChordReport(const char *keys, uint8_t (&report)[8],
                       bool allowModifiers) {
  memset(report, 0, sizeof(report));
  if (!keys || !*keys || strlen(keys) >= 64)
    return false;
  char copy[64];
  strcpy(copy, keys);
  unsigned used = 2;
  char *start = copy;
  while (start) {
    char *end = strchr(start, '+');
    if (end)
      *end++ = '\0';
    for (char *p = start; *p; ++p) {
      if (*p >= 'a' && *p <= 'z')
        *p -= 'a' - 'A';
      if (!(*p >= 'A' && *p <= 'Z') && !(*p >= '0' && *p <= '9'))
        return false;
    }
    uint8_t key = 0;
    if (strcmp(start, "CTRL") == 0)
      report[0] |= 1;
    else if (strcmp(start, "SHIFT") == 0)
      report[0] |= 2;
    else if (strcmp(start, "ALT") == 0)
      report[0] |= 4;
    else if (strcmp(start, "GUI") == 0)
      report[0] |= 8;
    else {
      if (strlen(start) == 1 && *start >= 'A' && *start <= 'Z')
        key = 4 + *start - 'A';
      else if (strlen(start) == 1 && *start >= '1' && *start <= '9')
        key = 30 + *start - '1';
      else if (strcmp(start, "0") == 0)
        key = 39;
      else if (*start == 'F') {
        char *tail = nullptr;
        long f = strtol(start + 1, &tail, 10);
        if (*tail == '\0' && f >= 1 && f <= 24)
          key = f <= 12 ? 57 + f : 91 + f;
      }
      struct NamedKey {
        const char *name;
        uint8_t code;
      };
      static constexpr NamedKey names[] = {
          {"ENTER", 40},  {"ESC", 41},      {"BACKSPACE", 42}, {"TAB", 43},
          {"SPACE", 44},  {"DELETE", 76},   {"RIGHT", 79},     {"LEFT", 80},
          {"DOWN", 81},   {"UP", 82},       {"HOME", 74},      {"END", 77},
          {"PAGEUP", 75}, {"PAGEDOWN", 78},
      };
      for (const auto &name : names)
        if (strcmp(start, name.name) == 0)
          key = name.code;
      if (!key || used == 8)
        return false;
      for (unsigned i = 2; i < used; ++i)
        if (report[i] == key)
          return false;
      report[used++] = key;
    }
    start = end;
  }
  return used > 2 || (allowModifiers && report[0]);
}

bool scriptAsciiReport(char character, uint8_t (&report)[8]) {
  memset(report, 0, sizeof(report));
  if (character >= 'a' && character <= 'z')
    report[2] = 4 + character - 'a';
  else if (character >= 'A' && character <= 'Z') {
    report[0] = 2;
    report[2] = 4 + character - 'A';
  } else if (character >= '1' && character <= '9')
    report[2] = 30 + character - '1';
  else {
    struct AsciiKey {
      char plain;
      char shifted;
      uint8_t usage;
    };
    static constexpr AsciiKey keys[] = {
        {'0', ')', 39},  {'1', '!', 30}, {'2', '@', 31},  {'3', '#', 32},
        {'4', '$', 33},  {'5', '%', 34}, {'6', '^', 35},  {'7', '&', 36},
        {'8', '*', 37},  {'9', '(', 38}, {'-', '_', 45},  {'=', '+', 46},
        {'[', '{', 47},  {']', '}', 48}, {'\\', '|', 49}, {';', ':', 51},
        {'\'', '"', 52}, {'`', '~', 53}, {',', '<', 54},  {'.', '>', 55},
        {'/', '?', 56},
    };
    if (character == ' ')
      report[2] = 44;
    else if (character == '\n')
      report[2] = 40;
    else if (character == '\t')
      report[2] = 43;
    else
      for (const auto &key : keys) {
        if (character == key.plain || character == key.shifted) {
          report[0] = character == key.shifted ? 2 : 0;
          report[2] = key.usage;
          break;
        }
      }
  }
  return report[2] != 0;
}
