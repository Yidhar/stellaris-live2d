#pragma once
// Paths as text: every path kept in a std::string in this code is UTF-8, so a user folder with any characters works (the ANSI code page cannot
// represent every name, and std::filesystem::path::string() throws on one it cannot). Convert at the boundary: P() to a path, U8() back, W()
// for a wide Windows API.
#include <windows.h>

#include <filesystem>
#include <string>

namespace l2d {

inline std::wstring W(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), w.data(), n);
    return w;
}

inline std::string U8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

inline std::string U8(const std::filesystem::path& p) { return U8(p.wstring()); }

inline std::filesystem::path P(const std::string& utf8) { return std::filesystem::path(W(utf8)); }

}  // namespace l2d
