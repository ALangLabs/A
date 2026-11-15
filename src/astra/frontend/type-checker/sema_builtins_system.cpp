#include "type-checker/sema_internal.hpp"

namespace a {

TypePtr Sema::checkBuiltinSystem(const std::string& name, CallExpr& call) {
  // os.c primitives (exposed through the native fs/env modules and as
  // os_* builtins). Return types mirror what the runtime actually builds.
  if (name.rfind("os_", 0) == 0) {
    auto inferAll = [&]() { inferCallArgTypes(call); };
    auto arity = [&](std::size_t lo, std::size_t hi) -> bool {
      if (call.args.size() < lo || call.args.size() > hi) {
        builtinArityError(name, call, lo);
        inferAll();
        return false;
      }
      return true;
    };
    if (name == "os_chdir" || name == "os_chmod" || name == "os_symlink" ||
        name == "os_link" || name == "os_utime" || name == "os_mkdir" ||
        name == "os_rmdir" || name == "os_truncate" || name == "os_setenv" ||
        name == "os_unsetenv" || name == "os_chown" || name == "os_kill" ||
        name == "os_exit") {
      inferAll();
      return typeVoid();
    }
    if (name == "os_getpid" || name == "os_getppid" || name == "os_getuid" ||
        name == "os_geteuid" || name == "os_getgid" || name == "os_getegid" ||
        name == "os_cpu_count" || name == "os_system" ||
        name == "os_umask" || name == "os_getpagesize") {
      inferAll();
      return typeInt(64, true);
    }
    if (name == "os_getlogin" || name == "os_readlink" ||
        name == "os_gethostname" || name == "os_popen") {
      inferAll();
      return typeString();
    }
    if (name == "os_getloadavg") {
      inferAll();
      std::vector<TypePtr> parts(3, typeFloat(64));
      return typeTuple(std::move(parts));
    }
    if (name == "os_ismount") {
      if (!arity(1, 1)) return typeError();
      inferAll();
      return typeBool();
    }
    if (name == "os_disk_usage") {
      if (!arity(1, 1)) return typeError();
      inferAll();
      std::vector<TypePtr> parts(3, typeInt(64, true));
      return typeTuple(std::move(parts));
    }
    if (name == "os_uname") {
      inferAll();
      std::vector<TypePtr> parts(5, typeString());
      return typeTuple(std::move(parts));
    }
    if (name == "os_stat" || name == "os_lstat") {
      if (!arity(1, 1)) return typeError();
      inferAll();
      // 0 mode,1 ino,2 dev,3 nlink,4 uid,5 gid,6 size,
      // 7 atime,8 mtime,9 ctime,10 is_file,11 is_dir,12 is_symlink
      std::vector<TypePtr> parts = {
          typeInt(64, true),  typeInt(64, true), typeInt(64, true),
          typeInt(64, true),  typeInt(64, true), typeInt(64, true),
          typeInt(64, true),  typeFloat(64),      typeFloat(64),
          typeFloat(64),        typeBool(),       typeBool(),
          typeBool()};
      return typeTuple(std::move(parts));
    }
    if (name == "os_access") {
      inferAll();
      return typeBool();
    }
    if (name == "os_environ") {
      return typeMap(typeString(), typeString());
    }
    if (name == "os_getenv") {
      inferAll();
      auto t = std::make_shared<Type>();
      t->kind = TypeKind::Var;
      t->allowUnresolved = true;
      return t;
    }
    if (name == "os_urandom") {
      inferAll();
      return typeArray(typeInt(64, true));
    }
    return typeError();
  }

  if (name == "now") {
    if (!call.args.empty()) {
      builtinArityError(name, call, 0);
      inferCallArgTypes(call);
    }
    return typeInt(64, true);
  }

  if (name == "sleep") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeVoid();
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The duration passed to 'sleep'");
    return typeVoid();
  }

  if (name == "panic") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeVoid();
    }
    TypePtr type = inferExpr(*call.args[0].value);
    if (!unify(type, typeString())) {
      reportMismatch(type, typeString(), call.args[0].value->span,
                     "in the message passed to 'panic'");
    }
    return typeVoid();
  }

  if (name == "assert") {
    if (call.args.empty() || call.args.size() > 2) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeVoid();
    }
    requireBool(inferExpr(*call.args[0].value), call.args[0].value->span,
                "The condition of 'assert'");
    if (call.args.size() == 2) {
      TypePtr type = inferExpr(*call.args[1].value);
      if (!unify(type, typeString())) {
        reportMismatch(type, typeString(), call.args[1].value->span,
                       "in the message passed to 'assert'");
      }
    }
    return typeVoid();
  }

  if (name == "assert_eq") {
    if (call.args.size() < 2 || call.args.size() > 3) {
      builtinArityError(name, call, 2);
      inferCallArgTypes(call);
      return typeVoid();
    }
    TypePtr left = inferExpr(*call.args[0].value);
    TypePtr right = inferExpr(*call.args[1].value);
    if (!unify(left, right)) {
      reportMismatch(right, left, call.args[1].value->span, "in the second argument of 'assert_eq'");
    }
    if (call.args.size() == 3) {
      TypePtr type = inferExpr(*call.args[2].value);
      if (!unify(type, typeString())) {
        reportMismatch(type, typeString(), call.args[2].value->span,
                       "in the message passed to 'assert_eq'");
      }
    }
    return typeVoid();
  }

  if (name == "terminal_isatty" || name == "terminal_raw_mode") {
    if (!call.args.empty()) inferCallArgTypes(call);
    return typeBool();
  }

  if (name == "terminal_read_char" || name == "terminal_read_char_ms") {
    if (!call.args.empty()) inferCallArgTypes(call);
    return typeOptional(typeString());
  }

  if (name == "terminal_write" || name == "terminal_flush") {
    if (!call.args.empty()) inferCallArgTypes(call);
    return typeVoid();
  }

  if (name == "terminal_size") {
    if (!call.args.empty()) inferCallArgTypes(call);
    return typeArray(typeInt(64, true));
  }

  if (name == "terminal_home") {
    if (!call.args.empty()) inferCallArgTypes(call);
    return typeString();
  }

  if (name == "env_get") {
    if (!call.args.empty()) inferCallArgTypes(call);
    return typeString();
  }

  if (name == "repl_eval") {
    if (!call.args.empty()) inferCallArgTypes(call);
    return typeInt(64, true);
  }

  if (name == "repl_quiet") {
    if (!call.args.empty()) inferCallArgTypes(call);
    return typeVoid();
  }

  if (name == "struct_name") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeString();
    }
    checkArgAgainst(*call.args[0].value, typeJson(), "in the value passed to 'struct_name'");
    return typeString();
  }

  if (name == "bytes_decode_utf8") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeOptional(typeString());
    }
    TypePtr arg = inferExpr(*call.args[0].value);
    TypePtr res = resolveType(arg);
    if (res->kind != TypeKind::Bytes)
      checkArgAgainst(*call.args[0].value, typeArray(typeInt(64, true)),
                      "as the bytes to decode");
    return typeOptional(typeString());
  }

  if (name == "str_encode_utf8") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeArray(typeInt(64, true));
    }
    checkArgAgainst(*call.args[0].value, typeString(), "as the string to encode");
    return typeArray(typeInt(64, true));
  }

  if (name == "struct_methods") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeArray(typeString());
    }
    checkArgAgainst(*call.args[0].value, typeJson(), "in the value passed to 'struct_methods'");
    return typeArray(typeString());
  }

  if (name == "fn_signature" || name == "fn_doc") {
    if (call.args.size() != 1) {
      builtinArityError(name, call, 1);
      inferCallArgTypes(call);
      return typeString();
    }
    inferAnyCallArg(call, 0, "to '" + name + "'");
    return typeString();
  }

  return nullptr;
}

}  // namespace a
