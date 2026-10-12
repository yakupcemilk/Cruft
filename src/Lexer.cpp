// Lexer.cpp - implementation of the Cruft scanner.
#include "Lexer.h"

#include <cctype>
#include <unordered_map>

namespace cruft {

namespace {

const std::unordered_map<std::string_view, TokenKind> &keywordTable() {
  static const std::unordered_map<std::string_view, TokenKind> table = {
      {"package", TokenKind::KwPackage}, {"import", TokenKind::KwImport},
      {"pub", TokenKind::KwPub},         {"pri", TokenKind::KwPri},
      {"fn", TokenKind::KwFn},           {"extern", TokenKind::KwExtern},
      {"var", TokenKind::KwVar},         {"const", TokenKind::KwConst},
      {"ret", TokenKind::KwRet},         {"end", TokenKind::KwEnd},
      {"if", TokenKind::KwIf},           {"then", TokenKind::KwThen},
      {"else", TokenKind::KwElse},       {"while", TokenKind::KwWhile},
      {"do", TokenKind::KwDo},           {"type", TokenKind::KwType},
      {"match", TokenKind::KwMatch},     {"case", TokenKind::KwCase},
      {"and", TokenKind::KwAnd},         {"or", TokenKind::KwOr},
      {"not", TokenKind::KwNot},         {"true", TokenKind::KwTrue},
      {"false", TokenKind::KwFalse},     {"scope", TokenKind::KwScope},
      {"arena", TokenKind::KwArena},     {"owned", TokenKind::KwOwned},
      {"shared", TokenKind::KwShared},   {"i32", TokenKind::KwI32},
      {"i64", TokenKind::KwI64},         {"f32", TokenKind::KwF32},
      {"f64", TokenKind::KwF64},         {"bool", TokenKind::KwBool},
      {"I", TokenKind::KwInterval},      {"Type", TokenKind::KwUniverse},
      {"Path", TokenKind::KwPath},       {"Equiv", TokenKind::KwEquiv},
      {"i0", TokenKind::KwI0},           {"i1", TokenKind::KwI1},
  };
  return table;
}

} // namespace

std::string_view tokenKindName(TokenKind k) {
  switch (k) {
  case TokenKind::KwPackage: return "'package'";
  case TokenKind::KwImport: return "'import'";
  case TokenKind::KwPub: return "'pub'";
  case TokenKind::KwPri: return "'pri'";
  case TokenKind::KwFn: return "'fn'";
  case TokenKind::KwExtern: return "'extern'";
  case TokenKind::KwVar: return "'var'";
  case TokenKind::KwConst: return "'const'";
  case TokenKind::KwRet: return "'ret'";
  case TokenKind::KwEnd: return "'end'";
  case TokenKind::KwIf: return "'if'";
  case TokenKind::KwThen: return "'then'";
  case TokenKind::KwElse: return "'else'";
  case TokenKind::KwWhile: return "'while'";
  case TokenKind::KwDo: return "'do'";
  case TokenKind::KwType: return "'type'";
  case TokenKind::KwMatch: return "'match'";
  case TokenKind::KwCase: return "'case'";
  case TokenKind::KwAnd: return "'and'";
  case TokenKind::KwOr: return "'or'";
  case TokenKind::KwNot: return "'not'";
  case TokenKind::KwTrue: return "'true'";
  case TokenKind::KwFalse: return "'false'";
  case TokenKind::KwScope: return "'scope'";
  case TokenKind::KwArena: return "'arena'";
  case TokenKind::KwOwned: return "'owned'";
  case TokenKind::KwShared: return "'shared'";
  case TokenKind::KwI32: return "'i32'";
  case TokenKind::KwI64: return "'i64'";
  case TokenKind::KwF32: return "'f32'";
  case TokenKind::KwF64: return "'f64'";
  case TokenKind::KwBool: return "'bool'";
  case TokenKind::KwInterval: return "'I'";
  case TokenKind::KwUniverse: return "'Type'";
  case TokenKind::KwPath: return "'Path'";
  case TokenKind::KwEquiv: return "'Equiv'";
  case TokenKind::KwI0: return "'i0'";
  case TokenKind::KwI1: return "'i1'";
  case TokenKind::Identifier: return "identifier";
  case TokenKind::IntLiteral: return "integer literal";
  case TokenKind::FloatLiteral: return "float literal";
  case TokenKind::StringLiteral: return "string literal";
  case TokenKind::LParen: return "'('";
  case TokenKind::RParen: return "')'";
  case TokenKind::LBracket: return "'['";
  case TokenKind::RBracket: return "']'";
  case TokenKind::Comma: return "','";
  case TokenKind::Colon: return "':'";
  case TokenKind::Dot: return "'.'";
  case TokenKind::Assign: return "'='";
  case TokenKind::Plus: return "'+'";
  case TokenKind::Minus: return "'-'";
  case TokenKind::Star: return "'*'";
  case TokenKind::Slash: return "'/'";
  case TokenKind::Percent: return "'%'";
  case TokenKind::EqEq: return "'=='";
  case TokenKind::BangEq: return "'!='";
  case TokenKind::Less: return "'<'";
  case TokenKind::LessEq: return "'<='";
  case TokenKind::Greater: return "'>'";
  case TokenKind::GreaterEq: return "'>='";
  case TokenKind::At: return "'@'";
  case TokenKind::Tilde: return "'~'";
  case TokenKind::Meet: return "'/\\'";
  case TokenKind::Join: return "'\\/'";
  case TokenKind::Arrow: return "'->'";
  case TokenKind::FatArrow: return "'=>'";
  case TokenKind::EndOfFile: return "end of file";
  case TokenKind::Invalid: return "invalid token";
  }
  return "?";
}

Lexer::Lexer(std::string source, std::string filename, DiagnosticEngine &diags)
    : src_(std::move(source)),
      file_(std::make_shared<const std::string>(std::move(filename))),
      diags_(diags) {}

SourceLocation Lexer::here() const { return SourceLocation{file_, line_, column_}; }

char Lexer::advance() {
  char c = src_[pos_++];
  if (c == '\n') {
    ++line_;
    column_ = 1;
  } else {
    ++column_;
  }
  return c;
}

Token Lexer::make(TokenKind k, std::string text, SourceLocation loc) const {
  return Token{k, std::move(text), std::move(loc)};
}

void Lexer::skipTrivia() {
  for (;;) {
    char c = peek();
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      advance();
    } else if (c == '/' && peek(1) == '/') {
      while (!atEnd() && peek() != '\n')
        advance();
    } else if (c == '/' && peek(1) == '*') {
      SourceLocation start = here();
      advance();
      advance();
      bool closed = false;
      while (!atEnd()) {
        if (peek() == '*' && peek(1) == '/') {
          advance();
          advance();
          closed = true;
          break;
        }
        advance();
      }
      if (!closed)
        diags_.error(start, "unterminated block comment");
    } else {
      return;
    }
  }
}

Token Lexer::lexIdentifierOrKeyword() {
  SourceLocation loc = here();
  size_t start = pos_;
  while (!atEnd() && (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_'))
    advance();
  std::string text = src_.substr(start, pos_ - start);
  auto it = keywordTable().find(text);
  if (it != keywordTable().end())
    return make(it->second, std::move(text), loc);
  return make(TokenKind::Identifier, std::move(text), loc);
}

Token Lexer::lexNumber() {
  SourceLocation loc = here();
  size_t start = pos_;
  bool isFloat = false;
  while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek())))
    advance();
  if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
    isFloat = true;
    advance();
    while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek())))
      advance();
  }
  if (peek() == 'e' || peek() == 'E') {
    size_t save = pos_;
    unsigned saveLine = line_, saveCol = column_;
    advance();
    if (peek() == '+' || peek() == '-')
      advance();
    if (std::isdigit(static_cast<unsigned char>(peek()))) {
      isFloat = true;
      while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek())))
        advance();
    } else {
      pos_ = save;
      line_ = saveLine;
      column_ = saveCol;
    }
  }
  return make(isFloat ? TokenKind::FloatLiteral : TokenKind::IntLiteral,
              src_.substr(start, pos_ - start), loc);
}

Token Lexer::lexString() {
  SourceLocation loc = here();
  advance(); // opening quote
  std::string value;
  while (!atEnd() && peek() != '"') {
    char c = advance();
    if (c == '\\' && !atEnd()) {
      char e = advance();
      switch (e) {
      case 'n': value += '\n'; break;
      case 't': value += '\t'; break;
      case '\\': value += '\\'; break;
      case '"': value += '"'; break;
      case '0': value += '\0'; break;
      default:
        diags_.error(here(), std::string("unknown escape sequence '\\") + e + "'");
      }
    } else {
      value += c;
    }
  }
  if (atEnd())
    diags_.error(loc, "unterminated string literal");
  else
    advance(); // closing quote
  return make(TokenKind::StringLiteral, std::move(value), loc);
}

std::vector<Token> Lexer::tokenize() {
  std::vector<Token> out;
  for (;;) {
    skipTrivia();
    if (atEnd()) {
      out.push_back(make(TokenKind::EndOfFile, "", here()));
      return out;
    }
    char c = peek();
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      out.push_back(lexIdentifierOrKeyword());
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c))) {
      out.push_back(lexNumber());
      continue;
    }
    if (c == '"') {
      out.push_back(lexString());
      continue;
    }

    SourceLocation loc = here();
    auto two = [&](char second, TokenKind yes, TokenKind no) {
      advance();
      if (peek() == second) {
        advance();
        return yes;
      }
      return no;
    };
    TokenKind kind = TokenKind::Invalid;
    std::string text(1, c);
    switch (c) {
    case '(': advance(); kind = TokenKind::LParen; break;
    case ')': advance(); kind = TokenKind::RParen; break;
    case '[': advance(); kind = TokenKind::LBracket; break;
    case ']': advance(); kind = TokenKind::RBracket; break;
    case ',': advance(); kind = TokenKind::Comma; break;
    case ':':
      advance();
      if (peek() == ':') {
        advance();
        diags_.error(loc, "Cruft has no '::' path separator; use dot notation (package.name)");
        kind = TokenKind::Invalid;
        text = "::";
      } else {
        kind = TokenKind::Colon;
      }
      break;
    case '.': advance(); kind = TokenKind::Dot; break;
    case '+': advance(); kind = TokenKind::Plus; break;
    case '*': advance(); kind = TokenKind::Star; break;
    case '%': advance(); kind = TokenKind::Percent; break;
    case '@': advance(); kind = TokenKind::At; break;
    case '~': advance(); kind = TokenKind::Tilde; break;
    case '-': kind = two('>', TokenKind::Arrow, TokenKind::Minus); break;
    case '=':
      advance();
      if (peek() == '>') {
        advance();
        kind = TokenKind::FatArrow;
        text = "=>";
      } else if (peek() == '=') {
        advance();
        kind = TokenKind::EqEq;
        text = "==";
      } else {
        kind = TokenKind::Assign;
      }
      break;
    case '<': kind = two('=', TokenKind::LessEq, TokenKind::Less); break;
    case '>': kind = two('=', TokenKind::GreaterEq, TokenKind::Greater); break;
    case '/': kind = two('\\', TokenKind::Meet, TokenKind::Slash); break;
    case '\\': kind = two('/', TokenKind::Join, TokenKind::Invalid); break;
    case '!':
      kind = two('=', TokenKind::BangEq, TokenKind::Invalid);
      break;
    default:
      advance();
      break;
    }
    if (kind == TokenKind::Invalid && text != "::")
      diags_.error(loc, "unexpected character '" + text + "'");
    out.push_back(make(kind, std::move(text), loc));
  }
}

} // namespace cruft
