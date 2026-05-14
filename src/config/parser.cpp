#include "config/parser.hpp"

#include <stdexcept>
#include <cctype>
#include <algorithm>

// ─── NoxLexer ─────────────────────────────────────────────────────────────

NoxLexer::NoxLexer(const std::string &source) : m_src(source) {}

char NoxLexer::peek(int offset) const {
    size_t idx = m_pos + offset;
    return idx < m_src.size() ? m_src[idx] : '\0';
}

char NoxLexer::advance() {
    char c = m_src[m_pos++];
    if (c == '\n') m_line++;
    return c;
}

void NoxLexer::skip_whitespace_inline() {
    while (m_pos < m_src.size() && (peek() == ' ' || peek() == '\t'))
        advance();
}

void NoxLexer::skip_comment() {
    while (m_pos < m_src.size() && peek() != '\n')
        advance();
}

Token NoxLexer::read_ident_or_value() {
    size_t start = m_pos;
    // Read until whitespace, =, {, }, newline, or #
    while (m_pos < m_src.size()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '=' ||
            c == '{' || c == '}' || c == '\n' || c == '#')
            break;
        advance();
    }
    return { TokenType::IDENT, m_src.substr(start, m_pos - start), m_line };
}

std::vector<Token> NoxLexer::tokenize() {
    std::vector<Token> tokens;

    while (m_pos < m_src.size()) {
        skip_whitespace_inline();

        char c = peek();

        if (c == '\0') break;
        if (c == '#') { skip_comment(); continue; }
        if (c == '\n') {
            tokens.push_back({ TokenType::NEWLINE, "\n", m_line });
            advance();
            continue;
        }
        if (c == '=') {
            tokens.push_back({ TokenType::EQUALS, "=", m_line });
            advance();
            continue;
        }
        if (c == '{') {
            tokens.push_back({ TokenType::LBRACE, "{", m_line });
            advance();
            continue;
        }
        if (c == '}') {
            tokens.push_back({ TokenType::RBRACE, "}", m_line });
            advance();
            continue;
        }

        // Read identifier or bare value (rest of line after =)
        tokens.push_back(read_ident_or_value());
    }

    tokens.push_back({ TokenType::END, "", m_line });
    return tokens;
}

// ─── NoxParser ────────────────────────────────────────────────────────────

NoxParser::NoxParser(std::vector<Token> tokens)
    : m_tokens(std::move(tokens))
{}

Token &NoxParser::peek() {
    return m_tokens[m_pos];
}

Token &NoxParser::advance() {
    return m_tokens[m_pos++];
}

bool NoxParser::expect(TokenType t) {
    if (peek().type != t) return false;
    advance();
    return true;
}

BlockMap NoxParser::parse() {
    BlockMap result;

    while (peek().type != TokenType::END) {
        // Skip blank lines
        if (peek().type == TokenType::NEWLINE) { advance(); continue; }

        // Expect block name
        if (peek().type != TokenType::IDENT) { advance(); continue; }
        std::string block_name = advance().value;

        // Skip optional whitespace/newlines
        while (peek().type == TokenType::NEWLINE) advance();

        // Expect {
        if (peek().type != TokenType::LBRACE) continue;
        advance();

        Block block;

        while (peek().type != TokenType::RBRACE &&
               peek().type != TokenType::END)
        {
            if (peek().type == TokenType::NEWLINE) { advance(); continue; }
            if (peek().type != TokenType::IDENT)   { advance(); continue; }

            // key = rest-of-line
            std::string key = advance().value;

            // Skip optional whitespace
            while (peek().type == TokenType::NEWLINE) { advance(); continue; }

            if (peek().type != TokenType::EQUALS) continue;
            advance(); // consume =

            // Value: collect all IDENT tokens on this line until NEWLINE
            std::string value;
            while (peek().type == TokenType::IDENT) {
                if (!value.empty()) value += ' ';
                value += advance().value;
            }

            block[key] = value;
        }

        if (peek().type == TokenType::RBRACE) advance();

        result[block_name].push_back(block);
    }

    return result;
}
