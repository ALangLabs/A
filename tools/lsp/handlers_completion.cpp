#include "handlers_common.hpp"

void LanguageServer::completion(const Value& id, const Value* params) {
  Document* document = documentFrom(params);
  if (!document || !document->compilation) return replyNull(id);
  const Compilation& compilation = *document->compilation;
  const FileIndex* file = compilation.entryIndex();
  const std::vector<Token>* tokens = compilation.entryTokens();
  if (!file || !tokens) return replyNull(id);

  int line = 0;
  int character = 0;
  if (!positionFrom(params, &line, &character)) return replyNull(id);
  int byteColumn = utf16ToByteColumn(lineText(document->text, line + 1), character) + 1;

  std::unordered_set<std::string> seen;
  Value items = Value::array();

  auto pushItem = [&](const std::string& name, int kind, const std::string& detail,
                      const std::string& documentation = {}) {
    if (!seen.insert(name).second) return;
    Value item = Value::object();
    item["label"] = name;
    item["kind"] = kind;
    item["insertText"] = name;
    item["filterText"] = name;
    // Snippets for control flow (ty/pyrefly offer these).
    if (name == "if") { item["insertText"] = "if ${1:condition}:\n    $0"; item["insertTextFormat"] = 2; }
    else if (name == "while") { item["insertText"] = "while ${1:condition}:\n    $0"; item["insertTextFormat"] = 2; }
    else if (name == "for") { item["insertText"] = "for ${1:item} in ${2:iterable}:\n    $0"; item["insertTextFormat"] = 2; }
    else if (name == "fn") { item["insertText"] = "fn ${1:name}(${2:params}) -> ${3:void}:\n    $0"; item["insertTextFormat"] = 2; }
    if (!detail.empty()) item["detail"] = detail;
    if (!documentation.empty()) {
      Value data = Value::object();
      data["documentation"] = documentation;
      item["data"] = std::move(data);
      item["documentation"] = Value::object();
      item["documentation"]["kind"] = "markdown";
      item["documentation"]["value"] = documentation;
    }
    items.push(std::move(item));
  };

  auto pushMethods = [&](const std::vector<a::MethodSig>& table) {
    for (const a::MethodSig& method : table) {
      pushItem(method.name, 2, method.signature, method.documentation);
    }
  };

  // Member completion after "." or "?.".
  int previous = tokenIndexBefore(*tokens, line + 1, byteColumn - 1);
  int base = -1;
  if (previous >= 0) {
    const Token& token = (*tokens)[static_cast<std::size_t>(previous)];
    if (token.kind == Tok::Dot || token.kind == Tok::QuestionDot) {
      if (previous >= 1) base = previous - 1;
    } else if (previous >= 1) {
      const Token& before =
          (*tokens)[static_cast<std::size_t>(previous - 1)];
      if ((before.kind == Tok::Dot || before.kind == Tok::QuestionDot) && previous >= 2) {
        base = previous - 2;
      }
    }
  }

  if (base >= 0) {
    const Token& baseToken = (*tokens)[static_cast<std::size_t>(base)];
    for (const a::lsp::ImportDef& import : file->imports) {
      if (!import.wholeModule || import.alias != baseToken.lexeme || import.moduleId < 0) continue;
      const FileIndex* target = compilation.indexForModule(import.moduleId);
      if (!target) continue;
      for (const SymbolDef& symbol : target->symbols) {
        if (symbol.topLevel) {
          pushItem(symbol.name, completionKind(symbol.kind), symbol.detail);
        }
      }
    }

    if (baseToken.kind == Tok::Ident || baseToken.kind == Tok::KwSelf) {
      const std::string moduleName = nativeModuleForAlias(*file, baseToken.lexeme);
      if (!moduleName.empty()) {
        for (const a::lsp::MemberInfo& member : a::lsp::nativeModuleMembers(moduleName)) {
          pushItem(member.name, member.isConstant ? 21 : 3, member.signature,
                   member.documentation);
        }
      }
      ResolvedSymbol resolved =
          a::lsp::resolveToken(compilation, *file, *tokens, base);
      if (resolved.valid()) {
        std::string typeName = resolved.symbol->typeName;
        if (typeName.empty() &&
            (resolved.symbol->kind == SymbolKind::Struct ||
             resolved.symbol->kind == SymbolKind::Class ||
             resolved.symbol->kind == SymbolKind::Enum)) {
          typeName = resolved.symbol->name;
        }
        if (const std::vector<a::MethodSig>* table = methodTableForType(typeName)) {
          pushMethods(*table);
        } else if (!typeName.empty()) {
          for (const SymbolDef* member :
               a::lsp::structMembers(compilation, *resolved.file, typeName)) {
            pushItem(member->name, completionKind(member->kind), member->detail);
          }
        }
      }
    } else if (baseToken.kind == Tok::String) {
      pushMethods(a::stringMethods());
    } else if (baseToken.kind == Tok::RBracket) {
      pushMethods(a::arrayMethods());
    } else if (baseToken.kind == Tok::RBrace) {
      // Distinguish `{key: value}` maps from `{element}` sets by looking
      // for a colon at the literal's top level.
      bool isMap = false;
      int depth = 0;
      for (int i = base - 1; i >= 0; --i) {
        const Token& inner = (*tokens)[static_cast<std::size_t>(i)];
        if (inner.kind == Tok::RBrace || inner.kind == Tok::RBracket ||
            inner.kind == Tok::RParen) {
          ++depth;
        } else if (inner.kind == Tok::LBrace) {
          if (depth == 0) break;
          --depth;
        } else if (inner.kind == Tok::LBracket || inner.kind == Tok::LParen) {
          --depth;
        } else if (inner.kind == Tok::Colon && depth == 0) {
          isMap = true;
          break;
        }
      }
      pushMethods(isMap ? a::mapMethods() : a::setMethods());
    }

    // Never fall back to the global list after a dot; empty is correct
    // for receivers with no members.
    Value result = Value::object();
    result["isIncomplete"] = false;
    result["items"] = std::move(items);
    reply(id, std::move(result));
    return;
  }

  for (const char* keyword : kKeywords) pushItem(keyword, 14, "keyword");
  for (const auto& entry : a::builtinDocs()) {
    const std::string& name = entry.first;
    if (a::isStdlibBuiltin(name) || name == "PI" || name == "E") continue;
    pushItem(name, 3, entry.second.signature);
  }
  for (const SymbolDef* symbol :
       a::lsp::visibleSymbols(compilation, *file, line + 1, byteColumn)) {
    pushItem(symbol->name, completionKind(symbol->kind), symbol->detail);
  }
  for (const a::lsp::ImportDef& import : file->imports) {
    pushItem(import.alias, import.wholeModule ? 9 : 18, import.modulePath);
  }

  // `import <TAB>`, `from <TAB>`, and `from module import <TAB>`.
  std::string currentLine = lineText(document->text, line + 1);
  std::string prefix = currentLine.substr(
      0, std::min<std::size_t>(byteColumn - 1, currentLine.size()));
  const bool insideWord =
      !prefix.empty() && !std::isspace(static_cast<unsigned char>(prefix.back()));
  std::vector<std::string> words;
  {
    std::istringstream stream(prefix);
    std::string word;
    while (stream >> word) words.push_back(word);
  }
  std::string previousWord;
  if (!words.empty()) {
    previousWord =
        insideWord && words.size() >= 2 ? words[words.size() - 2] : words.back();
  }
  if (previousWord == "import" || previousWord == "from") {
    const std::string& typed = insideWord ? words.back() : std::string();
    if (typed.rfind("std:", 0) == 0) {
      // `import std:<TAB>` / `from std:<TAB>`: complete stdlib packages.
      for (const auto& module : a::nativeModules()) {
        pushItem(module.first, 9, "native module");
      }
    } else if (previousWord == "from" && words.size() >= 2) {
      const std::size_t needed = insideWord ? 3 : 2;
      if (words.size() >= needed) {
        const std::size_t moduleIndex = insideWord ? words.size() - 3 : words.size() - 2;
        std::string moduleName = words[moduleIndex];
        if (moduleName.rfind("std:", 0) == 0) moduleName.erase(0, 4);
        if (a::isNativeModule(moduleName)) {
          for (const a::lsp::MemberInfo& member :
               a::lsp::nativeModuleMembers(moduleName)) {
            pushItem(member.name, member.isConstant ? 21 : 3, member.signature,
                     member.documentation);
          }
        }
      }
    } else {
      pushItem("std:", 9, "stdlib package");
      for (const auto& module : a::nativeModules()) {
        pushItem(module.first, 9, "native module");
      }
    }
  }

  Value result = Value::object();
  result["isIncomplete"] = false;
  result["items"] = std::move(items);
  reply(id, std::move(result));
}

void LanguageServer::completionResolve(const Value& id, const Value* params) {
  if (!params || !params->isObject()) return replyNull(id);
  const Value* item = params;
  if (const Value* nested = params->find("item"); nested && nested->isObject()) item = nested;
  Value result = *item;
  const Value* data = item->find("data");
  const Value* documentation = data && data->isObject() ? data->find("documentation") : nullptr;
  if (documentation && documentation->isString()) {
    Value contents = Value::object();
    contents["kind"] = "markdown";
    contents["value"] = documentation->asString();
    result["documentation"] = std::move(contents);
  }
  reply(id, std::move(result));
}

void LanguageServer::signatureHelp(const Value& id, const Value* params) {
  Document* document = documentFrom(params);
  if (!document || !document->compilation) return replyNull(id);
  const Compilation& compilation = *document->compilation;
  const FileIndex* file = compilation.entryIndex();
  const std::vector<Token>* tokens = compilation.entryTokens();
  if (!file || !tokens) return replyNull(id);

  int line = 0;
  int character = 0;
  if (!positionFrom(params, &line, &character)) return replyNull(id);
  const int cursorLine = line + 1;
  const int byteColumn = utf16ToByteColumn(lineText(document->text, cursorLine), character) + 1;

  // Innermost unmatched bracket whose opening is before the cursor.
  std::vector<int> stack;
  for (int i = 0; i < static_cast<int>(tokens->size()); ++i) {
    const Token& token = (*tokens)[static_cast<std::size_t>(i)];
    if (token.kind == Tok::Eof) break;
    if (token.span.line > cursorLine ||
        (token.span.line == cursorLine && token.span.col >= byteColumn)) {
      break;
    }
    if (token.kind == Tok::LParen || token.kind == Tok::LBracket || token.kind == Tok::LBrace) {
      stack.push_back(i);
    } else if (token.kind == Tok::RParen || token.kind == Tok::RBracket ||
               token.kind == Tok::RBrace) {
      if (!stack.empty()) stack.pop_back();
    }
  }
  if (stack.empty() || (*tokens)[static_cast<std::size_t>(stack.back())].kind != Tok::LParen) {
    return replyNull(id);
  }
  const int openIndex = stack.back();
  if (openIndex == 0) return replyNull(id);

  // Callee: `name(`, `module.member(`, or `receiver.method(`.
  std::string label;
  std::string documentation;  // markdown for SignatureInformation.documentation
  const Token& callee = (*tokens)[static_cast<std::size_t>(openIndex - 1)];
  if (callee.kind != Tok::Ident) return replyNull(id);
  const bool qualified = openIndex >= 3 &&
                         ((*tokens)[static_cast<std::size_t>(openIndex - 2)].kind == Tok::Dot ||
                          (*tokens)[static_cast<std::size_t>(openIndex - 2)].kind ==
                              Tok::QuestionDot);
  const Token& baseToken =
      qualified ? (*tokens)[static_cast<std::size_t>(openIndex - 3)] : callee;
  const bool qualifiedOnDot =
      qualified && (baseToken.kind == Tok::Ident || baseToken.kind == Tok::String ||
                    baseToken.kind == Tok::RBracket || baseToken.kind == Tok::RBrace);
  if (!qualified && baseToken.kind != Tok::Ident) return replyNull(id);

  const std::string moduleAlias =
      qualifiedOnDot && baseToken.kind == Tok::Ident ? nativeModuleForAlias(*file, baseToken.lexeme)
                                                     : std::string();
  if (!moduleAlias.empty()) {
    const std::string* builtinName = a::nativeFunction(moduleAlias, callee.lexeme);
    if (!builtinName) builtinName = a::nativeConstant(moduleAlias, callee.lexeme);
    if (builtinName) {
      if (const a::BuiltinDoc* doc = a::findBuiltinDoc(*builtinName)) {
        label = doc->signature;
        documentation = doc->documentation ? doc->documentation : "";
        if (doc->example && *doc->example) {
          documentation += "\n\n**Example:**\n\n```\n";
          documentation += doc->example;
          documentation += "\n```";
        }
      }
    }
  }
  if (label.empty()) {
    const bool primitiveBase = baseToken.kind != Tok::Ident &&
                               baseToken.kind != Tok::Eof &&
                               (baseToken.kind == Tok::String ||
                                baseToken.kind == Tok::RBracket ||
                                baseToken.kind == Tok::RBrace);
    std::string typeName;
    if (primitiveBase) {
      typeName = baseToken.kind == Tok::String
                     ? "String"
                     : (baseToken.kind == Tok::RBracket ? "array" : "map");
      if (baseToken.kind == Tok::RBrace) {
        int depth = 0;
        bool isMap = false;
        for (int i = openIndex - 4; i >= 0; --i) {
          const Token& inner = (*tokens)[static_cast<std::size_t>(i)];
          if (inner.kind == Tok::RBrace || inner.kind == Tok::RBracket ||
              inner.kind == Tok::RParen) {
            ++depth;
          } else if (inner.kind == Tok::LBrace) {
            if (depth == 0) break;
            --depth;
          } else if (inner.kind == Tok::LBracket || inner.kind == Tok::LParen) {
            --depth;
          } else if (inner.kind == Tok::Colon && depth == 0) {
            isMap = true;
            break;
          }
        }
        typeName = isMap ? "map" : "set";
      }
    } else if (baseToken.kind == Tok::Ident || baseToken.kind == Tok::KwSelf) {
      ResolvedSymbol resolved =
          a::lsp::resolveToken(compilation, *file, *tokens, openIndex - 3);
      if (resolved.valid()) typeName = resolved.symbol->typeName;
    }
    const std::vector<a::MethodSig>* table = methodTableForType(typeName);
    if (table) {
      if (const a::MethodSig* method = findMethodSig(*table, callee.lexeme)) {
        label = method->signature;
        documentation = method->documentation ? method->documentation : "";
        if (method->example && *method->example) {
          documentation += "\n\n**Example:**\n\n```\n";
          documentation += method->example;
          documentation += "\n```";
        }
      }
    }
  }
  if (label.empty() && (baseToken.kind == Tok::Ident || baseToken.kind == Tok::KwSelf)) {
    ResolvedSymbol resolved = a::lsp::resolveToken(compilation, *file, *tokens, openIndex - 1);
    if (resolved.valid() && !resolved.symbol->detail.empty()) {
      label = resolved.symbol->detail;
      documentation =
          docCommentText(compilation, *resolved.file, resolved.symbol->span);
    }
  }
  if (label.empty() && qualifiedOnDot && baseToken.kind == Tok::Ident) {
    auto found = compilation.sema.functions.find(baseToken.lexeme + "." + callee.lexeme);
    if (found == compilation.sema.functions.end()) {
      found = compilation.sema.functions.find(callee.lexeme);
    }
    if (found != compilation.sema.functions.end() && found->second) {
      label = a::lsp::signatureText(*found->second);
    }
  }
  if (label.empty() && !qualifiedOnDot) {
    if (const a::BuiltinDoc* doc = a::findBuiltinDoc(callee.lexeme)) {
      label = doc->signature;
      documentation = doc->documentation ? doc->documentation : "";
      if (doc->example && *doc->example) {
        documentation += "\n\n**Example:**\n\n```\n";
        documentation += doc->example;
        documentation += "\n```";
      }
    }
  }
  if (label.empty()) return replyNull(id);

  // Split the parameter list of the label into display segments.
  std::vector<std::string> parameters;
  std::size_t start = label.find('(');
  if (start != std::string::npos) {
    int depth = 0;
    std::size_t segmentStart = start + 1;
    for (std::size_t i = start; i < label.size(); ++i) {
      char c = label[i];
      if (c == '(' || c == '[' || c == '{') {
        ++depth;
      } else if (c == ')' || c == ']' || c == '}') {
        --depth;
        if (depth == 0) {
          std::string last = label.substr(segmentStart, i - segmentStart);
          if (!last.empty()) parameters.push_back(last);
          break;
        }
      } else if (c == ',' && depth == 1) {
        parameters.push_back(label.substr(segmentStart, i - segmentStart));
        segmentStart = i + 1;
      }
    }
  }

  // Active parameter: top-level commas between the open paren and cursor.
  int activeParameter = 0;
  int depth = 0;
  for (int i = openIndex + 1; i < static_cast<int>(tokens->size()); ++i) {
    const Token& token = (*tokens)[static_cast<std::size_t>(i)];
    if (token.kind == Tok::Eof) break;
    if (token.span.line > cursorLine ||
        (token.span.line == cursorLine && token.span.col >= byteColumn)) {
      break;
    }
    if (token.kind == Tok::LParen || token.kind == Tok::LBracket || token.kind == Tok::LBrace) {
      ++depth;
    } else if (token.kind == Tok::RParen || token.kind == Tok::RBracket ||
               token.kind == Tok::RBrace) {
      if (depth == 0) break;
      --depth;
    } else if (token.kind == Tok::Comma && depth == 0) {
      ++activeParameter;
    }
  }
  if (activeParameter > 0 && parameters.empty()) activeParameter = 0;
  if (static_cast<std::size_t>(activeParameter) >= parameters.size() && !parameters.empty()) {
    activeParameter = static_cast<int>(parameters.size()) - 1;
  }

  Value signature = Value::object();
  signature["label"] = label;
  if (!documentation.empty()) {
    Value doc = Value::object();
    doc["kind"] = "markdown";
    doc["value"] = documentation;
    signature["documentation"] = std::move(doc);
  }
  Value parameterValues = Value::array();
  for (const std::string& parameter : parameters) {
    Value entry = Value::object();
    std::string text = parameter;
    while (!text.empty() && text.front() == ' ') text.erase(text.begin());
    entry["label"] = text;
    parameterValues.push(std::move(entry));
  }
  if (!parameters.empty()) signature["parameters"] = std::move(parameterValues);
  Value signatures = Value::array();
  signatures.push(std::move(signature));

  Value result = Value::object();
  result["signatures"] = std::move(signatures);
  result["activeSignature"] = 0;
  result["activeParameter"] = activeParameter;
  reply(id, std::move(result));
}


}  // namespace a::lsp
