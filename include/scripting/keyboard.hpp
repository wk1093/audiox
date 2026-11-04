#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

bool scriptChordReport(const char *keys, uint8_t (&report)[8],
                       bool allowModifiers = false);
bool scriptAsciiReport(char character, uint8_t (&report)[8]);
std::string scriptHexEncode(const char *text);
bool scriptHexDecode(const char *hex, char *text, size_t capacity);

inline uint16_t scriptMediaUsage(const char *name) {
  if (!name)
    return 0;
  struct Key {
    const char *name;
    uint16_t usage;
  };
  static constexpr Key keys[] = {
      {"MEDIA_PLAY", 0xb0},        {"MEDIA_PAUSE", 0xb1},
      {"MEDIA_RECORD", 0xb2},      {"MEDIA_FAST_FORWARD", 0xb3},
      {"MEDIA_REWIND", 0xb4},      {"MEDIA_NEXT", 0xb5},
      {"MEDIA_PREVIOUS", 0xb6},    {"MEDIA_STOP", 0xb7},
      {"MEDIA_EJECT", 0xb8},       {"MEDIA_PLAY_PAUSE", 0xcd},
      {"MEDIA_MUTE", 0xe2},        {"MEDIA_VOLUME_UP", 0xe9},
      {"MEDIA_VOLUME_DOWN", 0xea},
  };
  for (const auto &key : keys) {
    size_t i = 0;
    for (; name[i] && key.name[i]; ++i) {
      const char c =
          name[i] >= 'a' && name[i] <= 'z' ? name[i] - ('a' - 'A') : name[i];
      if (c != key.name[i])
        break;
    }
    if (!name[i] && !key.name[i])
      return key.usage;
  }
  return 0;
}

inline constexpr uint8_t SCRIPT_KEYBOARD_REPORT_ID = 1;
inline constexpr uint8_t SCRIPT_MEDIA_REPORT_ID = 2;
inline constexpr size_t SCRIPT_HID_REPORT_LENGTH = 9;
inline constexpr uint8_t SCRIPT_HID_DESCRIPTOR[] = {
    0x05,0x01,0x09,0x06,0xa1,0x01,0x85,SCRIPT_KEYBOARD_REPORT_ID,
    0x05,0x07,0x19,0xe0,0x29,0xe7,
    0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x08,0x81,0x02,
    0x95,0x01,0x75,0x08,0x81,0x01,
    0x95,0x06,0x75,0x08,0x15,0x00,0x25,0x73,0x19,0x00,0x29,0x73,0x81,0x00,0xc0,
    0x05,0x0c,0x09,0x01,0xa1,0x01,0x85,SCRIPT_MEDIA_REPORT_ID,
    0x15,0x00,0x26,0xff,0x03,0x19,0x00,0x2a,0xff,0x03,
    0x75,0x10,0x95,0x01,0x81,0x00,0xc0,
};
