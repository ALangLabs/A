// JSON-RPC transport over stdio: Content-Length framing, replies,
// notifications, and error responses. Methods only touch outMutex_ and
// stdin/stdout; see server.hpp.

#include "server.hpp"

#include <cctype>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>

namespace a::lsp {

bool LanguageServer::logging() const {
  static const bool enabled = std::getenv("A_LSP_LOG") != nullptr;
  return enabled;
}

void LanguageServer::log(const std::string& message) {
  if (logging()) std::cerr << "[a-lsp] " << message << "\n";
}

void LanguageServer::reply(const Value& id, Value result) {
  Value response = Value::object();
  response["jsonrpc"] = "2.0";
  response["id"] = id;
  response["result"] = std::move(result);
  writeMessage(response);
}

void LanguageServer::replyNull(const Value& id) {
  Value response = Value::object();
  response["jsonrpc"] = "2.0";
  response["id"] = id;
  response["result"] = Value();
  writeMessage(response);
}

void LanguageServer::sendError(const Value& id, int code, const std::string& message) {
  Value response = Value::object();
  response["jsonrpc"] = "2.0";
  response["id"] = id;
  Value error = Value::object();
  error["code"] = code;
  error["message"] = message;
  response["error"] = std::move(error);
  writeMessage(response);
}

void LanguageServer::notify(const std::string& method, Value params) {
  Value message = Value::object();
  message["jsonrpc"] = "2.0";
  message["method"] = method;
  message["params"] = std::move(params);
  writeMessage(message);
}

bool LanguageServer::readMessage(std::istream& input, std::string& body) {
  std::size_t contentLength = 0;
  bool sawHeader = false;
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) break;
    sawHeader = true;
    std::size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string name = line.substr(0, colon);
    std::string value = line.substr(colon + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
      value.erase(value.begin());
    }
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (name == "content-length") {
      try {
        contentLength = static_cast<std::size_t>(std::stoul(value));
      } catch (...) {
        return false;
      }
    }
  }
  if (!sawHeader && input.eof()) return false;
  if (contentLength == 0) return !input.eof();
  body.resize(contentLength);
  input.read(body.data(), static_cast<std::streamsize>(contentLength));
  return static_cast<std::size_t>(input.gcount()) == contentLength;
}

void LanguageServer::writeMessage(const Value& message) {
  std::string body = message.dump();
  std::lock_guard<std::mutex> lock(outMutex_);
  std::cout << "Content-Length: " << body.size() << "\r\n\r\n";
  std::cout.write(body.data(), static_cast<std::streamsize>(body.size()));
  std::cout.flush();
}

}  // namespace a::lsp
