// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>

namespace coretx::ui::detail {

// Three ink columns in a 4x6 cell; bit zero is the top row.
constexpr std::array<std::uint8_t, 3> micro_glyph(const char ch) noexcept {
    switch (ch) {
    case '0':
        return {31, 17, 31};
    case '1':
        return {18, 31, 16};
    case '2':
        return {29, 21, 23};
    case '3':
        return {21, 21, 31};
    case '4':
        return {7, 4, 31};
    case '5':
        return {23, 21, 29};
    case '6':
        return {31, 21, 29};
    case '7':
        return {1, 1, 31};
    case '8':
        return {31, 21, 31};
    case '9':
        return {23, 21, 31};
    case 'A':
        return {30, 5, 30};
    case 'B':
        return {31, 21, 10};
    case 'C':
        return {31, 17, 17};
    case 'D':
        return {31, 17, 14};
    case 'E':
        return {31, 21, 17};
    case 'F':
        return {31, 5, 1};
    case 'G':
        return {31, 17, 29};
    case 'H':
        return {31, 4, 31};
    case 'I':
        return {17, 31, 17};
    case 'J':
        return {24, 16, 31};
    case 'K':
        return {31, 4, 27};
    case 'L':
        return {31, 16, 16};
    case 'M':
        return {31, 2, 31};
    case 'N':
        return {31, 1, 30};
    case 'O':
        return {14, 17, 14};
    case 'P':
        return {31, 5, 7};
    case 'Q':
        return {15, 9, 31};
    case 'R':
        return {31, 5, 26};
    case 'S':
        return {23, 21, 29};
    case 'T':
        return {1, 31, 1};
    case 'U':
        return {31, 16, 31};
    case 'V':
        return {15, 16, 15};
    case 'W':
        return {31, 8, 31};
    case 'X':
        return {27, 4, 27};
    case 'Y':
        return {3, 28, 3};
    case 'Z':
        return {25, 21, 19};
    case 'a':
        return {24, 20, 28};
    case 'b':
        return {31, 20, 8};
    case 'c':
        return {28, 20, 20};
    case 'd':
        return {8, 20, 31};
    case 'e':
        return {28, 28, 20};
    case 'f':
        return {4, 30, 5};
    case 'g':
        return {44, 36, 60};
    case 'h':
        return {31, 4, 24};
    case 'i':
        return {0, 29, 0};
    case 'j':
        return {32, 32, 29};
    case 'k':
        return {31, 8, 20};
    case 'l':
        return {0, 31, 16};
    case 'm':
        return {28, 4, 28};
    case 'n':
        return {28, 4, 24};
    case 'o':
        return {28, 20, 28};
    case 'p':
        return {60, 20, 8};
    case 'q':
        return {8, 20, 60};
    case 'r':
        return {28, 4, 4};
    case 's':
        return {20, 28, 8};
    case 't':
        return {4, 31, 20};
    case 'u':
        return {28, 16, 28};
    case 'v':
        return {12, 16, 12};
    case 'w':
        return {28, 8, 28};
    case 'x':
        return {20, 8, 20};
    case 'y':
        return {44, 32, 60};
    case 'z':
        return {4, 28, 16};
    case '+':
        return {4, 14, 4};
    case '-':
        return {4, 4, 4};
    case '*':
        return {10, 4, 10};
    case '/':
        return {24, 4, 3};
    case '\\':
        return {3, 4, 24};
    case '=':
        return {10, 10, 10};
    case '.':
        return {0, 16, 0};
    case ',':
        return {32, 16, 0};
    case ':':
        return {0, 10, 0};
    case ';':
        return {32, 10, 0};
    case '!':
        return {0, 23, 0};
    case '?':
        return {1, 21, 3};
    case '%':
        return {25, 4, 19};
    case '_':
        return {32, 32, 32};
    case '#':
        return {10, 31, 10};
    case '^':
        return {2, 1, 2};
    case '<':
        return {4, 10, 17};
    case '>':
        return {17, 10, 4};
    case '(':
        return {0, 14, 17};
    case ')':
        return {17, 14, 0};
    case '[':
        return {0, 31, 17};
    case ']':
        return {17, 31, 0};
    case '\'':
        return {0, 3, 0};
    case '"':
        return {3, 0, 3};
    case '|':
        return {0, 31, 0};
    case ' ':
        return {};
    default:
        return {1, 21, 3};
    }
}

} // namespace coretx::ui::detail
