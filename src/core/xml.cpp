#include "sc2/core/xml.hpp"

#include <cstdint>
#include <stdexcept>

namespace sc2::core {
namespace {

class Parser {
public:
    Parser(const char* p, size_t n) : p_(p), end_(p + n) {}

    XmlNode parse_document() {
        skip_bom();
        skip_prolog();
        skip_space_and_misc();
        if (!more() || *p_ != '<')
            fail("expected root element");
        XmlNode root = parse_element();
        return root;
    }

private:
    const char* p_;
    const char* end_;

    bool more() const { return p_ < end_; }
    char peek() const { return *p_; }

    [[noreturn]] void fail(const char* msg) const {
        throw std::runtime_error(std::string("xml: ") + msg);
    }

    void skip_bom() {
        if (end_ - p_ >= 3 && uint8_t(p_[0]) == 0xEF && uint8_t(p_[1]) == 0xBB &&
            uint8_t(p_[2]) == 0xBF)
            p_ += 3;
    }

    void skip_space() {
        while (more() && (*p_ == ' ' || *p_ == '\t' || *p_ == '\r' || *p_ == '\n'))
            ++p_;
    }

    void skip_prolog() {
        skip_space();
        if (end_ - p_ >= 2 && p_[0] == '<' && p_[1] == '?') {
            p_ += 2;
            while (more() && !(p_[0] == '?' && p_ + 1 < end_ && p_[1] == '>')) ++p_;
            if (!more()) fail("unterminated <?xml?>");
            p_ += 2;
        }
    }

    void skip_space_and_misc() {
        for (;;) {
            skip_space();
            if (end_ - p_ >= 4 && p_[0] == '<' && p_[1] == '!' && p_[2] == '-' &&
                p_[3] == '-') {
                p_ += 4;
                while (more() &&
                       !(p_[0] == '-' && p_ + 2 < end_ && p_[1] == '-' && p_[2] == '>'))
                    ++p_;
                if (!more()) fail("unterminated comment");
                p_ += 3;
            } else if (end_ - p_ >= 2 && p_[0] == '<' &&
                       (p_[1] == '?' || p_[1] == '!')) {
                p_ += 2;
                while (more() && *p_ != '>') ++p_;
                if (more()) ++p_;
            } else {
                return;
            }
        }
    }

    bool is_name_char(char c) const {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' ||
               c == ':';
    }

    std::string parse_name() {
        const char* start = p_;
        while (more() && is_name_char(*p_)) ++p_;
        if (p_ == start) fail("expected name");
        return std::string(start, p_);
    }

    void decode_entity(std::string& out) {
        ++p_;
        const char* start = p_;
        while (more() && *p_ != ';') ++p_;
        if (!more()) fail("unterminated entity");
        std::string ent(start, p_);
        ++p_;
        if (ent == "amp")
            out.push_back('&');
        else if (ent == "lt")
            out.push_back('<');
        else if (ent == "gt")
            out.push_back('>');
        else if (ent == "quot")
            out.push_back('"');
        else if (ent == "apos")
            out.push_back('\'');
        else if (!ent.empty() && ent[0] == '#') {
            unsigned long code = 0;
            try {
                if (ent.size() >= 2 && (ent[1] == 'x' || ent[1] == 'X'))
                    code = std::stoul(ent.substr(2), nullptr, 16);
                else
                    code = std::stoul(ent.substr(1), nullptr, 10);
            } catch (const std::exception&) {
                throw std::runtime_error("xml: malformed character reference &" + ent + ";");
            }
            append_utf8(out, code);
        } else {
            out.push_back('&');
            out += ent;
            out.push_back(';');
        }
    }

    static void append_utf8(std::string& out, unsigned long c) {
        if (c < 0x80) {
            out.push_back(char(c));
        } else if (c < 0x800) {
            out.push_back(char(0xC0 | (c >> 6)));
            out.push_back(char(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out.push_back(char(0xE0 | (c >> 12)));
            out.push_back(char(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(char(0x80 | (c & 0x3F)));
        } else {
            out.push_back(char(0xF0 | (c >> 18)));
            out.push_back(char(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(char(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(char(0x80 | (c & 0x3F)));
        }
    }

    std::string parse_attr_value() {
        char quote = *p_;
        if (quote != '"' && quote != '\'') fail("expected quoted attribute value");
        ++p_;
        std::string val;
        while (more() && *p_ != quote) {
            if (*p_ == '&')
                decode_entity(val);
            else
                val.push_back(*p_++);
        }
        if (!more()) fail("unterminated attribute value");
        ++p_;
        return val;
    }

    XmlNode parse_element() {
        static thread_local int depth = 0;
        struct DepthGuard {
            int& d;
            explicit DepthGuard(int& x) : d(x) {
                if (++d > 256) { --d; throw std::runtime_error("xml: element nesting deeper than 256"); }
            }
            ~DepthGuard() { --d; }
        } depth_guard(depth);
        ++p_;
        XmlNode node;
        node.name = parse_name();

        for (;;) {
            skip_space();
            if (!more()) fail("unterminated start tag");
            if (*p_ == '/' || *p_ == '>') break;
            std::string key = parse_name();
            skip_space();
            if (!more() || *p_ != '=') fail("expected '=' in attribute");
            ++p_;
            skip_space();
            node.attrs.emplace_back(std::move(key), parse_attr_value());
        }

        if (*p_ == '/') {
            ++p_;
            if (!more() || *p_ != '>') fail("expected '>' after '/'");
            ++p_;
            return node;
        }
        ++p_;

        std::string text;
        for (;;) {
            if (!more()) fail("unterminated element");
            if (*p_ == '<') {
                if (end_ - p_ >= 4 && p_[1] == '!' && p_[2] == '-' && p_[3] == '-') {
                    p_ += 4;
                    while (more() && !(p_[0] == '-' && p_ + 2 < end_ &&
                                       p_[1] == '-' && p_[2] == '>'))
                        ++p_;
                    if (!more()) fail("unterminated comment");
                    p_ += 3;
                    continue;
                }
                if (end_ - p_ >= 2 && p_[1] == '/') {
                    p_ += 2;
                    parse_name();
                    skip_space();
                    if (!more() || *p_ != '>') fail("expected '>' in end tag");
                    ++p_;
                    break;
                }
                if (end_ - p_ >= 8 && std::string(p_, p_ + 8) == "<?token ") {
                    p_ += 7;
                    XmlNode tok;
                    tok.name = "?token";
                    for (;;) {
                        skip_space();
                        if (!more()) fail("unterminated <?token?>");
                        if (*p_ == '?') break;
                        std::string key = parse_name();
                        skip_space();
                        if (!more() || *p_ != '=') fail("expected '=' in <?token?> attribute");
                        ++p_;
                        skip_space();
                        tok.attrs.emplace_back(std::move(key), parse_attr_value());
                    }
                    while (more() && *p_ != '>') ++p_;
                    if (more()) ++p_;
                    node.children.push_back(std::move(tok));
                    continue;
                }
                if (end_ - p_ >= 9 && std::string(p_, p_ + 9) == "<![CDATA[") {
                    p_ += 9;
                    const char* s = p_;
                    while (more() && !(p_[0] == ']' && p_ + 2 < end_ &&
                                       p_[1] == ']' && p_[2] == '>'))
                        ++p_;
                    text.append(s, p_);
                    if (more()) p_ += 3;
                    continue;
                }
                if (end_ - p_ >= 2 && (p_[1] == '?' || p_[1] == '!')) {
                    p_ += 2;
                    while (more() && *p_ != '>') ++p_;
                    if (more()) ++p_;
                    continue;
                }
                node.children.push_back(parse_element());
            } else if (*p_ == '&') {
                decode_entity(text);
            } else {
                text.push_back(*p_++);
            }
        }
        size_t a = text.find_first_not_of(" \t\r\n");
        size_t b = text.find_last_not_of(" \t\r\n");
        if (a != std::string::npos) node.text = text.substr(a, b - a + 1);
        return node;
    }
};

}

XmlNode xml_parse(const std::string& data) {
    Parser parser(data.data(), data.size());
    return parser.parse_document();
}

XmlNode xml_parse(const std::vector<uint8_t>& data) {
    Parser parser(reinterpret_cast<const char*>(data.data()), data.size());
    return parser.parse_document();
}

std::size_t xml_node_heap_bytes(const XmlNode& n) {
    auto s = [](const std::string& x) { return x.capacity() > 15 ? x.capacity() + 1 : std::size_t(0); };
    std::size_t b = s(n.name) + s(n.text) + n.attrs.capacity() * sizeof(n.attrs[0]) + n.children.capacity() * sizeof(XmlNode);
    for (const auto& kv : n.attrs) b += s(kv.first) + s(kv.second);
    for (const XmlNode& c : n.children) b += xml_node_heap_bytes(c);
    return b;
}

}
