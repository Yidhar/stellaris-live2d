#include "pdx_script.hpp"
#include "utf8_path.hpp"

#include <cstdlib>
#include <fstream>
#include <iterator>

namespace pdx {

const Node* Node::Find(const std::string& k) const {
    for (const Node& c : children)
        if (c.key == k) return &c;
    return nullptr;
}

std::string Node::Str(const std::string& k, const std::string& fallback) const {
    const Node* n = Find(k);
    return n && !n->block ? n->value : fallback;
}

bool Node::Bool(const std::string& k, bool fallback) const {
    const Node* n = Find(k);
    if (!n || n->block) return fallback;
    return n->value == "yes" || n->value == "true" || n->value == "1";
}

double Node::Num(const std::string& k, double fallback) const {
    const Node* n = Find(k);
    if (!n || n->block || n->value.empty()) return fallback;
    char* end = nullptr;
    const double v = std::strtod(n->value.c_str(), &end);
    return end && end != n->value.c_str() ? v : fallback;
}

std::vector<std::string> Node::List(const std::string& k) const {
    std::vector<std::string> out;
    if (const Node* n = Find(k))
        for (const Node& c : n->children)
            if (c.key.empty() && !c.block) out.push_back(c.value);
    return out;
}

namespace {

struct Reader {
    const std::string& s;
    size_t i = 0;
    int line = 1;
    std::string error;

    explicit Reader(const std::string& text) : s(text) {
        if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) i = 3;
    }

    void SkipSpace() {
        while (i < s.size()) {
            const char c = s[i];
            if (c == '\n') { ++line; ++i; }
            else if (c == ' ' || c == '\t' || c == '\r') ++i;
            else if (c == '#') { while (i < s.size() && s[i] != '\n') ++i; }
            else break;
        }
    }

    static bool WordChar(char c) {
        return !(c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '{' || c == '}' || c == '=' || c == '"' || c == '#' ||
                 c == '<' || c == '>' || c == '!' || c == '?');
    }

    // a word or a quoted string; false at a token that is neither
    bool Token(std::string* out, bool* quoted) {
        SkipSpace();
        if (i >= s.size()) return false;
        if (s[i] == '"') {
            ++i;
            out->clear();
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < s.size()) ++i;
                if (s[i] == '\n') ++line;
                out->push_back(s[i++]);
            }
            if (i >= s.size()) { error = "unterminated string"; return false; }
            ++i;
            *quoted = true;
            return true;
        }
        if (!WordChar(s[i])) return false;
        const size_t b = i;
        while (i < s.size() && WordChar(s[i])) ++i;
        *out = s.substr(b, i - b);
        *quoted = false;
        return true;
    }

    // `=`, `:`, `<`, `>`, `<=`, `>=`, `!=`, `?=`; empty if the next token is not an operator
    std::string Operator() {
        SkipSpace();
        if (i >= s.size()) return "";
        const char c = s[i];
        if (c == '=' || c == ':') { ++i; return "="; }
        if (c == '<' || c == '>' || c == '!' || c == '?') {
            std::string op(1, c);
            ++i;
            if (i < s.size() && s[i] == '=') { op += '='; ++i; }
            return op;
        }
        return "";
    }

    bool Block(Node* parent, int depth) {
        if (depth > 64) { error = "blocks nested too deeply"; return false; }
        for (;;) {
            SkipSpace();
            if (i >= s.size()) return depth == 0 ? true : (error = "missing }", false);
            if (s[i] == '}') {
                ++i;
                if (depth == 0) { error = "unexpected }"; return false; }
                return true;
            }
            Node n;
            n.line = line;
            if (s[i] == '{') {  // an anonymous block
                ++i;
                n.block = true;
                if (!Block(&n, depth + 1)) return false;
                parent->children.push_back(std::move(n));
                continue;
            }
            std::string word;
            bool quoted = false;
            if (!Token(&word, &quoted)) {
                if (error.empty()) error = std::string("unexpected '") + s[i] + "'";
                return false;
            }
            const std::string op = Operator();
            if (op.empty()) {  // a bare list item
                n.value = word;
                n.quoted = quoted;
                parent->children.push_back(std::move(n));
                continue;
            }
            n.key = word;
            n.op = op;
            SkipSpace();
            if (i < s.size() && s[i] == '{') {
                ++i;
                n.block = true;
                if (!Block(&n, depth + 1)) return false;
            } else if (!Token(&n.value, &n.quoted)) {
                if (error.empty()) error = "missing value after '" + word + " " + op + "'";
                return false;
            }
            parent->children.push_back(std::move(n));
        }
    }
};

} // namespace

bool Parse(const std::string& text, Node* root, std::string* error) {
    Reader r(text);
    root->block = true;
    const bool ok = r.Block(root, 0);
    if (!ok && error) *error = "line " + std::to_string(r.line) + ": " + r.error;
    return ok;
}

bool ParseFile(const std::string& path, Node* root, std::string* error) {
    std::ifstream f(l2d::P(path), std::ios::binary);
    if (!f) {
        if (error) *error = "cannot open " + path;
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return Parse(text, root, error);
}

} // namespace pdx
