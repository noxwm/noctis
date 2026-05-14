#pragma once

#include <string>
#include <vector>
#include <unordered_map>

// Token types for the .nox config parser
enum class TokenType {
    IDENT,        // general, keybinds, exec, ...
    STRING,       // "kitty"
    EQUALS,       // =
    LBRACE,       // {
    RBRACE,       // }
    NEWLINE,
    END,
};

struct Token {
    TokenType   type;
    std::string value;
    int         line;
};

// Tokenises a .nox config file into a flat token stream
class NoxLexer {
public:
    explicit NoxLexer(const std::string &source);
    std::vector<Token> tokenize();

private:
    std::string m_src;
    size_t      m_pos  = 0;
    int         m_line = 1;

    char peek(int offset = 0) const;
    char advance();
    void skip_whitespace_inline();
    void skip_comment();
    Token read_ident_or_value();
};

// Parses the token stream into key=value blocks
// Returns a map of block_name → { key → value }
using Block = std::unordered_map<std::string, std::string>;
using BlockMap = std::unordered_map<std::string, std::vector<Block>>;

class NoxParser {
public:
    explicit NoxParser(std::vector<Token> tokens);
    BlockMap parse();

private:
    std::vector<Token> m_tokens;
    size_t             m_pos = 0;

    Token &peek();
    Token &advance();
    bool   expect(TokenType t);
};
