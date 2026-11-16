#include "tooling/docgen_internal.hpp"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

namespace a::tooling {
namespace {
void renderDoc(std::ostringstream& out, const std::string& doc) {
  if (doc.empty()) return;
  out << doc << "\n\n";
}

void renderFunction(std::ostringstream& out, const DocItem& item, int headingLevel) {
  const std::string hashes(static_cast<std::size_t>(headingLevel), '#');
  out << hashes << " `" << item.name << "`\n\n";
  out << "```a\n" << item.signature << "\n```\n\n";
  renderDoc(out, item.doc);
  if (!item.params.empty()) {
    out << "**Parameters**\n\n";
    for (const DocParameter& param : item.params) {
      out << "- `" << param.name << "`";
      if (!param.type.empty()) out << " (`" << param.type << "`)";
      out << "\n";
    }
    out << "\n";
  }
}

void renderMembers(std::ostringstream& out, const std::string& title,
                   const std::vector<DocMember>& members) {
  if (members.empty()) return;
  out << "**" << title << "**\n\n";
  for (const DocMember& member : members) {
    out << "- `" << member.signature << "`";
    if (!member.doc.empty()) {
      std::string doc = member.doc;
      std::replace(doc.begin(), doc.end(), '\n', ' ');
      out << " — " << doc;
    }
    out << "\n";
  }
  out << "\n";
}

void renderTypeItem(std::ostringstream& out, const DocItem& item) {
  out << "### `" << item.name << "`\n\n";
  out << "```a\n" << item.signature << "\n```\n\n";
  renderDoc(out, item.doc);
  renderMembers(out, "Fields", item.fields);
  renderMembers(out, "Variants", item.variants);

  for (const DocItem& method : item.methods) {
    out << "#### `" << method.name << "`\n\n";
    out << "```a\n" << method.signature << "\n```\n\n";
    renderDoc(out, method.doc);
    if (!method.params.empty()) {
      out << "**Parameters**\n\n";
      for (const DocParameter& param : method.params) {
        out << "- `" << param.name << "`";
        if (!param.type.empty()) out << " (`" << param.type << "`)";
        out << "\n";
      }
      out << "\n";
    }
  }
}

std::string escapeJson(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (unsigned char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          static const char* hex = "0123456789abcdef";
          out += "\\u00";
          out.push_back(hex[(c >> 4) & 0xF]);
          out.push_back(hex[c & 0xF]);
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  return out;
}

void renderJsonMembers(std::ostringstream& out, const std::vector<DocMember>& members) {
  out << "[";
  for (std::size_t i = 0; i < members.size(); ++i) {
    if (i) out << ",";
    out << "{\"name\":\"" << escapeJson(members[i].name) << "\",\"signature\":\""
        << escapeJson(members[i].signature) << "\",\"type\":\"" << escapeJson(members[i].type)
        << "\",\"doc\":\"" << escapeJson(members[i].doc) << "\"}";
  }
  out << "]";
}

void renderJsonItem(std::ostringstream& out, const DocItem& item) {
  out << "{\"kind\":\"" << kindName(item.kind) << "\",\"name\":\"" << escapeJson(item.name)
      << "\",\"signature\":\"" << escapeJson(item.signature) << "\",\"doc\":\""
      << escapeJson(item.doc) << "\",\"returnType\":\"" << escapeJson(item.returnType)
      << "\",\"private\":" << (item.isPrivate ? "true" : "false");

  out << ",\"params\":[";
  for (std::size_t i = 0; i < item.params.size(); ++i) {
    if (i) out << ",";
    out << "{\"name\":\"" << escapeJson(item.params[i].name) << "\",\"type\":\""
        << escapeJson(item.params[i].type) << "\"}";
  }
  out << "],\"fields\":";
  renderJsonMembers(out, item.fields);
  out << ",\"variants\":";
  renderJsonMembers(out, item.variants);
  out << ",\"methods\":[";
  for (std::size_t i = 0; i < item.methods.size(); ++i) {
    if (i) out << ",";
    renderJsonItem(out, item.methods[i]);
  }
  out << "]}";
}
}  // namespace

std::string renderMarkdown(const DocModule& module) {
  std::ostringstream out;
  out << "# " << module.name << "\n\n";
  if (!module.path.empty()) {
    out << "> Generated from `" << module.path << "` by `a doc`.\n\n";
  }
  renderDoc(out, module.doc);

  std::vector<const DocItem*> functions;
  std::vector<const DocItem*> types;
  std::vector<const DocItem*> values;
  for (const DocItem& item : module.items) {
    switch (item.kind) {
      case DocItem::Kind::Function: functions.push_back(&item); break;
      case DocItem::Kind::Class:
      case DocItem::Kind::Struct:
      case DocItem::Kind::Metaclass:
      case DocItem::Kind::Enum:
      case DocItem::Kind::Trait:
      case DocItem::Kind::TypeAlias: types.push_back(&item); break;
      default: values.push_back(&item); break;
    }
  }

  if (!functions.empty() || !types.empty() || !values.empty()) {
    out << "## Contents\n\n";
    if (!functions.empty()) out << "- [Functions](#functions)\n";
    if (!types.empty()) out << "- [Types](#types)\n";
    if (!values.empty()) out << "- [Constants and variables](#constants-and-variables)\n";
    out << "\n";
  }

  if (!functions.empty()) {
    out << "## Functions\n\n";
    for (const DocItem* item : functions) {
      renderFunction(out, *item, 3);
    }
  }
  if (!types.empty()) {
    out << "## Types\n\n";
    for (const DocItem* item : types) renderTypeItem(out, *item);
  }
  if (!values.empty()) {
    out << "## Constants and variables\n\n";
    for (const DocItem* item : values) {
      out << "### `" << item->name << "`\n\n";
      out << "```a\n" << item->signature << "\n```\n\n";
      renderDoc(out, item->doc);
    }
  }

  std::string text = out.str();
  while (!text.empty() && text.back() == '\n') text.pop_back();
  if (!text.empty()) text += '\n';
  return text;
}

std::string renderJson(const DocModule& module) {
  std::ostringstream out;
  out << "{\"module\":\"" << escapeJson(module.name) << "\",\"path\":\""
      << escapeJson(module.path) << "\",\"doc\":\"" << escapeJson(module.doc)
      << "\",\"items\":[";
  for (std::size_t i = 0; i < module.items.size(); ++i) {
    if (i) out << ",";
    renderJsonItem(out, module.items[i]);
  }
  out << "]}\n";
  return out.str();
}

}  // namespace a::tooling
