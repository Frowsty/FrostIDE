// SPDX-License-Identifier: Apache-2.0
//
// SettingsDocument (document.hpp): parsing a scope file into member spans, and the byte-exact edits
// (replace a value, append or prepend a member, remove a member, rename a key).

#include "frost/settings/document.hpp"

#include "frost/core/assert.hpp"

#include "json.hpp"

#include <format>
#include <limits>
#include <utility>

namespace frost::settings {

namespace {

[[nodiscard]] bool isBlank(char c) noexcept { return c == ' ' || c == '\t'; }

} // namespace

Result<SettingsDocument> SettingsDocument::parse(std::string bytes) {
    auto parsed = detail::parseObject(bytes);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    SettingsDocument document;
    document.m_hasObject = parsed->hasObject;
    document.m_open = parsed->open;
    document.m_close = parsed->close;
    document.m_members.reserve(parsed->spans.size());
    for (std::size_t i = 0; i < parsed->spans.size(); ++i) {
        const detail::MemberSpan& span = parsed->spans[i];
        const auto [line, column] = detail::lineColumn(bytes, span.keyBegin);
        document.m_members.push_back(Member{.key = std::move(parsed->keys[i]),
                                            .keyBegin = span.keyBegin,
                                            .keyEnd = span.keyEnd,
                                            .valueBegin = span.valueBegin,
                                            .valueEnd = span.valueEnd,
                                            .commaEnd = span.commaEnd,
                                            .line = line,
                                            .column = column});
    }
    document.m_bytes = std::move(bytes);
    return document;
}

SettingsDocument SettingsDocument::create(std::uint32_t schemaVersion) {
    auto document = parse(std::format("{{\n  \"{}\": {}\n}}\n", kSchemaVersionKey, schemaVersion));
    FROST_ASSERT(document.has_value());
    return std::move(*document);
}

const SettingsDocument::Member* SettingsDocument::find(std::string_view key) const noexcept {
    for (auto it = m_members.rbegin(); it != m_members.rend(); ++it) {
        if (it->key == key) {
            return &*it;
        }
    }
    return nullptr;
}

std::string_view SettingsDocument::rawValue(const Member& member) const noexcept {
    return std::string_view(m_bytes).substr(member.valueBegin, member.valueEnd - member.valueBegin);
}

Result<Value> SettingsDocument::value(std::string_view key, const Schema& schema) const {
    const Member* member = find(key);
    if (member == nullptr) {
        return fail(ErrorCode::NotFound, std::format("{} is not set", key));
    }
    auto decoded = detail::decodeValue(rawValue(*member), schema);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    if (auto ok = validateValue(schema, *decoded); !ok) {
        return std::unexpected(ok.error());
    }
    return decoded;
}

std::optional<std::uint32_t> SettingsDocument::schemaVersion() const {
    auto version = value(kSchemaVersionKey, integerSchema(0, std::numeric_limits<std::uint32_t>::max()));
    if (!version) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(std::get<std::int64_t>(*version));
}

Result<void> SettingsDocument::replaceBytes(std::string bytes) {
    auto next = parse(std::move(bytes));
    if (!next) {
        return fail(ErrorCode::Internal, "a settings edit produced invalid JSON: " + next.error().message());
    }
    *this = std::move(*next);
    return {};
}

Result<void> SettingsDocument::insertMember(std::string_view key, std::string_view json, bool atFront) {
    auto quoted = detail::encodeString(key);
    if (!quoted) {
        return std::unexpected(quoted.error());
    }
    const std::string member = *quoted + ": " + std::string(json);
    const std::string newline = m_bytes.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    // The indentation of the line holding byte `pos`, when only blanks precede it there.
    const auto indentAt = [&](std::size_t pos) {
        const std::size_t lineStart = m_bytes.rfind('\n', pos == 0 ? 0 : pos - 1);
        const std::size_t begin = lineStart == std::string::npos ? 0 : lineStart + 1;
        for (std::size_t i = begin; i < pos; ++i) {
            if (!isBlank(m_bytes[i])) {
                return std::string();
            }
        }
        return m_bytes.substr(begin, pos - begin);
    };

    std::string bytes = m_bytes;
    if (!m_hasObject) {
        // Keep whatever the file held (a BOM, comments) and add the object after it.
        if (!bytes.empty() && bytes.back() != '\n' && bytes != "\xEF\xBB\xBF") {
            bytes += newline;
        }
        bytes += "{" + newline + "  " + member + newline + "}" + newline;
        return replaceBytes(std::move(bytes));
    }
    if (m_members.empty()) {
        const std::string_view interior = std::string_view(m_bytes).substr(m_open + 1, m_close - m_open - 1);
        if (interior.find_first_not_of(" \t\r\n") == std::string_view::npos) {
            bytes.replace(m_open + 1, interior.size(), newline + "  " + member + newline);
        } else {
            bytes.insert(m_open + 1, newline + "  " + member);
        }
        return replaceBytes(std::move(bytes));
    }
    const std::string_view lead = std::string_view(m_bytes).substr(m_open + 1, m_members.front().keyBegin - m_open - 1);
    const bool multiLine = lead.find('\n') != std::string_view::npos;
    if (atFront) {
        const Member& first = m_members.front();
        bytes.insert(first.keyBegin, member + "," + (multiLine ? newline + indentAt(first.keyBegin) : " "));
    } else {
        const Member& last = m_members.back();
        const std::string separator = multiLine ? newline + indentAt(last.keyBegin) : " ";
        if (last.commaEnd != 0) {
            bytes.insert(last.commaEnd, separator + member + ","); // Keep the trailing-comma style.
        } else {
            bytes.insert(last.valueEnd, "," + separator + member);
        }
    }
    return replaceBytes(std::move(bytes));
}

Result<void> SettingsDocument::setRaw(std::string_view key, std::string_view json) {
    if (auto ok = detail::checkSingleValue(json); !ok) {
        return ok;
    }
    if (const Member* member = find(key)) {
        std::string bytes = m_bytes;
        bytes.replace(member->valueBegin, member->valueEnd - member->valueBegin, json);
        return replaceBytes(std::move(bytes));
    }
    return insertMember(key, json, false);
}

Result<void> SettingsDocument::set(std::string_view key, const Value& value) {
    auto json = detail::encodeValue(value);
    if (!json) {
        return std::unexpected(json.error());
    }
    return setRaw(key, *json);
}

Result<void> SettingsDocument::setSchemaVersion(std::uint32_t version) {
    const std::string json = std::to_string(version);
    if (const Member* member = find(kSchemaVersionKey)) {
        std::string bytes = m_bytes;
        bytes.replace(member->valueBegin, member->valueEnd - member->valueBegin, json);
        return replaceBytes(std::move(bytes));
    }
    return insertMember(kSchemaVersionKey, json, true);
}

Result<bool> SettingsDocument::remove(std::string_view key) {
    bool removed = false;
    while (const Member* member = find(key)) {
        const auto index = static_cast<std::size_t>(member - m_members.data());
        const std::size_t count = m_members.size();
        std::string bytes = m_bytes;
        if (count == 1) {
            // The only member: empty the object, keeping one line break if it spanned lines.
            const std::string_view interior = std::string_view(m_bytes).substr(m_open + 1, m_close - m_open - 1);
            const bool crlf = m_bytes.find("\r\n") != std::string::npos;
            bytes.replace(m_open + 1, interior.size(),
                          interior.find('\n') != std::string_view::npos ? (crlf ? "\r\n" : "\n") : "");
        } else if (index + 1 < count) {
            // The next member moves into this one's place (and indentation).
            bytes.erase(member->keyBegin, m_members[index + 1].keyBegin - member->keyBegin);
        } else {
            // The last member: take its line, and the comma before it unless commas trail.
            const Member& previous = m_members[index - 1];
            const std::size_t end = member->commaEnd != 0 ? member->commaEnd : member->valueEnd;
            std::size_t begin = member->keyBegin;
            while (begin > previous.commaEnd && isBlank(bytes[begin - 1])) {
                --begin;
            }
            if (begin > previous.commaEnd && bytes[begin - 1] == '\n') {
                --begin;
                if (begin > previous.commaEnd && bytes[begin - 1] == '\r') {
                    --begin;
                }
            }
            bytes.erase(begin, end - begin);
            if (member->commaEnd == 0) {
                bytes.erase(previous.commaEnd - 1, 1);
            }
        }
        if (auto ok = replaceBytes(std::move(bytes)); !ok) {
            return std::unexpected(ok.error());
        }
        removed = true;
    }
    return removed;
}

Result<bool> SettingsDocument::rename(std::string_view from, std::string_view to) {
    if (find(from) == nullptr) {
        return false;
    }
    if (from == to) {
        return true;
    }
    if (find(to) == nullptr) {
        auto quoted = detail::encodeString(to);
        if (!quoted) {
            return std::unexpected(quoted.error());
        }
        const Member* member = find(from);
        std::string bytes = m_bytes;
        bytes.replace(member->keyBegin, member->keyEnd - member->keyBegin, *quoted);
        if (auto ok = replaceBytes(std::move(bytes)); !ok) {
            return std::unexpected(ok.error());
        }
    }
    // Earlier duplicates of `from`, or `from` itself when `to` already existed.
    if (auto removed = remove(from); !removed) {
        return std::unexpected(removed.error());
    }
    return true;
}

} // namespace frost::settings
