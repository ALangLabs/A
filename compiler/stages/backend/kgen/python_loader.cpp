// Runtime CPython discovery + dlopen, Mojo-style: instead of linking one
// libpython at build time, `a` finds the Python environment that actually has
// the packages you import — A_PYTHON / A_PYTHON_LIB overrides, project
// ./.venv & venv, VIRTUAL_ENV / UV_PROJECT_ENVIRONMENT / CONDA_PREFIX, PATH
// pythons, uv-managed installs, then system libraries as a last resort.
#include "backend/kgen/python_symbols.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace a::python::detail {

PythonRuntimeInfo g_pythonRuntime{};

#ifdef A_HAVE_PYTHON

namespace fs = std::filesystem;

PySymbols g_pySymbols{};

namespace {

struct Probe {
  std::string exe;                 // interpreter that produced this probe
  std::string version;             // sys.version split
  std::string home;                // sys.base_prefix (real install, not venv)
  std::vector<std::string> siteDirs;  // purelib, platlib, stdlib
  std::vector<fs::path> libs;      // libpython candidates, best first
};

bool hasModule(const Probe& p, const std::string& top) {
  if (top.empty()) return false;
  std::error_code ec;
  for (const std::string& dir : p.siteDirs) {
    if (dir.empty()) continue;
    fs::path base(dir);
    if (fs::is_directory(base / top, ec)) return true;
    if (fs::is_regular_file(base / (top + ".py"), ec)) return true;
  }
  return false;
}

// Top-level module name for `import python:a.b.c` -> "a".
std::string topLevelModule(const std::string& name) {
  std::string top = name.substr(0, name.find('.'));
  bool ok = !top.empty();
  for (char c : top) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') ok = false;
  }
  return ok ? top : std::string();
}

// Runs `<exe> -c <probe>` and returns its stdout, or "" on failure.
std::string runProbe(const std::string& exe) {
  static const char* kCode =
      "import sys,sysconfig\n"
      "print(sys.version.split()[0])\n"
      "print(sysconfig.get_config_var(\"LIBDIR\") or \"\")\n"
      "print(sysconfig.get_config_var(\"LDLIBRARY\") or \"\")\n"
      "print(sysconfig.get_paths()[\"purelib\"])\n"
      "print(sysconfig.get_paths()[\"platlib\"])\n"
      "print(sysconfig.get_paths()[\"stdlib\"])\n"
      "print(sys.base_prefix)\n";
  if (exe.find_first_of("\"'$`\\") != std::string::npos) return {};
  std::string cmd = "\"" + exe + "\" -c '" + kCode + "' 2>/dev/null";
  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) return {};
  std::string out;
  char buf[512];
  while (fgets(buf, sizeof buf, pipe)) out += buf;
  pclose(pipe);
  return out;
}

// Splits probe output into exactly the 7 expected lines.
bool parseProbe(const std::string& exe, const std::string& out, Probe& p) {
  std::vector<std::string> lines;
  std::size_t pos = 0;
  while (lines.size() < 8 && pos <= out.size()) {
    std::size_t nl = out.find('\n', pos);
    if (nl == std::string::npos) nl = out.size();
    lines.push_back(out.substr(pos, nl - pos));
    pos = nl + 1;
  }
  if (lines.size() < 8 || lines[0].empty()) return false;
  p.exe = exe;
  p.version = lines[0];
  p.home = lines[6];
  std::string libDir = lines[1];
  std::string ldLibrary = lines[2];
  for (int i = 3; i <= 5; ++i) {
    if (!lines[i].empty()) p.siteDirs.push_back(lines[i]);
  }

  // Candidate libpython paths: LDLIBRARY first, then every libpython3*.so*
  // in LIBDIR (covers builds where LDLIBRARY is empty or points at a .a).
  std::error_code ec;
  if (!ldLibrary.empty()) {
    fs::path cand(ldLibrary);
    if (!cand.is_absolute() && !libDir.empty()) cand = fs::path(libDir) / cand;
    if (cand.extension() == ".a") {
      fs::path so = cand;
      so.replace_extension(".so");
      if (fs::exists(so, ec)) p.libs.push_back(so);
    } else {
      p.libs.push_back(cand);
    }
  }
  if (!libDir.empty() && fs::is_directory(libDir, ec)) {
    for (const auto& entry : fs::directory_iterator(libDir, ec)) {
      std::string name = entry.path().filename().string();
      if (name.rfind("libpython3", 0) != 0 || name.find(".so") == std::string::npos)
        continue;
      if (!entry.is_regular_file(ec)) continue;
      if (std::find(p.libs.begin(), p.libs.end(), entry.path()) == p.libs.end())
        p.libs.push_back(entry.path());
    }
  }
  std::sort(p.libs.begin(), p.libs.end());
  // Keep the LDLIBRARY-derived candidate first (it was pushed before sorting
  // only if it sorted earlier; re-pin it explicitly).
  if (!ldLibrary.empty() && !libDir.empty()) {
    fs::path pinned(ldLibrary);
    if (!pinned.is_absolute()) pinned = fs::path(libDir) / pinned;
    if (pinned.extension() == ".a") pinned.replace_extension(".so");
    auto it = std::find(p.libs.begin(), p.libs.end(), pinned);
    if (it != p.libs.end() && it != p.libs.begin())
      std::rotate(p.libs.begin(), it, it + 1);
  }
  return !p.libs.empty();
}

void addVenvExe(std::vector<std::string>& out, const char* dir) {
  if (!dir || !*dir) return;
  fs::path base(dir);
  for (const char* name : {"python3", "python"}) {
    fs::path exe = base / "bin" / name;
    std::error_code ec;
    if (fs::is_regular_file(exe, ec)) out.push_back(exe.string());
  }
}

// uv-managed CPython installs live under ~/.local/share/uv/python/*/bin.
void addUvPythons(std::vector<std::string>& out) {
  const char* overrideDir = std::getenv("UV_PYTHON_INSTALL_DIR");
  fs::path root = overrideDir && *overrideDir
                      ? fs::path(overrideDir)
                      : (fs::path(std::getenv("HOME") ? std::getenv("HOME") : "") /
                         ".local/share/uv/python");
  std::error_code ec;
  if (!fs::is_directory(root, ec)) return;
  std::vector<fs::path> found;
  for (const auto& entry : fs::directory_iterator(root, ec)) {
    if (!entry.is_directory(ec)) continue;
    fs::path bin = entry.path() / "bin";
    if (!fs::is_directory(bin, ec)) continue;
    for (const auto& f : fs::directory_iterator(bin, ec)) {
      std::string name = f.path().filename().string();
      if (name.rfind("python3", 0) != 0 || name.find("config") != std::string::npos)
        continue;
      if (f.is_regular_file(ec)) found.push_back(f.path());
    }
  }
  // Prefer newer interpreters: parse "cpython-X.Y..." from the install dir.
  auto versionKey = [](const fs::path& p) {
    std::string dir = p.parent_path().parent_path().filename().string();
    int major = 0, minor = 0;
    std::sscanf(dir.c_str(), "cpython-%d.%d", &major, &minor);
    return std::pair<int, int>{major, minor};
  };
  std::sort(found.begin(), found.end(),
            [&](const fs::path& a, const fs::path& b) {
              return versionKey(a) > versionKey(b);
            });
  for (const auto& p : found) out.push_back(p.string());
}

std::vector<Probe> gatherProbes() {
  std::vector<Probe> probes;
  std::vector<std::string> exes;

  const char* forced = std::getenv("A_PYTHON");
  if (forced && *forced) {
    exes.push_back(forced);  // interpreter path (or a libpython*.so path)
  } else {
    // Project environments first: the venv in the cwd is the most specific.
    for (const char* venv : {".venv", "venv"}) {
      fs::path base(venv);
      for (const char* name : {"python3", "python"}) {
        fs::path exe = base / "bin" / name;
        std::error_code ec;
        if (fs::is_regular_file(exe, ec)) exes.push_back(exe.string());
      }
    }
    addVenvExe(exes, std::getenv("VIRTUAL_ENV"));
    addVenvExe(exes, std::getenv("UV_PROJECT_ENVIRONMENT"));
    addVenvExe(exes, std::getenv("CONDA_PREFIX"));
    exes.push_back("python3");
    exes.push_back("python");
    addUvPythons(exes);
    for (const char* sys : {"/usr/bin/python3", "/usr/local/bin/python3",
                            "/usr/bin/python"}) {
      std::error_code ec;
      if (fs::is_regular_file(sys, ec)) exes.push_back(sys);
    }
  }

  std::vector<std::string> seenLibs;
  for (const std::string& exe : exes) {
    // A_PYTHON may point straight at a libpython shared object.
    std::string base = fs::path(exe).filename().string();
    if (base.find("libpython") != std::string::npos &&
        base.find(".so") != std::string::npos) {
      std::error_code ec;
      if (!fs::is_regular_file(exe, ec)) continue;
      Probe p;
      p.exe = exe;
      p.libs.push_back(exe);
      probes.push_back(std::move(p));
      continue;
    }
    std::string out = runProbe(exe);
    if (out.empty()) continue;
    Probe p;
    if (!parseProbe(exe, out, p)) continue;
    // Skip probes that resolve to a libpython we already have.
    if (!p.libs.empty() &&
        std::find(seenLibs.begin(), seenLibs.end(),
                  p.libs.front().string()) != seenLibs.end())
      continue;
    if (!p.libs.empty()) seenLibs.push_back(p.libs.front().string());
    probes.push_back(std::move(p));
  }

  // Explicit libpython override.
  const char* libEnv = std::getenv("A_PYTHON_LIB");
  if (libEnv && *libEnv) {
    std::error_code ec;
    if (fs::is_regular_file(libEnv, ec)) {
      Probe p;
      p.exe = libEnv;
      p.libs.push_back(libEnv);
      probes.insert(probes.begin(), std::move(p));
    }
  }

  // Last resort: system libpython shared objects with no interpreter probe.
  for (const char* dir : {"/usr/lib/x86_64-linux-gnu", "/usr/lib",
                          "/usr/local/lib", "/lib/x86_64-linux-gnu"}) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) continue;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
      std::string name = entry.path().filename().string();
      if (name.rfind("libpython3", 0) != 0 ||
          name.find(".so") == std::string::npos ||
          !entry.is_regular_file(ec))
        continue;
      bool dup = false;
      for (const auto& p : probes)
        if (std::find(p.libs.begin(), p.libs.end(), entry.path()) != p.libs.end())
          dup = true;
      if (!dup) {
        Probe p;
        p.exe = entry.path().string();
        p.libs.push_back(entry.path());
        probes.push_back(std::move(p));
      }
    }
  }
  return probes;
}

template <typename T>
bool loadSym(void* handle, const char* name, T& slot) {
  void* sym = dlsym(handle, name);
  if (!sym) return false;
  slot = reinterpret_cast<T>(sym);
  return true;
}

bool fillSymbols(void* handle) {
  auto& g = g_pySymbols;
  bool ok = true;
#define A_PY_SYM(n) ok = loadSym(handle, #n, g.sym_##n) && ok
  A_PY_SYM(Py_Initialize);
  A_PY_SYM(PyGILState_Ensure);
  A_PY_SYM(PyGILState_Release);
  A_PY_SYM(PyImport_ImportModule);
  A_PY_SYM(PyObject_GetAttrString);
  A_PY_SYM(PyObject_GetItem);
  A_PY_SYM(PyObject_Call);
  A_PY_SYM(PyObject_CallFunction);
  A_PY_SYM(PyObject_RichCompareBool);
  A_PY_SYM(PyObject_Str);
  A_PY_SYM(PyObject_Repr);
  A_PY_SYM(PyObject_GetIter);
  A_PY_SYM(PyIter_Next);
  A_PY_SYM(PyErr_Occurred);
  A_PY_SYM(PyErr_Clear);
  A_PY_SYM(PyErr_Fetch);
  A_PY_SYM(PyErr_NormalizeException);
  A_PY_SYM(PySys_GetObject);
  A_PY_SYM(PyUnicode_AsUTF8);
  A_PY_SYM(PyUnicode_AsUTF8AndSize);
  A_PY_SYM(PyUnicode_DecodeUTF8);
  A_PY_SYM(PyUnicode_DecodeFSDefault);
  A_PY_SYM(PyUnicode_FromString);
  A_PY_SYM(PyLong_AsLongLongAndOverflow);
  A_PY_SYM(PyLong_FromLongLong);
  A_PY_SYM(PyFloat_AsDouble);
  A_PY_SYM(PyFloat_FromDouble);
  A_PY_SYM(PyList_New);
  A_PY_SYM(PyList_Size);
  A_PY_SYM(PyList_GetItem);
  A_PY_SYM(PyList_Append);
  A_PY_SYM(PyTuple_New);
  A_PY_SYM(PyTuple_Size);
  A_PY_SYM(PyTuple_GetItem);
  A_PY_SYM(PyDict_New);
  A_PY_SYM(PyDict_SetItem);
  A_PY_SYM(PyDict_SetItemString);
  A_PY_SYM(PyDict_Next);
  A_PY_SYM(PySet_New);
  A_PY_SYM(PySet_Add);
  A_PY_SYM(PyBytes_AsString);
  A_PY_SYM(PyBytes_Size);
  A_PY_SYM(PyType_IsSubtype);
  A_PY_SYM(PyCallable_Check);
  A_PY_SYM(PyNumber_Add);
  A_PY_SYM(PyNumber_Subtract);
  A_PY_SYM(PyNumber_Multiply);
  A_PY_SYM(PyNumber_TrueDivide);
  A_PY_SYM(PyNumber_Remainder);
  A_PY_SYM(PyNumber_Power);
  A_PY_SYM(PyNumber_And);
  A_PY_SYM(PyNumber_Or);
  A_PY_SYM(PyNumber_Xor);
  A_PY_SYM(PyNumber_Lshift);
  A_PY_SYM(PyNumber_Rshift);
  A_PY_SYM(PySequence_Contains);
  A_PY_SYM(PyNumber_Index);
  A_PY_SYM(PyNumber_Float);
  A_PY_SYM(Py_IncRef);
  A_PY_SYM(Py_DecRef);
  A_PY_SYM(_Py_NoneStruct);
  A_PY_SYM(_Py_TrueStruct);
  A_PY_SYM(_Py_FalseStruct);
  A_PY_SYM(PyBool_Type);
  A_PY_SYM(PyLong_Type);
  A_PY_SYM(PyFloat_Type);
  A_PY_SYM(PyUnicode_Type);
  A_PY_SYM(PyBytes_Type);
  A_PY_SYM(PyList_Type);
  A_PY_SYM(PyTuple_Type);
  A_PY_SYM(PyDict_Type);
  A_PY_SYM(PySet_Type);
  A_PY_SYM(PyFrozenSet_Type);
  A_PY_SYM(PyRange_Type);
#undef A_PY_SYM
  return ok;
}

}  // namespace

const PythonRuntimeInfo& pythonRuntimeInfo() { return g_pythonRuntime; }

bool ensurePythonLoaded(const std::string& moduleHint) {
  if (g_pythonRuntime.status != 0) return g_pythonRuntime.status == 1;

  std::string top = topLevelModule(moduleHint);
  std::vector<Probe> probes = gatherProbes();
  // Environments that actually contain the requested module win; the rest
  // keep their discovery order as fallback (covers builtins/stdlib modules).
  std::stable_partition(probes.begin(), probes.end(),
                        [&](const Probe& p) { return hasModule(p, top); });

  std::string tried;
  for (const Probe& probe : probes) {
    for (const fs::path& lib : probe.libs) {
      void* handle = dlopen(lib.c_str(), RTLD_NOW | RTLD_GLOBAL);
      if (!handle) {
        tried += lib.string() + ": " + dlerror() + "; ";
        continue;
      }
      if (fillSymbols(handle)) {
        g_pythonRuntime.status = 1;
        g_pythonRuntime.siteDirs = probe.siteDirs;
        g_pythonRuntime.pythonHome = probe.home;
        g_pythonRuntime.description =
            "CPython " + probe.version + " (" + lib.string() + ")";
        if (!probe.exe.empty() && probe.exe != lib.string())
          g_pythonRuntime.description += " via " + probe.exe;
        return true;
      }
      tried += lib.string() + ": missing symbols; ";
      dlclose(handle);
    }
  }

  g_pythonRuntime.status = 2;
  g_pythonRuntime.error =
      "no usable libpython found" + (tried.empty() ? std::string()
                                                 : " (" + tried + ")");
  return false;
}

#else   // !A_HAVE_PYTHON
const PythonRuntimeInfo& pythonRuntimeInfo() { return g_pythonRuntime; }
bool ensurePythonLoaded(const std::string&) { return false; }
#endif  // A_HAVE_PYTHON

}  // namespace a::python::detail

#ifdef A_HAVE_PYTHON
// Forwarders for CPython entry points that header-defined inline helpers
// (e.g. PyObject_TypeCheck) call by their real symbol names — those calls are
// bound when Python.h is parsed, before python_shim.hpp's macros exist, so
// they need a real definition that forwards into the dlopened libpython.
extern "C" int PyType_IsSubtype(PyTypeObject* a, PyTypeObject* b) {
  return a::python::detail::g_pySymbols.sym_PyType_IsSubtype(a, b);
}
extern "C" int PyCallable_Check(PyObject* o) {
  return a::python::detail::g_pySymbols.sym_PyCallable_Check(o);
}
#endif
