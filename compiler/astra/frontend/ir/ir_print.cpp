#include "ir/ir.hpp"

#include <ostream>
#include <sstream>
#include <string>

#include "strings/utf8.hpp"

namespace a {
namespace ir {
namespace {
std::string typeText(const TypePtr& type) {
  TypePtr resolved = defaultType(type);
  return resolved ? typeName(resolved) : "?";
}

std::string quoteText(const std::string& text) {
  std::string out = "\"";
  for (char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\r': out += "\\r"; break;
      default: out.push_back(c); break;
    }
  }
  out += '"';
  return out;
}

std::string floatText(double value) {
  std::ostringstream stream;
  stream << value;
  return stream.str();
}

void printInst(std::ostream& os, const Inst& inst) {
  if (inst.id >= 0) os << "%" << inst.id << " = ";
  switch (inst.op) {
    case Op::ConstInt: os << "const " << inst.intValue; break;
    case Op::ConstFloat: os << "const " << floatText(inst.floatValue); break;
    case Op::ConstBool: os << "const " << (inst.boolValue ? "true" : "false"); break;
    case Op::ConstChar: os << "const '" << utf8Encode(inst.charValue) << "'"; break;
    case Op::ConstString: os << "const " << quoteText(inst.stringValue); break;
    case Op::Param: os << "param " << inst.text; break;
    case Op::Phi: break;  // operands print in the phi-specific format below
    case Op::Copy: os << "copy"; break;
    case Op::Binary: os << inst.text; break;
    case Op::Unary: os << inst.text; break;
    case Op::Select: os << "select"; break;
    case Op::Call:
      os << (inst.tailCall ? "tail call " : "call ") << inst.text;
      break;
    case Op::CallValue: os << (inst.tailCall ? "tail call_value" : "call_value"); break;
    case Op::Index: os << "index"; break;
    case Op::Slice: os << "slice"; break;
    case Op::Field: os << "field " << inst.text; break;
    case Op::Array: os << "array"; break;
    case Op::Tuple: os << "tuple"; break;
    case Op::Map: os << "map"; break;
    case Op::Set: os << "set"; break;
    case Op::Range: os << "range"; break;
    case Op::StringConcat: os << "concat"; break;
    case Op::IterInit: os << "iter_init"; break;
    case Op::IterHasNext: os << "iter_has_next"; break;
    case Op::IterNext: os << "iter_next"; break;
    case Op::TupleGet: os << "tuple_get " << inst.intValue; break;
    case Op::StoreIndex: os << "store_index"; break;
    case Op::StoreField: os << "store_field " << inst.text; break;
    case Op::Global: os << "global " << inst.text; break;
    case Op::FuncRef: os << "func " << inst.text; break;
    case Op::Lambda: os << "lambda"; break;
    case Op::Cast: os << "cast " << inst.text; break;
    case Op::GetElementPtr: os << "getelementptr"; break;
    case Op::ExtractValue: os << "extractvalue " << inst.intValue; break;
    case Op::InsertValue: os << "insertvalue " << inst.intValue; break;
    case Op::Freeze: os << "freeze"; break;
    case Op::Fma: os << "fma"; break;
    case Op::Opaque: os << inst.text; break;
  }
  if (inst.op == Op::Phi) {
    os << "phi";
    for (std::size_t i = 0; i < inst.operands.size(); ++i) {
      if (i) os << ",";
      os << " [" << (inst.operands[i] >= 0 ? "%" + std::to_string(inst.operands[i]) : "-")
         << ", bb" << (i < inst.incomingBlocks.size() ? inst.incomingBlocks[i] : -1) << "]";
    }
  } else {
    for (std::size_t i = 0; i < inst.operands.size(); ++i) {
      if (i == 0) os << " ";
      else os << ", ";
      if (inst.operands[i] >= 0) os << "%" << inst.operands[i];
      else os << "_";
    }
  }
  if (inst.type) os << " : " << typeText(inst.type);
  os << "\n";
}

}  // namespace

std::string printFunction(const Function& function) {
  std::ostringstream os;
  os << "fn @" << function.name << "(";
  for (std::size_t i = 0; i < function.params.size(); ++i) {
    if (i) os << ", ";
    os << function.params[i].name << ": " << typeText(function.params[i].type);
  }
  os << ") -> " << typeText(function.returnType) << " {\n";
  for (std::size_t b = 0; b < function.blocks.size(); ++b) {
    const Block& block = function.blocks[b];
    os << "bb" << b << " (" << block.label << "):\n";
    for (const auto& inst : block.insts) printInst(os, inst);
    switch (block.term.kind) {
      case Terminator::Kind::Jump:
        os << "  jump bb" << block.term.target << "\n";
        break;
      case Terminator::Kind::Branch:
        os << "  branch %" << block.term.value << ", bb" << block.term.target << ", bb"
           << block.term.falseTarget << "\n";
        break;
      case Terminator::Kind::Return:
        os << "  ret";
        if (block.term.value >= 0) os << " %" << block.term.value;
        os << "\n";
        break;
      case Terminator::Kind::Unreachable:
        os << "  unreachable\n";
        break;
    }
  }
  os << "}\n";
  return os.str();
}

std::string printProgram(const Program& program) {
  std::ostringstream os;
  for (std::size_t i = 0; i < program.functions.size(); ++i) {
    if (i) os << "\n";
    os << printFunction(program.functions[i]);
  }
  return os.str();
}

}  // namespace ir
}  // namespace a
