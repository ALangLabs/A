//===- bytecode.cpp - .ambc MLIR bytecode packaging ----------------------===//
//
// Part of the A toolchain (src/astra/mlir). See docs/mlir-architecture.md.
//
//===----------------------------------------------------------------------===//

#include "bytecode.hpp"

#include "mlir/Bytecode/BytecodeReader.h"
#include "mlir/Bytecode/BytecodeWriter.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Block.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/Operation.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <system_error>
#include <vector>

namespace a {
namespace {

/// `.ambc` on-disk layout (all integers little-endian):
///
///   offset  size  field
///   0       4     magic "AMBC" (kAmbcMagic)
///   4       4     u32 kAmbcFormatVersion
///   8       4     u32 producer string length
///   12      N     producer string (UTF-8, no NUL)
///   12+N    8     u64 dialect checksum (FNV-1a over loaded dialect namespaces)
///   20+N    ...   MLIR bytecode payload (mlir::writeBytecodeToFile output)
///
/// The MLIR bytecode payload itself begins with the "ML\xEF"R" magic, so a
/// reader can locate it after the prefix and hand a MemoryBufferRef slice
/// directly to mlir::BytecodeReader.

constexpr size_t kHeaderFixedSize = 4 + 4 + 4 + 8; // magic+ver+len+checksum

void appendU32(std::string &out, uint32_t v) {
  for (unsigned i = 0; i < 4; ++i)
    out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

void appendU64(std::string &out, uint64_t v) {
  for (unsigned i = 0; i < 8; ++i)
    out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

bool readU32(llvm::StringRef s, size_t off, uint32_t &out) {
  if (off + 4 > s.size())
    return false;
  out = 0;
  for (unsigned i = 0; i < 4; ++i)
    out |= static_cast<uint32_t>(static_cast<unsigned char>(s[off + i]))
           << (8 * i);
  return true;
}

bool readU64(llvm::StringRef s, size_t off, uint64_t &out) {
  if (off + 8 > s.size())
    return false;
  out = 0;
  for (unsigned i = 0; i < 8; ++i)
    out |= static_cast<uint64_t>(static_cast<unsigned char>(s[off + i]))
           << (8 * i);
  return true;
}

/// Producer stamp written into the header and into the MLIR bytecode
/// producer field. Embeds the A compiler version and the MLIR version so a
/// `.ambc` produced by a different toolchain is rejected.
std::string ambcProducerString() {
  return (llvm::Twine("a-ambc/") + kAmbcVersion + "/mlir/" +
          LLVM_VERSION_STRING)
      .str();
}

struct AmbcHeader {
  uint32_t formatVersion = 0;
  std::string producer;
  uint64_t dialectChecksum = 0;
  size_t payloadOffset = 0;
};

/// Parse the `.ambc` header from `bytes`. Returns false with `reason` set on
/// malformed input; on success `header` is filled and `payloadOffset` points
/// at the MLIR bytecode payload.
bool parseAmbcHeader(llvm::StringRef bytes, llvm::StringRef path,
                     AmbcHeader &header, std::string &reason) {
  if (bytes.size() < kHeaderFixedSize) {
    reason = "file too small to be .ambc bytecode: " + path.str();
    return false;
  }
  if (bytes.substr(0, 4) != llvm::StringRef(kAmbcMagic, 4)) {
    reason = "missing .ambc magic (expected \"AMBC\"): " + path.str();
    return false;
  }
  uint32_t producerLen = 0;
  if (!readU32(bytes, 4, header.formatVersion) ||
      !readU32(bytes, 8, producerLen)) {
    reason = "truncated .ambc header: " + path.str();
    return false;
  }
  if (header.formatVersion != kAmbcFormatVersion) {
    reason = "unsupported .ambc format version " +
             std::to_string(header.formatVersion) + " (expected " +
             std::to_string(kAmbcFormatVersion) + "): " + path.str();
    return false;
  }
  size_t producerOff = 12;
  if (producerOff + producerLen + 8 > bytes.size()) {
    reason = "truncated .ambc producer/checksum fields: " + path.str();
    return false;
  }
  header.producer = bytes.substr(producerOff, producerLen).str();
  size_t checksumOff = producerOff + producerLen;
  if (!readU64(bytes, checksumOff, header.dialectChecksum)) {
    reason = "truncated .ambc checksum field: " + path.str();
    return false;
  }
  header.payloadOffset = checksumOff + 8;
  return true;
}

} // namespace

uint64_t bytecodeDialectChecksum(mlir::MLIRContext &ctx) {
  std::vector<llvm::StringRef> namespaces;
  namespaces.reserve(ctx.getLoadedDialects().size());
  for (mlir::Dialect *dialect : ctx.getLoadedDialects())
    namespaces.push_back(dialect->getNamespace());
  llvm::sort(namespaces);

  // FNV-1a 64-bit.
  uint64_t hash = 14695981039346656037ULL;
  auto mix = [&hash](llvm::StringRef s) {
    for (char c : s) {
      hash ^= static_cast<unsigned char>(c);
      hash *= 1099511628211ULL;
    }
    hash ^= 0xFF; // record separator between dialect names
    hash *= 1099511628211ULL;
  };
  for (llvm::StringRef ns : namespaces)
    mix(ns);
  return hash;
}

bool writeModuleBytecode(mlir::ModuleOp module, llvm::StringRef path,
                         std::string &error) {
  if (!module) {
    error = "cannot write null module to .ambc: " + path.str();
    return false;
  }

  std::string producer = ambcProducerString();
  std::string header;
  header.reserve(kHeaderFixedSize + producer.size());
  header.append(kAmbcMagic, 4);
  appendU32(header, kAmbcFormatVersion);
  appendU32(header, static_cast<uint32_t>(producer.size()));
  header.append(producer);
  appendU64(header, bytecodeDialectChecksum(*module.getContext()));

  std::error_code ec;
  llvm::raw_fd_ostream os(path, ec, llvm::sys::fs::OF_None);
  if (ec) {
    error = "cannot open .ambc output '" + path.str() + "': " + ec.message();
    return false;
  }
  os.write(header.data(), header.size());

  mlir::BytecodeWriterConfig config(producer);
  if (mlir::failed(mlir::writeBytecodeToFile(module.getOperation(), os,
                                           config))) {
    error = "mlir::BytecodeWriter failed for '" + path.str() + "'";
    os.close();
    return false;
  }
  os.flush();
  if (os.has_error()) {
    error = "failed writing .ambc bytecode to '" + path.str() + "'";
    return false;
  }
  os.close();
  return true;
}

mlir::OwningOpRef<mlir::ModuleOp>
readModuleBytecode(mlir::MLIRContext &ctx, llvm::StringRef path,
                   std::string &error) {
  auto bufferOrErr = llvm::MemoryBuffer::getFile(path);
  if (!bufferOrErr) {
    error = "cannot read .ambc file '" + path.str() + "': " +
            bufferOrErr.getError().message();
    return {};
  }
  std::unique_ptr<llvm::MemoryBuffer> buffer = std::move(*bufferOrErr);
  llvm::StringRef bytes = buffer->getBuffer();

  AmbcHeader header;
  if (!parseAmbcHeader(bytes, path, header, error))
    return {};

  std::string expectedProducer = ambcProducerString();
  if (header.producer != expectedProducer) {
    error = "stale .ambc bytecode '" + path.str() + "': produced by '" +
            header.producer + "', this toolchain is '" + expectedProducer +
            "'";
    return {};
  }

  // Hand the payload slice (which begins with the MLIR bytecode magic) to
  // the bytecode reader. The MemoryBufferRef aliases `buffer`, which stays
  // alive until after finalize().
  llvm::StringRef payload = bytes.drop_front(header.payloadOffset);
  llvm::MemoryBufferRef payloadRef(payload, buffer->getBufferIdentifier());
  if (!mlir::isBytecode(payloadRef)) {
    error = "corrupt .ambc bytecode (missing MLIR payload magic): " +
            path.str();
    return {};
  }

  mlir::ParserConfig config(&ctx);
  mlir::BytecodeReader reader(payloadRef, config, /*lazyLoad=*/false);

  // v1 is eager: readTopLevel materializes every region and finalize() has
  // nothing left to do. The lazy path would pass lazyLoad=true, a lazyOps
  // callback, and call materialize()/finalize() incrementally — see the
  // doc comment in bytecode.hpp.
  auto block = std::make_unique<mlir::Block>();
  if (mlir::failed(reader.readTopLevel(block.get()))) {
    error = "failed to parse .ambc bytecode: " + path.str();
    return {};
  }
  if (mlir::failed(reader.finalize())) {
    error = "failed to finalize .ambc bytecode: " + path.str();
    return {};
  }

  if (block->empty() || !llvm::hasSingleElement(*block)) {
    error = ".ambc bytecode must contain exactly one top-level module op: " +
            path.str();
    return {};
  }
  mlir::ModuleOp module = mlir::dyn_cast<mlir::ModuleOp>(&block->front());
  if (!module) {
    error = ".ambc top-level op is not a module: " + path.str();
    return {};
  }
  // Detach from the temporary block so the block's destructor does not own
  // (and destroy) the module; OwningOpRef takes over ownership.
  module->remove();
  return mlir::OwningOpRef<mlir::ModuleOp>(module);
}

bool bytecodeIsCompatible(llvm::StringRef path, std::string &reason) {
  auto bufferOrErr = llvm::MemoryBuffer::getFile(path);
  if (!bufferOrErr) {
    reason = "cannot read file: " + bufferOrErr.getError().message();
    return false;
  }
  llvm::StringRef bytes = (*bufferOrErr)->getBuffer();

  AmbcHeader header;
  if (!parseAmbcHeader(bytes, path, header, reason))
    return false;

  std::string expectedProducer = ambcProducerString();
  if (header.producer != expectedProducer) {
    reason = "stale .ambc bytecode (producer '" + header.producer +
             "', expected '" + expectedProducer + "')";
    return false;
  }

  llvm::StringRef payload = bytes.drop_front(header.payloadOffset);
  llvm::MemoryBufferRef payloadRef(payload, (*bufferOrErr)->getBufferIdentifier());
  if (!mlir::isBytecode(payloadRef)) {
    reason = "corrupt .ambc bytecode (missing MLIR payload magic)";
    return false;
  }
  return true;
}

} // namespace a
