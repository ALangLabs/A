/* a_gpu.c — GPU offload runtime for `kernel fn` / `device fn`.
 *
 * Mojo-style: device code is embedded in the host binary (PTX text for
 * NVIDIA, code objects for AMD) and loaded through the *driver* APIs —
 * libcuda.so / libamdhip64 — so no CUDA/ROCm toolkit is required, only
 * the GPU driver. All entry points are safe to call on machines without
 * a GPU: they return -1 (or 0 for a_gpu_available) instead of crashing.
 *
 * The generated <obj>.offload.c registration unit calls
 * a_gpu_module_load() + a_gpu_register_kernel() from a constructor
 * before main; kernels are then launched by name through
 * a_gpu_launch_i / a_gpu_launch_f (fixed 8-slot scalar argument lists).
 */

#include "a_runtime_internal.h"

#include <string.h>

#if defined(_WIN32)
/* No dlopen on Windows; the LLVM backend is host-POSIX only today, but
 * keep the symbols linkable so generated offload units still build. */
long a_gpu_available(void) { return 0; }
long a_gpu_module_load(long kind, const void *data, long len) {
  (void)kind; (void)data; (void)len; return -1;
}
long a_gpu_register_kernel(long mod, const char *name, const char *ksym,
                           long nparams) {
  (void)mod; (void)name; (void)ksym; (void)nparams; return -1;
}
long a_gpu_launch_i(const char *name, long gx, long gy, long gz, long bx,
                    long by, long bz, long n, long a0, long a1, long a2,
                    long a3, long a4, long a5, long a6, long a7) {
  (void)name; (void)gx; (void)gy; (void)gz; (void)bx; (void)by; (void)bz;
  (void)n; (void)a0; (void)a1; (void)a2; (void)a3;
  (void)a4; (void)a5; (void)a6; (void)a7; return -1;
}
long a_gpu_launch_f(const char *name, long gx, long gy, long gz, long bx,
                    long by, long bz, long n, double a0, double a1, double a2,
                    double a3, double a4, double a5, double a6, double a7) {
  (void)name; (void)gx; (void)gy; (void)gz; (void)bx; (void)by; (void)bz;
  (void)n; (void)a0; (void)a1; (void)a2; (void)a3;
  (void)a4; (void)a5; (void)a6; (void)a7; return -1;
}
long a_gpu_alloc(long bytes) { (void)bytes; return -1; }
long a_gpu_free(long p) { (void)p; return -1; }
long a_gpu_memcpy_htod(long dst, long src, long n) {
  (void)dst; (void)src; (void)n; return -1;
}
long a_gpu_memcpy_dtoh(long dst, long src, long n) {
  (void)dst; (void)src; (void)n; return -1;
}
long a_gpu_synchronize(void) { return -1; }
#else

#include <dlfcn.h>
#include <stdint.h>
#include <stdlib.h>

/* ---- driver API bindings (resolved lazily via dlopen) ------------------ */

typedef int (*cuInit_t)(unsigned int);
typedef int (*cuDevicePrimaryCtxRetain_t)(void **, int);
typedef int (*cuCtxSetCurrent_t)(void *);
typedef int (*cuModuleLoadData_t)(void **, const void *);
typedef int (*cuModuleGetFunction_t)(void **, void *, const char *);
typedef int (*cuLaunchKernel_t)(void *, unsigned, unsigned, unsigned,
                                unsigned, unsigned, unsigned, unsigned,
                                void *, void **, void **);
typedef int (*cuMemAlloc_t)(unsigned long long *, size_t);
typedef int (*cuMemFree_t)(unsigned long long);
typedef int (*cuMemcpyHtoD_t)(unsigned long long, const void *, size_t);
typedef int (*cuMemcpyDtoH_t)(void *, unsigned long long, size_t);
typedef int (*cuCtxSynchronize_t)(void);

/* HIP equivalents (kind=2 device blobs): the HIP runtime exposes a
 * module API compatible enough for load+launch. */
typedef int (*hipModuleLoadData_t)(void **, const void *);
typedef int (*hipModuleGetFunction_t)(void **, void *, const char *);
typedef int (*hipLaunchKernel_t)(void *, unsigned, unsigned, unsigned,
                                 unsigned, unsigned, unsigned, unsigned,
                                 void *, void **, void **);
typedef int (*hipMalloc_t)(void **, size_t);
typedef int (*hipFree_t)(void *);
typedef int (*hipMemcpy_t)(void *, const void *, size_t, int);

static void *g_cuda = NULL, *g_hip = NULL;
static int g_backend = 0; /* 0 = none, 1 = CUDA, 2 = HIP */
static int g_tried = 0;

static cuInit_t p_cuInit;
static cuDevicePrimaryCtxRetain_t p_cuDevicePrimaryCtxRetain;
static cuCtxSetCurrent_t p_cuCtxSetCurrent;
static cuModuleLoadData_t p_cuModuleLoadData;
static cuModuleGetFunction_t p_cuModuleGetFunction;
static cuLaunchKernel_t p_cuLaunchKernel;
static cuMemAlloc_t p_cuMemAlloc;
static cuMemFree_t p_cuMemFree;
static cuMemcpyHtoD_t p_cuMemcpyHtoD;
static cuMemcpyDtoH_t p_cuMemcpyDtoH;
static cuCtxSynchronize_t p_cuCtxSynchronize;

static hipModuleLoadData_t p_hipModuleLoadData;
static hipModuleGetFunction_t p_hipModuleGetFunction;
static hipLaunchKernel_t p_hipLaunchKernel;
static hipMalloc_t p_hipMalloc;
static hipFree_t p_hipFree;
static hipMemcpy_t p_hipMemcpy;

#define CUDA_SYM(lib, name) ((name##_t)dlsym((lib), #name))

static int ensure_backend(void) {
  if (g_tried) return g_backend;
  g_tried = 1;
#ifdef __APPLE__
  static const char *const cudaNames[] = {"libcuda.dylib", NULL};
  static const char *const hipNames[] = {"libamdhip64.dylib", NULL};
#else
  static const char *const cudaNames[] = {
      "libcuda.so.1", "libcuda.so", NULL};
  static const char *const hipNames[] = {
      "libamdhip64.so.7", "libamdhip64.so", NULL};
#endif
  for (int i = 0; cudaNames[i] && !g_cuda; ++i)
    g_cuda = dlopen(cudaNames[i], RTLD_NOW | RTLD_LOCAL);
  for (int i = 0; hipNames[i] && !g_hip; ++i)
    g_hip = dlopen(hipNames[i], RTLD_NOW | RTLD_LOCAL);

  if (g_cuda) {
    p_cuInit = CUDA_SYM(g_cuda, cuInit);
    p_cuDevicePrimaryCtxRetain =
        CUDA_SYM(g_cuda, cuDevicePrimaryCtxRetain);
    p_cuCtxSetCurrent = CUDA_SYM(g_cuda, cuCtxSetCurrent);
    p_cuModuleLoadData = CUDA_SYM(g_cuda, cuModuleLoadData);
    p_cuModuleGetFunction = CUDA_SYM(g_cuda, cuModuleGetFunction);
    p_cuLaunchKernel = CUDA_SYM(g_cuda, cuLaunchKernel);
    p_cuMemAlloc = (cuMemAlloc_t)dlsym(g_cuda, "cuMemAlloc_v2");
    p_cuMemFree = (cuMemFree_t)dlsym(g_cuda, "cuMemFree_v2");
    p_cuMemcpyHtoD = (cuMemcpyHtoD_t)dlsym(g_cuda, "cuMemcpyHtoD_v2");
    p_cuMemcpyDtoH = (cuMemcpyDtoH_t)dlsym(g_cuda, "cuMemcpyDtoH_v2");
    p_cuCtxSynchronize = CUDA_SYM(g_cuda, cuCtxSynchronize);
    if (!p_cuMemAlloc) p_cuMemAlloc = CUDA_SYM(g_cuda, cuMemAlloc);
    if (!p_cuMemFree) p_cuMemFree = CUDA_SYM(g_cuda, cuMemFree);
    if (!p_cuMemcpyHtoD) p_cuMemcpyHtoD = CUDA_SYM(g_cuda, cuMemcpyHtoD);
    if (!p_cuMemcpyDtoH) p_cuMemcpyDtoH = CUDA_SYM(g_cuda, cuMemcpyDtoH);
    if (p_cuInit && p_cuDevicePrimaryCtxRetain && p_cuCtxSetCurrent &&
        p_cuModuleLoadData && p_cuModuleGetFunction && p_cuLaunchKernel &&
        p_cuMemAlloc && p_cuMemFree && p_cuMemcpyHtoD && p_cuMemcpyDtoH &&
        p_cuInit(0) == 0) {
      void *ctx = NULL;
      if (p_cuDevicePrimaryCtxRetain(&ctx, 0) == 0 && ctx)
        p_cuCtxSetCurrent(ctx);
      g_backend = 1;
      return 1;
    }
    g_cuda = NULL; /* symbols missing: not usable */
  }
  if (g_hip) {
    p_hipModuleLoadData = CUDA_SYM(g_hip, hipModuleLoadData);
    p_hipModuleGetFunction = CUDA_SYM(g_hip, hipModuleGetFunction);
    p_hipLaunchKernel = CUDA_SYM(g_hip, hipLaunchKernel);
    p_hipMalloc = CUDA_SYM(g_hip, hipMalloc);
    p_hipFree = CUDA_SYM(g_hip, hipFree);
    p_hipMemcpy = CUDA_SYM(g_hip, hipMemcpy);
    if (p_hipModuleLoadData && p_hipModuleGetFunction &&
        p_hipLaunchKernel && p_hipMalloc && p_hipFree && p_hipMemcpy) {
      g_backend = 2;
      return 2;
    }
    g_hip = NULL;
  }
  return 0;
}

/* ---- module + kernel registry ------------------------------------------ */

#define AGPU_MAX_MODULES 32
#define AGPU_MAX_KERNELS 256
#define AGPU_NAME_MAX 128

typedef struct {
  long id;
  int backend; /* 1 cuda, 2 hip */
  void *handle;
} AMod;

typedef struct {
  long mod;
  char name[AGPU_NAME_MAX];
  char ksym[AGPU_NAME_MAX * 2];
  long nparams;
  void *func; /* cached driver-side function handle */
} AKer;

static AMod g_mods[AGPU_MAX_MODULES];
static int g_nmods = 0;
static AKer g_kers[AGPU_MAX_KERNELS];
static int g_nkers = 0;

long a_gpu_available(void) { return ensure_backend() != 0; }

long a_gpu_module_load(long kind, const void *data, long len) {
  int be = ensure_backend();
  if (!be || len <= 0 || !data) return -1;
  /* kind 1 = PTX text (CUDA), kind 2 = code object (HIP/AMDGPU). HIP can
   * also take kind-1 fatbins; PTX is NVIDIA-only. */
  if (be == 1 && kind != 1) return -1;
  if (be == 2 && kind == 1) {
    /* PTX cannot load through HIP; only code objects (kind 2). */
    return -1;
  }
  if (g_nmods >= AGPU_MAX_MODULES) return -1;
  void *mod = NULL;
  int rc;
  if (be == 1)
    rc = p_cuModuleLoadData(&mod, data);
  else
    rc = p_hipModuleLoadData(&mod, data);
  if (rc != 0 || !mod) return -1;
  AMod *m = &g_mods[g_nmods];
  m->id = g_nmods;
  m->backend = be;
  m->handle = mod;
  return g_nmods++;
}

long a_gpu_register_kernel(long mod, const char *name, const char *ksym,
                           long nparams) {
  if (mod < 0 || mod >= g_nmods || !name || !ksym || g_nkers >= AGPU_MAX_KERNELS)
    return -1;
  AKer *k = &g_kers[g_nkers];
  k->mod = mod;
  strncpy(k->name, name, AGPU_NAME_MAX - 1);
  k->name[AGPU_NAME_MAX - 1] = '\0';
  strncpy(k->ksym, ksym, sizeof(k->ksym) - 1);
  k->ksym[sizeof(k->ksym) - 1] = '\0';
  k->nparams = nparams;
  k->func = NULL;
  return g_nkers++;
}

static AKer *find_kernel(const char *name) {
  for (int i = 0; i < g_nkers; ++i)
    if (strcmp(g_kers[i].name, name) == 0) return &g_kers[i];
  return NULL;
}

static int kernel_func(AKer *k, void **out) {
  if (k->func) { *out = k->func; return 0; }
  AMod *m = &g_mods[k->mod];
  void *f = NULL;
  int rc;
  if (m->backend == 1)
    rc = p_cuModuleGetFunction(&f, m->handle, k->ksym);
  else
    rc = p_hipModuleGetFunction(&f, m->handle, k->ksym);
  if (rc != 0 || !f) return -1;
  k->func = f;
  *out = f;
  return 0;
}

static long launch_common(const char *name, long gx, long gy, long gz,
                          long bx, long by, long bz, long n,
                          const uint64_t *slots) {
  AKer *k = find_kernel(name);
  if (!k) return -1;
  if (n > k->nparams) n = k->nparams;
  void *f = NULL;
  if (kernel_func(k, &f) != 0) return -1;
  void *params[16];
  if (n > 16) return -1;
  for (long i = 0; i < n; ++i) params[i] = (void *)(uintptr_t)&slots[i];
  AMod *m = &g_mods[k->mod];
  int rc;
  if (m->backend == 1)
    rc = p_cuLaunchKernel(f, (unsigned)gx, (unsigned)gy, (unsigned)gz,
                          (unsigned)bx, (unsigned)by, (unsigned)bz, 0,
                          NULL, params, NULL);
  else
    rc = p_hipLaunchKernel(f, (unsigned)gx, (unsigned)gy, (unsigned)gz,
                           (unsigned)bx, (unsigned)by, (unsigned)bz, 0,
                           NULL, params, NULL);
  return rc == 0 ? 0 : -1;
}

long a_gpu_launch_i(const char *name, long gx, long gy, long gz, long bx,
                    long by, long bz, long n, long a0, long a1, long a2,
                    long a3, long a4, long a5, long a6, long a7) {
  uint64_t slots[8] = {(uint64_t)a0, (uint64_t)a1, (uint64_t)a2,
                       (uint64_t)a3, (uint64_t)a4, (uint64_t)a5,
                       (uint64_t)a6, (uint64_t)a7};
  if (n > 8) n = 8;
  return launch_common(name, gx, gy, gz, bx, by, bz, n, slots);
}

long a_gpu_launch_f(const char *name, long gx, long gy, long gz, long bx,
                    long by, long bz, long n, double a0, double a1, double a2,
                    double a3, double a4, double a5, double a6, double a7) {
  double d[8] = {a0, a1, a2, a3, a4, a5, a6, a7};
  uint64_t slots[8];
  memcpy(slots, d, sizeof(slots));
  if (n > 8) n = 8;
  return launch_common(name, gx, gy, gz, bx, by, bz, n, slots);
}

long a_gpu_alloc(long bytes) {
  if (ensure_backend() == 1) {
    unsigned long long p = 0;
    return p_cuMemAlloc(&p, (size_t)bytes) == 0 ? (long)p : -1;
  }
  if (g_backend == 2) {
    void *p = NULL;
    return p_hipMalloc(&p, (size_t)bytes) == 0 ? (long)(uintptr_t)p : -1;
  }
  return -1;
}

long a_gpu_free(long p) {
  if (g_backend == 1)
    return p_cuMemFree((unsigned long long)p) == 0 ? 0 : -1;
  if (g_backend == 2)
    return p_hipFree((void *)(uintptr_t)p) == 0 ? 0 : -1;
  return -1;
}

long a_gpu_memcpy_htod(long dst, long src, long n) {
  if (g_backend == 1)
    return p_cuMemcpyHtoD((unsigned long long)dst, (const void *)(uintptr_t)src,
                          (size_t)n) == 0
               ? 0
               : -1;
  if (g_backend == 2)
    /* hipMemcpy kind 1 = HostToDevice */
    return p_hipMemcpy((void *)(uintptr_t)dst, (const void *)(uintptr_t)src,
                       (size_t)n, 1) == 0
               ? 0
               : -1;
  return -1;
}

long a_gpu_memcpy_dtoh(long dst, long src, long n) {
  if (g_backend == 1)
    return p_cuMemcpyDtoH((void *)(uintptr_t)dst,
                          (unsigned long long)src, (size_t)n) == 0
               ? 0
               : -1;
  if (g_backend == 2)
    /* hipMemcpy kind 2 = DeviceToHost */
    return p_hipMemcpy((void *)(uintptr_t)dst, (const void *)(uintptr_t)src,
                       (size_t)n, 2) == 0
               ? 0
               : -1;
  return -1;
}

long a_gpu_synchronize(void) {
  if (g_backend == 1 && p_cuCtxSynchronize) return p_cuCtxSynchronize() == 0 ? 0 : -1;
  return 0;
}

#endif /* _WIN32 */
