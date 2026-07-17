#pragma once

#include <iomanip>
#include <ostream>
#include <sstream>
#include <string>

namespace qas::evaluation {

inline std::string json_escape(const std::string& value) {
    std::ostringstream output;
    for (const unsigned char character : value) {
        switch (character) {
            case '\"':
                output << "\\\"";
                break;
            case '\\':
                output << "\\\\";
                break;
            case '\b':
                output << "\\b";
                break;
            case '\f':
                output << "\\f";
                break;
            case '\n':
                output << "\\n";
                break;
            case '\r':
                output << "\\r";
                break;
            case '\t':
                output << "\\t";
                break;
            default:
                if (character < 0x20U) {
                    output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                           << static_cast<unsigned>(character) << std::dec;
                } else {
                    output << character;
                }
        }
    }
    return output.str();
}

inline void write_json_string(std::ostream& output, const std::string& value) {
    output << '\"' << json_escape(value) << '\"';
}

}  // namespace qas::evaluation
