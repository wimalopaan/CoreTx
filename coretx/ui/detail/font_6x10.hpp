// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace coretx::ui::detail {

// Five ink columns and nine ink rows in a 6x10 cell.
constexpr std::array<std::uint8_t, 9> glyph(const char ch) noexcept {
    switch (ch) {
    case 'A':
        return {14, 17, 17, 17, 31, 17, 17, 17, 17};
    case 'B':
        return {30, 17, 17, 17, 30, 17, 17, 17, 30};
    case 'C':
        return {14, 17, 16, 16, 16, 16, 16, 17, 14};
    case 'D':
        return {30, 17, 17, 17, 17, 17, 17, 17, 30};
    case 'E':
        return {31, 16, 16, 16, 30, 16, 16, 16, 31};
    case 'F':
        return {31, 16, 16, 16, 30, 16, 16, 16, 16};
    case 'G':
        return {14, 17, 16, 16, 23, 17, 17, 17, 15};
    case 'H':
        return {17, 17, 17, 17, 31, 17, 17, 17, 17};
    case 'I':
        return {14, 4, 4, 4, 4, 4, 4, 4, 14};
    case 'J':
        return {7, 2, 2, 2, 2, 2, 18, 18, 12};
    case 'K':
        return {17, 17, 18, 20, 24, 20, 18, 17, 17};
    case 'L':
        return {16, 16, 16, 16, 16, 16, 16, 16, 31};
    case 'M':
        return {17, 27, 21, 21, 17, 17, 17, 17, 17};
    case 'N':
        return {17, 25, 25, 21, 21, 21, 19, 19, 17};
    case 'O':
        return {14, 17, 17, 17, 17, 17, 17, 17, 14};
    case 'P':
        return {30, 17, 17, 17, 30, 16, 16, 16, 16};
    case 'Q':
        return {14, 17, 17, 17, 17, 17, 21, 18, 13};
    case 'R':
        return {30, 17, 17, 17, 30, 20, 18, 17, 17};
    case 'S':
        return {15, 16, 16, 16, 14, 1, 1, 1, 30};
    case 'T':
        return {31, 4, 4, 4, 4, 4, 4, 4, 4};
    case 'U':
        return {17, 17, 17, 17, 17, 17, 17, 17, 14};
    case 'V':
        return {17, 17, 17, 17, 17, 17, 17, 10, 4};
    case 'W':
        return {17, 17, 17, 17, 17, 21, 21, 21, 10};
    case 'X':
        return {17, 17, 17, 10, 4, 10, 17, 17, 17};
    case 'Y':
        return {17, 17, 17, 10, 4, 4, 4, 4, 4};
    case 'Z':
        return {31, 1, 1, 2, 4, 8, 16, 16, 31};
    case '0':
        return {14, 17, 17, 19, 21, 25, 17, 17, 14};
    case '1':
        return {4, 12, 20, 4, 4, 4, 4, 4, 14};
    case '2':
        return {14, 17, 1, 1, 2, 4, 8, 16, 31};
    case '3':
        return {30, 1, 1, 1, 14, 1, 1, 1, 30};
    case '4':
        return {2, 6, 10, 18, 18, 31, 2, 2, 2};
    case '5':
        return {31, 16, 16, 16, 30, 1, 1, 1, 30};
    case '6':
        return {14, 16, 16, 16, 30, 17, 17, 17, 14};
    case '7':
        return {31, 1, 1, 2, 4, 8, 8, 8, 8};
    case '8':
        return {14, 17, 17, 17, 14, 17, 17, 17, 14};
    case '9':
        return {14, 17, 17, 17, 15, 1, 1, 1, 14};
    case '-':
        return {0, 0, 0, 0, 31, 0, 0, 0, 0};
    case '+':
        return {0, 0, 4, 4, 31, 4, 4, 0, 0};
    case '/':
        return {1, 1, 2, 2, 4, 8, 8, 16, 16};
    case '.':
        return {0, 0, 0, 0, 0, 0, 0, 12, 12};
    case ':':
        return {0, 0, 12, 12, 0, 12, 12, 0, 0};
    case '%':
        return {25, 25, 1, 2, 4, 8, 16, 19, 19};
    case '?':
        return {14, 17, 1, 1, 2, 4, 4, 0, 4};
    case 'a':
        return {0, 0, 0, 14, 1, 15, 17, 17, 15};
    case 'b':
        return {16, 16, 16, 30, 17, 17, 17, 17, 30};
    case 'c':
        return {0, 0, 0, 14, 17, 16, 16, 17, 14};
    case 'd':
        return {1, 1, 1, 15, 17, 17, 17, 17, 15};
    case 'e':
        return {0, 0, 0, 14, 17, 31, 16, 16, 14};
    case 'f':
        return {6, 8, 8, 30, 8, 8, 8, 8, 8};
    case 'g':
        return {0, 0, 14, 17, 17, 15, 1, 17, 14};
    case 'h':
        return {16, 16, 16, 30, 17, 17, 17, 17, 17};
    case 'i':
        return {4, 0, 0, 12, 4, 4, 4, 4, 14};
    case 'j':
        return {2, 0, 0, 6, 2, 2, 2, 18, 12};
    case 'k':
        return {16, 16, 16, 17, 18, 20, 24, 20, 18};
    case 'l':
        return {12, 4, 4, 4, 4, 4, 4, 4, 14};
    case 'm':
        return {0, 0, 0, 26, 21, 21, 21, 21, 21};
    case 'n':
        return {0, 0, 0, 30, 17, 17, 17, 17, 17};
    case 'o':
        return {0, 0, 0, 14, 17, 17, 17, 17, 14};
    case 'p':
        return {0, 0, 30, 17, 17, 30, 16, 16, 16};
    case 'q':
        return {0, 0, 15, 17, 17, 15, 1, 1, 1};
    case 'r':
        return {0, 0, 0, 22, 25, 16, 16, 16, 16};
    case 's':
        return {0, 0, 0, 15, 16, 14, 1, 1, 30};
    case 't':
        return {8, 8, 8, 30, 8, 8, 8, 9, 6};
    case 'u':
        return {0, 0, 0, 17, 17, 17, 17, 17, 15};
    case 'v':
        return {0, 0, 0, 17, 17, 17, 17, 10, 4};
    case 'w':
        return {0, 0, 0, 17, 17, 21, 21, 21, 10};
    case 'x':
        return {0, 0, 0, 17, 17, 10, 4, 10, 17};
    case 'y':
        return {0, 0, 17, 17, 17, 15, 1, 17, 14};
    case 'z':
        return {0, 0, 0, 31, 1, 2, 4, 8, 31};
    case '=':
        return {0, 0, 0, 31, 0, 31, 0, 0, 0};
    case '!':
        return {4, 4, 4, 4, 4, 4, 4, 0, 4};
    case '*':
        return {0, 0, 17, 10, 31, 10, 17, 0, 0};
    case '<':
        return {0, 1, 2, 4, 8, 4, 2, 1, 0};
    case '>':
        return {0, 16, 8, 4, 2, 4, 8, 16, 0};
    case '^':
        return {4, 10, 17, 0, 0, 0, 0, 0, 0};
    case '_':
        return {0, 0, 0, 0, 0, 0, 0, 0, 31};
    case ',':
        return {0, 0, 0, 0, 0, 0, 12, 4, 8};
    case '(':
        return {2, 4, 8, 8, 8, 8, 8, 4, 2};
    case ')':
        return {8, 4, 2, 2, 2, 2, 2, 4, 8};
    case '[':
        return {14, 8, 8, 8, 8, 8, 8, 8, 14};
    case ']':
        return {14, 2, 2, 2, 2, 2, 2, 2, 14};
    case '"':
        return {10, 10, 10, 0, 0, 0, 0, 0, 0};
    case '#':
        return {10, 10, 31, 10, 10, 31, 10, 10, 0};
    case '$':
        return {4, 15, 20, 20, 14, 5, 5, 30, 4};
    case '&':
        return {12, 18, 18, 12, 8, 21, 18, 18, 13};
    case '\'':
        return {4, 4, 8, 0, 0, 0, 0, 0, 0};
    case ';':
        return {0, 12, 12, 0, 0, 12, 4, 8, 0};
    case '@':
        return {14, 17, 17, 23, 21, 23, 16, 17, 14};
    case '\\':
        return {16, 16, 8, 8, 4, 2, 2, 1, 1};
    case '`':
        return {8, 4, 2, 0, 0, 0, 0, 0, 0};
    case '{':
        return {2, 4, 4, 4, 8, 4, 4, 4, 2};
    case '|':
        return {4, 4, 4, 4, 4, 4, 4, 4, 4};
    case '}':
        return {8, 4, 4, 4, 2, 4, 4, 4, 8};
    case '~':
        return {0, 0, 0, 9, 22, 0, 0, 0, 0};
    case ' ':
        return {};
    default:
        return {14, 17, 1, 1, 2, 4, 4, 0, 4};
    }
}

inline constexpr auto font_columns = [] {
    std::array<std::array<std::uint16_t, 5>, '~' - ' ' + 1> font{};
    for (std::size_t i = 0; i < font.size(); ++i) {
        const auto rows = glyph(static_cast<char>(' ' + i));
        for (std::size_t column = 0; column < font[i].size(); ++column)
            for (std::size_t row = 0; row < rows.size(); ++row)
                if ((rows[row] & (1U << (4 - column))) != 0)
                    font[i][column] |= static_cast<std::uint16_t>(1U << row);
    }
    return font;
}();

[[nodiscard]] constexpr const std::array<std::uint16_t, 5> &glyph_columns(const char ch) noexcept {
    return font_columns[(ch >= ' ') && (ch <= '~') ? static_cast<std::size_t>(ch - ' ')
                                                   : static_cast<std::size_t>('?' - ' ')];
}

} // namespace coretx::ui::detail
