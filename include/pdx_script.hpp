#pragma once
// A reader for Paradox script files (the `key = value` / `key = { ... }` text of Stellaris' common/ and gfx/ folders): enough
// to read portrait registrations. No evaluation, no macros; comments (#...), quoted strings, bare words, numbers, nested blocks
// and bare list items are understood. Comparison operators (<, >, <=, >=, !=, ?=) are kept as the node's `op`.
#include <string>
#include <vector>

namespace pdx {

struct Node {
    std::string key;    // empty for a bare list item or an anonymous block
    std::string op;     // "=" (also ":" ), a comparison operator, or empty for a bare item
    std::string value;  // the word or string, unless `block`
    bool block = false;
    bool quoted = false;
    int line = 0;
    std::vector<Node> children;

    const Node* Find(const std::string& k) const;           // first child with this key
    std::string Str(const std::string& k, const std::string& fallback = "") const;
    bool Bool(const std::string& k, bool fallback = false) const;  // yes / no / true / false / 1 / 0
    double Num(const std::string& k, double fallback = 0.0) const;
    std::vector<std::string> List(const std::string& k) const;     // bare words in the child block `k`
};

// Parses `text` (UTF-8, a BOM is skipped) into a node whose children are the file's top-level statements. On a syntax error
// returns false and puts "line N: ..." in *error; whatever was parsed up to there stays in *root.
bool Parse(const std::string& text, Node* root, std::string* error);
bool ParseFile(const std::string& path, Node* root, std::string* error);

} // namespace pdx
