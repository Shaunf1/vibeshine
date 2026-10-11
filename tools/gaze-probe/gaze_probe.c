/**
 * @file tools/gaze-probe/gaze_probe.c
 * @brief Steam Frame gaze probe: reads XR_EXT_eye_gaze_interaction and sends samples over UDP.
 *
 * This answers open hardware check 1 in docs/plans/eye-tracked-foveated-streaming.md:
 * can a separate OpenXR program read gaze while a flat Moonlight window is open?
 *
 * Session strategy, cheapest first (see --mode):
 *   1. headless : XR_MND_headless. No graphics, no frame loop, cannot cover the display.
 *   2. overlay  : XR_EXTX_overlay on top of an EGL/OpenGL session. Submits no layers.
 *   3. egl      : plain EGL/OpenGL session (XR_KHR_opengl_enable + XR_MNDX_egl_enable).
 *                 Submits no layers, but the runtime may still treat it as the scene app.
 *
 * There is no OpenXR loader dependency: the probe reads the active runtime manifest and
 * dlopens the runtime itself (see load_runtime). EGL is also dlopened and declared locally.
 * So the build needs only a C compiler and the OpenXR headers (see build.sh).
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netdb.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define XR_USE_TIMESPEC
#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_platform.h>

#include "gaze_packet.h"

/* ---- Minimal runtime loader --------------------------------------------------------------- */

#define XR_FUNCS(X) \
  X(xrResultToString) \
  X(xrEnumerateInstanceExtensionProperties) \
  X(xrCreateInstance) \
  X(xrDestroyInstance) \
  X(xrGetInstanceProperties) \
  X(xrGetSystem) \
  X(xrGetSystemProperties) \
  X(xrEnumerateViewConfigurations) \
  X(xrEnumerateEnvironmentBlendModes) \
  X(xrCreateSession) \
  X(xrDestroySession) \
  X(xrBeginSession) \
  X(xrEndSession) \
  X(xrRequestExitSession) \
  X(xrPollEvent) \
  X(xrCreateActionSet) \
  X(xrCreateAction) \
  X(xrStringToPath) \
  X(xrPathToString) \
  X(xrSuggestInteractionProfileBindings) \
  X(xrAttachSessionActionSets) \
  X(xrGetCurrentInteractionProfile) \
  X(xrCreateActionSpace) \
  X(xrCreateReferenceSpace) \
  X(xrSyncActions) \
  X(xrGetActionStatePose) \
  X(xrLocateSpace) \
  X(xrWaitFrame) \
  X(xrBeginFrame) \
  X(xrEndFrame)

#define X(name) static PFN_##name p_##name;
XR_FUNCS(X)
#undef X
static PFN_xrGetInstanceProcAddr p_xrGetInstanceProcAddr;

// The rest of the file calls the usual names.
#define xrGetInstanceProcAddr p_xrGetInstanceProcAddr
#define xrResultToString p_xrResultToString
#define xrEnumerateInstanceExtensionProperties p_xrEnumerateInstanceExtensionProperties
#define xrCreateInstance p_xrCreateInstance
#define xrDestroyInstance p_xrDestroyInstance
#define xrGetInstanceProperties p_xrGetInstanceProperties
#define xrGetSystem p_xrGetSystem
#define xrGetSystemProperties p_xrGetSystemProperties
#define xrEnumerateViewConfigurations p_xrEnumerateViewConfigurations
#define xrEnumerateEnvironmentBlendModes p_xrEnumerateEnvironmentBlendModes
#define xrCreateSession p_xrCreateSession
#define xrDestroySession p_xrDestroySession
#define xrBeginSession p_xrBeginSession
#define xrEndSession p_xrEndSession
#define xrRequestExitSession p_xrRequestExitSession
#define xrPollEvent p_xrPollEvent
#define xrCreateActionSet p_xrCreateActionSet
#define xrCreateAction p_xrCreateAction
#define xrStringToPath p_xrStringToPath
#define xrPathToString p_xrPathToString
#define xrSuggestInteractionProfileBindings p_xrSuggestInteractionProfileBindings
#define xrAttachSessionActionSets p_xrAttachSessionActionSets
#define xrGetCurrentInteractionProfile p_xrGetCurrentInteractionProfile
#define xrCreateActionSpace p_xrCreateActionSpace
#define xrCreateReferenceSpace p_xrCreateReferenceSpace
#define xrSyncActions p_xrSyncActions
#define xrGetActionStatePose p_xrGetActionStatePose
#define xrLocateSpace p_xrLocateSpace
#define xrWaitFrame p_xrWaitFrame
#define xrBeginFrame p_xrBeginFrame
#define xrEndFrame p_xrEndFrame

/** Reads a small file into a NUL-terminated buffer. Caller frees. */
static char *read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    return NULL;
  }
  char *buf = calloc(1, 65537);
  size_t n = fread(buf, 1, 65536, f);
  buf[n] = 0;
  fclose(f);
  return buf;
}

/** Finds the active runtime manifest the way the Khronos loader does on Linux. */
static bool find_manifest(char *out, size_t out_len) {
  const char *env = getenv("XR_RUNTIME_JSON");
  if (env && *env) {
    snprintf(out, out_len, "%s", env);
    return access(out, R_OK) == 0;
  }
#if defined(__aarch64__)
  const char *arch_name = "active_runtime.aarch64.json";
#elif defined(__x86_64__)
  const char *arch_name = "active_runtime.x86_64.json";
#else
  const char *arch_name = "active_runtime.json";
#endif
  const char *names[] = {arch_name, "active_runtime.json"};
  char dirs[8][1024];
  int nd = 0;
  const char *xdg_config = getenv("XDG_CONFIG_HOME");
  const char *home = getenv("HOME");
  if (xdg_config && *xdg_config) {
    snprintf(dirs[nd++], sizeof(dirs[0]), "%s", xdg_config);
  } else if (home) {
    snprintf(dirs[nd++], sizeof(dirs[0]), "%s/.config", home);
  }
  const char *xdg_dirs = getenv("XDG_CONFIG_DIRS");
  char tmp[4096];
  snprintf(tmp, sizeof(tmp), "%s", xdg_dirs && *xdg_dirs ? xdg_dirs : "/etc/xdg");
  for (char *save = NULL, *d = strtok_r(tmp, ":", &save); d && nd < 7; d = strtok_r(NULL, ":", &save)) {
    snprintf(dirs[nd++], sizeof(dirs[0]), "%s", d);
  }
  snprintf(dirs[nd++], sizeof(dirs[0]), "/etc");
  for (int i = 0; i < nd; i++) {
    for (int j = 0; j < 2; j++) {
      snprintf(out, out_len, "%s/openxr/1/%s", dirs[i], names[j]);
      if (access(out, R_OK) == 0) {
        return true;
      }
    }
  }
  return false;
}

/** Extracts "library_path" from a runtime manifest and resolves it against the manifest's folder. */
static bool manifest_library(const char *manifest, char *out, size_t out_len) {
  char *json = read_file(manifest);
  if (!json) {
    return false;
  }
  bool ok = false;
  char *k = strstr(json, "\"library_path\"");
  char *q = k ? strchr(k + 14, ':') : NULL;
  q = q ? strchr(q, '"') : NULL;
  char *e = q ? strchr(q + 1, '"') : NULL;
  if (q && e) {
    *e = 0;
    const char *lib = q + 1;
    if (lib[0] == '/' || !strchr(lib, '/')) {
      snprintf(out, out_len, "%s", lib);  // Absolute, or a bare name for the dlopen search path.
    } else {
      char dir[4096];
      snprintf(dir, sizeof(dir), "%s", manifest);
      char *slash = strrchr(dir, '/');
      if (slash) {
        *slash = 0;
      }
      snprintf(out, out_len, "%s/%s", dir, lib);
    }
    ok = true;
  }
  free(json);
  return ok;
}

static bool load_runtime(void) {
  char manifest[4096], lib[8192];
  if (!find_manifest(manifest, sizeof(manifest))) {
    fprintf(stderr, "FAIL: no OpenXR runtime manifest found. Set XR_RUNTIME_JSON to SteamVR's runtime json (see README).\n");
    return false;
  }
  if (!manifest_library(manifest, lib, sizeof(lib))) {
    fprintf(stderr, "FAIL: no library_path in %s\n", manifest);
    return false;
  }
  printf("Runtime manifest: %s\nRuntime library: %s\n", manifest, lib);
  void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    fprintf(stderr, "FAIL: dlopen runtime: %s\n", dlerror());
    return false;
  }
  PFN_xrNegotiateLoaderRuntimeInterface negotiate;
  *(void **) &negotiate = dlsym(h, "xrNegotiateLoaderRuntimeInterface");
  if (!negotiate) {
    fprintf(stderr, "FAIL: runtime does not export xrNegotiateLoaderRuntimeInterface\n");
    return false;
  }
  XrNegotiateLoaderInfo li = {0};
  li.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
  li.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
  li.structSize = sizeof(li);
  li.minInterfaceVersion = 1;
  li.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
  li.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
  li.maxApiVersion = XR_MAKE_VERSION(1, 0x3ff, 0xfff);
  XrNegotiateRuntimeRequest rr = {0};
  rr.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
  rr.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
  rr.structSize = sizeof(rr);
  XrResult r = negotiate(&li, &rr);
  if (XR_FAILED(r) || !rr.getInstanceProcAddr) {
    fprintf(stderr, "FAIL: runtime negotiation failed (%d)\n", (int) r);
    return false;
  }
  p_xrGetInstanceProcAddr = rr.getInstanceProcAddr;
  // These two may be fetched with a null instance.
  p_xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction *) &p_xrEnumerateInstanceExtensionProperties);
  p_xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction *) &p_xrCreateInstance);
  return p_xrEnumerateInstanceExtensionProperties && p_xrCreateInstance;
}

/** Loads the remaining entry points once the instance exists. */
static bool load_instance_functions(XrInstance instance) {
  bool ok = true;
#define X(name) \
  if (!p_##name && XR_FAILED(p_xrGetInstanceProcAddr(instance, #name, (PFN_xrVoidFunction *) &p_##name))) { \
    fprintf(stderr, "FAIL: runtime lacks %s\n", #name); \
    ok = false; \
  }
  XR_FUNCS(X)
#undef X
  return ok;
}

/* ---- Local declarations that would otherwise need GL/EGL headers ---------------------------- */

typedef void *egl_display_t;
typedef void *egl_config_t;
typedef void *egl_context_t;
typedef void (*egl_proc_t)(void);

typedef struct {
  XrStructureType type;
  void *next;
  XrVersion minApiVersionSupported;
  XrVersion maxApiVersionSupported;
} probe_gl_requirements_t;  // == XrGraphicsRequirementsOpenGLKHR

typedef XrResult(XRAPI_PTR *probe_get_gl_requirements_t)(XrInstance, XrSystemId, probe_gl_requirements_t *);

typedef struct {
  XrStructureType type;
  const void *next;
  egl_proc_t (*getProcAddress)(const char *);
  egl_display_t display;
  egl_config_t config;
  egl_context_t context;
} probe_egl_binding_t;  // == XrGraphicsBindingEGLMNDX

// Extension names that openxr_platform.h only declares with GL/EGL headers enabled.
#define PROBE_KHR_OPENGL_ENABLE "XR_KHR_opengl_enable"
#define PROBE_MNDX_EGL_ENABLE "XR_MNDX_egl_enable"

#define EGL_NONE 0x3038
#define EGL_SURFACE_TYPE 0x3033
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_BIT 0x0008
#define EGL_OPENGL_API 0x30A2
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#define EGL_CONTEXT_MINOR_VERSION 0x30FB
#define EGL_CONTEXT_OPENGL_PROFILE_MASK 0x30FD
#define EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT 0x0001
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD

static struct {
  void *lib;
  egl_proc_t (*GetProcAddress)(const char *);
  egl_display_t (*GetDisplay)(void *);
  egl_display_t (*GetPlatformDisplayEXT)(unsigned, void *, const int *);
  unsigned (*Initialize)(egl_display_t, int *, int *);
  unsigned (*BindAPI)(unsigned);
  unsigned (*ChooseConfig)(egl_display_t, const int *, egl_config_t *, int, int *);
  egl_context_t (*CreateContext)(egl_display_t, egl_config_t, egl_context_t, const int *);
  unsigned (*MakeCurrent)(egl_display_t, void *, void *, egl_context_t);
  int (*GetError)(void);
  egl_display_t dpy;
  egl_config_t cfg;
  egl_context_t ctx;
} egl;

/* ---- Options and globals ----------------------------------------------------------------- */

typedef enum { MODE_AUTO, MODE_HEADLESS, MODE_OVERLAY, MODE_EGL } probe_mode_t;

static struct {
  const char *host;
  const char *port;
  probe_mode_t mode;
  bool view_space;  // true: head-relative (VIEW), false: LOCAL
  double rate_hz;  // headless pacing
  bool quiet;  // print once per second instead of per sample
  double duration_s;  // 0 = until Ctrl+C
  bool force;  // keep going when the system reports no eye gaze support (testing on other runtimes)
} opt = {NULL, NULL, MODE_AUTO, true, 90.0, false, 0.0, false};

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
  (void) sig;
  g_stop = 1;
}

static XrInstance g_instance = XR_NULL_HANDLE;

static const char *xr_str(XrResult r) {
  static char buf[XR_MAX_RESULT_STRING_SIZE];
  if (g_instance == XR_NULL_HANDLE || !p_xrResultToString || XR_FAILED(xrResultToString(g_instance, r, buf))) {
    snprintf(buf, sizeof(buf), "XrResult(%d)", (int) r);
  }
  return buf;
}

#define XR_CHECK(call) \
  do { \
    XrResult r_ = (call); \
    if (XR_FAILED(r_)) { \
      fprintf(stderr, "FAIL: %s -> %s\n", #call, xr_str(r_)); \
      return false; \
    } \
  } while (0)

static uint64_t realtime_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
}

static double mono_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double) ts.tv_sec + (double) ts.tv_nsec * 1e-9;
}

/* ---- UDP --------------------------------------------------------------------------------- */

static int g_sock = -1;
static struct sockaddr_storage g_dest;
static socklen_t g_dest_len;
static uint32_t g_rtt_us = 0;
static uint32_t g_acks = 0;

static bool udp_open(void) {
  struct addrinfo hints = {0}, *res = NULL;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  int rc = getaddrinfo(opt.host, opt.port, &hints, &res);
  if (rc != 0) {
    fprintf(stderr, "FAIL: cannot resolve %s:%s: %s\n", opt.host, opt.port, gai_strerror(rc));
    return false;
  }
  g_sock = socket(res->ai_family, SOCK_DGRAM, 0);
  if (g_sock < 0) {
    perror("FAIL: socket");
    freeaddrinfo(res);
    return false;
  }
  memcpy(&g_dest, res->ai_addr, res->ai_addrlen);
  g_dest_len = res->ai_addrlen;
  freeaddrinfo(res);
  fcntl(g_sock, F_SETFL, fcntl(g_sock, F_GETFL) | O_NONBLOCK);
  int one = 1;
  setsockopt(g_sock, SOL_SOCKET, SO_TIMESTAMPNS, &one, sizeof(one));
  return true;
}

static void udp_drain_acks(void) {
  for (;;) {
    gaze_ack_packet_t ack;
    struct iovec iov = {&ack, sizeof(ack)};
    union {
      char buf[CMSG_SPACE(sizeof(struct timespec))];
      struct cmsghdr align;
    } ctrl;
    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl.buf;
    msg.msg_controllen = sizeof(ctrl.buf);
    ssize_t n = recvmsg(g_sock, &msg, 0);
    if (n < 0) {
      break;
    }
    if (n != (ssize_t) sizeof(ack) || memcmp(ack.magic, GAZE_ACK_MAGIC, 4) != 0) {
      continue;
    }
    // Kernel arrival time, so the RTT does not include the wait until the next frame.
    uint64_t arrived = realtime_ns();
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
      if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPNS) {
        struct timespec ts;
        memcpy(&ts, CMSG_DATA(c), sizeof(ts));
        arrived = (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
      }
    }
    if (arrived >= ack.echo_send_time_ns) {
      g_rtt_us = (uint32_t) ((arrived - ack.echo_send_time_ns) / 1000u);
    }
    g_acks++;
  }
}

static void udp_send(uint32_t seq, uint16_t flags, XrTime sample_time, const float dir[3]) {
  gaze_sample_packet_t p;
  memset(&p, 0, sizeof(p));
  memcpy(p.magic, GAZE_SAMPLE_MAGIC, 4);
  p.version = GAZE_PACKET_VERSION;
  p.flags = flags;
  p.seq = seq;
  p.rtt_us = g_rtt_us;
  p.xr_sample_time = sample_time;
  p.dir[0] = dir[0];
  p.dir[1] = dir[1];
  p.dir[2] = dir[2];
  p.send_time_ns = realtime_ns();
  // Errors such as ECONNREFUSED (no listener yet) are expected and ignored.
  (void) sendto(g_sock, &p, sizeof(p), 0, (struct sockaddr *) &g_dest, g_dest_len);
  udp_drain_acks();
}

/* ---- EGL --------------------------------------------------------------------------------- */

static bool egl_init(int gl_major, int gl_minor) {
  egl.lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
  if (!egl.lib) {
    fprintf(stderr, "FAIL: dlopen libEGL.so.1: %s\n", dlerror());
    return false;
  }
#define LOAD(name) *(void **) &egl.name = dlsym(egl.lib, "egl" #name)
  LOAD(GetProcAddress);
  LOAD(GetDisplay);
  LOAD(Initialize);
  LOAD(BindAPI);
  LOAD(ChooseConfig);
  LOAD(CreateContext);
  LOAD(MakeCurrent);
  LOAD(GetError);
#undef LOAD
  if (!egl.GetProcAddress || !egl.GetDisplay || !egl.Initialize || !egl.BindAPI || !egl.ChooseConfig ||
      !egl.CreateContext || !egl.MakeCurrent || !egl.GetError) {
    fprintf(stderr, "FAIL: libEGL is missing core entry points\n");
    return false;
  }
  *(void **) &egl.GetPlatformDisplayEXT = (void *) egl.GetProcAddress("eglGetPlatformDisplayEXT");

  // Surfaceless first: the probe never draws, and it must not open a window.
  int major = 0, minor = 0;
  egl.dpy = NULL;
  if (egl.GetPlatformDisplayEXT) {
    egl.dpy = egl.GetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);
    if (egl.dpy && !egl.Initialize(egl.dpy, &major, &minor)) {
      egl.dpy = NULL;
    }
  }
  if (!egl.dpy) {
    egl.dpy = egl.GetDisplay(NULL);
    if (!egl.dpy || !egl.Initialize(egl.dpy, &major, &minor)) {
      fprintf(stderr, "FAIL: eglInitialize (0x%x)\n", egl.GetError());
      return false;
    }
  }
  printf("EGL %d.%d initialised\n", major, minor);

  if (!egl.BindAPI(EGL_OPENGL_API)) {
    fprintf(stderr, "FAIL: eglBindAPI(OpenGL) (0x%x)\n", egl.GetError());
    return false;
  }
  // EGL_SURFACE_TYPE defaults to EGL_WINDOW_BIT, which surfaceless displays never offer.
  const int cfg_attr[] = {EGL_SURFACE_TYPE, 0, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
  int count = 0;
  if (!egl.ChooseConfig(egl.dpy, cfg_attr, &egl.cfg, 1, &count) || count < 1) {
    fprintf(stderr, "FAIL: eglChooseConfig found no OpenGL config (0x%x)\n", egl.GetError());
    return false;
  }
  const int ctx_attr[] = {
    EGL_CONTEXT_MAJOR_VERSION,
    gl_major,
    EGL_CONTEXT_MINOR_VERSION,
    gl_minor,
    EGL_CONTEXT_OPENGL_PROFILE_MASK,
    EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
    EGL_NONE
  };
  egl.ctx = egl.CreateContext(egl.dpy, egl.cfg, NULL, ctx_attr);
  if (!egl.ctx) {
    egl.ctx = egl.CreateContext(egl.dpy, egl.cfg, NULL, NULL);
  }
  if (!egl.ctx || !egl.MakeCurrent(egl.dpy, NULL, NULL, egl.ctx)) {
    fprintf(stderr, "FAIL: EGL context create/make current (0x%x)\n", egl.GetError());
    return false;
  }
  return true;
}

/* ---- OpenXR ------------------------------------------------------------------------------ */

static struct {
  bool eye_gaze, headless, overlay, opengl, egl, timespec;
} has;

static struct {
  bool headless;
  bool overlay;
  XrSystemId system;
  XrSession session;
  XrSessionState state;
  bool running;
  XrViewConfigurationType view_config;
  XrEnvironmentBlendMode blend;
  XrActionSet action_set;
  XrAction gaze_action;
  XrSpace gaze_space;
  XrSpace base_space;
  PFN_xrConvertTimespecTimeToTimeKHR convert_time;
} xr;

static const char *state_name(XrSessionState s) {
  switch (s) {
    case XR_SESSION_STATE_IDLE:
      return "IDLE";
    case XR_SESSION_STATE_READY:
      return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED:
      return "SYNCHRONIZED";
    case XR_SESSION_STATE_VISIBLE:
      return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED:
      return "FOCUSED";
    case XR_SESSION_STATE_STOPPING:
      return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING:
      return "LOSS_PENDING";
    case XR_SESSION_STATE_EXITING:
      return "EXITING";
    default:
      return "UNKNOWN";
  }
}

static bool create_instance(void) {
  uint32_t n = 0;
  XR_CHECK(xrEnumerateInstanceExtensionProperties(NULL, 0, &n, NULL));
  XrExtensionProperties *props = calloc(n, sizeof(*props));
  for (uint32_t i = 0; i < n; i++) {
    props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
  }
  XR_CHECK(xrEnumerateInstanceExtensionProperties(NULL, n, &n, props));
  printf("Runtime instance extensions (%u):\n", n);
  for (uint32_t i = 0; i < n; i++) {
    const char *e = props[i].extensionName;
    printf("  %s v%u\n", e, props[i].extensionVersion);
    has.eye_gaze |= !strcmp(e, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
    has.headless |= !strcmp(e, XR_MND_HEADLESS_EXTENSION_NAME);
    has.overlay |= !strcmp(e, XR_EXTX_OVERLAY_EXTENSION_NAME);
    has.opengl |= !strcmp(e, PROBE_KHR_OPENGL_ENABLE);
    has.egl |= !strcmp(e, PROBE_MNDX_EGL_ENABLE);
    has.timespec |= !strcmp(e, XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME);
  }
  free(props);
  printf(
    "CHECK eye_gaze_interaction=%s headless=%s overlay=%s opengl=%s egl=%s convert_timespec=%s\n",
    has.eye_gaze ? "yes" : "NO",
    has.headless ? "yes" : "no",
    has.overlay ? "yes" : "no",
    has.opengl ? "yes" : "no",
    has.egl ? "yes" : "no",
    has.timespec ? "yes" : "no"
  );
  if (!has.eye_gaze) {
    fprintf(stderr, "FAIL: runtime does not offer XR_EXT_eye_gaze_interaction\n");
    return false;
  }

  bool can_gl = has.opengl && has.egl;
  switch (opt.mode) {
    case MODE_HEADLESS:
      if (!has.timespec) {
        fprintf(stderr, "FAIL: headless mode also needs XR_KHR_convert_timespec_time\n");
        return false;
      }
      xr.headless = true;
      break;
    case MODE_OVERLAY:
      xr.overlay = true;
      break;
    case MODE_EGL:
      break;
    case MODE_AUTO:
      xr.headless = has.headless && has.timespec;
      xr.overlay = !xr.headless && has.overlay && can_gl;
      if (!xr.headless && !can_gl) {
        fprintf(stderr, "FAIL: no usable session type (need XR_MND_headless, or XR_KHR_opengl_enable + XR_MNDX_egl_enable)\n");
        return false;
      }
      break;
  }

  const char *exts[8];
  uint32_t ne = 0;
  exts[ne++] = XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME;
  if (xr.headless) {
    exts[ne++] = XR_MND_HEADLESS_EXTENSION_NAME;
    exts[ne++] = XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME;
  } else {
    exts[ne++] = PROBE_KHR_OPENGL_ENABLE;
    exts[ne++] = PROBE_MNDX_EGL_ENABLE;
    if (xr.overlay) {
      exts[ne++] = XR_EXTX_OVERLAY_EXTENSION_NAME;
    }
    if (has.timespec) {
      exts[ne++] = XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME;
    }
  }
  printf("Session type: %s\n", xr.headless ? "headless (XR_MND_headless)" : xr.overlay ? "overlay (XR_EXTX_overlay + EGL)" : "EGL/OpenGL, no layers submitted");

  XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
  strncpy(ci.applicationInfo.applicationName, "vibeshine-gaze-probe", XR_MAX_APPLICATION_NAME_SIZE - 1);
  ci.applicationInfo.applicationVersion = 1;
  strncpy(ci.applicationInfo.engineName, "none", XR_MAX_ENGINE_NAME_SIZE - 1);
  ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
  ci.enabledExtensionCount = ne;
  ci.enabledExtensionNames = exts;
  XR_CHECK(xrCreateInstance(&ci, &g_instance));
  if (!load_instance_functions(g_instance)) {
    return false;
  }

  XrInstanceProperties ip = {XR_TYPE_INSTANCE_PROPERTIES};
  XR_CHECK(xrGetInstanceProperties(g_instance, &ip));
  printf(
    "Runtime: %s %u.%u.%u\n",
    ip.runtimeName,
    XR_VERSION_MAJOR(ip.runtimeVersion),
    XR_VERSION_MINOR(ip.runtimeVersion),
    XR_VERSION_PATCH(ip.runtimeVersion)
  );
  if (has.timespec) {
    xrGetInstanceProcAddr(g_instance, "xrConvertTimespecTimeToTimeKHR", (PFN_xrVoidFunction *) &xr.convert_time);
  }
  return true;
}

static bool create_session(void) {
  XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO};
  sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  XR_CHECK(xrGetSystem(g_instance, &sgi, &xr.system));

  XrSystemEyeGazeInteractionPropertiesEXT eg = {XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT};
  XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
  sp.next = &eg;
  XR_CHECK(xrGetSystemProperties(g_instance, xr.system, &sp));
  printf("System: %s (vendor 0x%x)\n", sp.systemName, sp.vendorId);
  printf("CHECK supportsEyeGazeInteraction=%s\n", eg.supportsEyeGazeInteraction ? "true" : "FALSE");
  if (!eg.supportsEyeGazeInteraction && !opt.force) {
    fprintf(stderr, "FAIL: system reports no eye gaze support (eye tracking off in settings, or not allowed for this app?)\n");
    return false;
  }

  // View configuration and blend mode, needed for xrBeginSession and xrEndFrame.
  xr.view_config = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  uint32_t n = 0;
  XrViewConfigurationType vct[8];
  if (XR_SUCCEEDED(xrEnumerateViewConfigurations(g_instance, xr.system, 8, &n, vct)) && n > 0) {
    xr.view_config = vct[0];
  }
  XrEnvironmentBlendMode bm[8];
  xr.blend = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  if (XR_SUCCEEDED(xrEnumerateEnvironmentBlendModes(g_instance, xr.system, xr.view_config, 8, &n, bm)) && n > 0) {
    xr.blend = bm[0];
  }

  XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
  sci.systemId = xr.system;
  probe_egl_binding_t binding = {XR_TYPE_GRAPHICS_BINDING_EGL_MNDX};
  XrSessionCreateInfoOverlayEXTX overlay = {XR_TYPE_SESSION_CREATE_INFO_OVERLAY_EXTX};
  if (!xr.headless) {
    probe_get_gl_requirements_t get_req = NULL;
    XR_CHECK(xrGetInstanceProcAddr(g_instance, "xrGetOpenGLGraphicsRequirementsKHR", (PFN_xrVoidFunction *) &get_req));
    probe_gl_requirements_t req = {XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
    XR_CHECK(get_req(g_instance, xr.system, &req));
    int gl_major = (int) XR_VERSION_MAJOR(req.minApiVersionSupported);
    int gl_minor = (int) XR_VERSION_MINOR(req.minApiVersionSupported);
    if (gl_major < 3) {
      gl_major = 3;
      gl_minor = 3;
    }
    if (!egl_init(gl_major, gl_minor)) {
      return false;
    }
    binding.getProcAddress = egl.GetProcAddress;
    binding.display = egl.dpy;
    binding.config = egl.cfg;
    binding.context = egl.ctx;
    sci.next = &binding;
    if (xr.overlay) {
      overlay.sessionLayersPlacement = 1;  // Above the main app.
      binding.next = &overlay;
    }
  }
  XR_CHECK(xrCreateSession(g_instance, &sci, &xr.session));
  printf("CHECK session_created=true\n");

  // Gaze action and binding.
  XrActionSetCreateInfo asci = {XR_TYPE_ACTION_SET_CREATE_INFO};
  strcpy(asci.actionSetName, "gaze_probe");
  strcpy(asci.localizedActionSetName, "Gaze probe");
  XR_CHECK(xrCreateActionSet(g_instance, &asci, &xr.action_set));

  XrActionCreateInfo aci = {XR_TYPE_ACTION_CREATE_INFO};
  aci.actionType = XR_ACTION_TYPE_POSE_INPUT;
  strcpy(aci.actionName, "gaze");
  strcpy(aci.localizedActionName, "Gaze");
  XR_CHECK(xrCreateAction(xr.action_set, &aci, &xr.gaze_action));

  XrPath profile, gaze_path;
  XR_CHECK(xrStringToPath(g_instance, "/interaction_profiles/ext/eye_gaze_interaction", &profile));
  XR_CHECK(xrStringToPath(g_instance, "/user/eyes_ext/input/gaze_ext/pose", &gaze_path));
  XrActionSuggestedBinding sb = {xr.gaze_action, gaze_path};
  XrInteractionProfileSuggestedBinding ipsb = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
  ipsb.interactionProfile = profile;
  ipsb.countSuggestedBindings = 1;
  ipsb.suggestedBindings = &sb;
  XR_CHECK(xrSuggestInteractionProfileBindings(g_instance, &ipsb));

  XrSessionActionSetsAttachInfo att = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
  att.countActionSets = 1;
  att.actionSets = &xr.action_set;
  XR_CHECK(xrAttachSessionActionSets(xr.session, &att));

  XrActionSpaceCreateInfo spci = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
  spci.action = xr.gaze_action;
  spci.poseInActionSpace.orientation.w = 1.0f;
  XR_CHECK(xrCreateActionSpace(xr.session, &spci, &xr.gaze_space));

  XrReferenceSpaceCreateInfo rsci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  rsci.referenceSpaceType = opt.view_space ? XR_REFERENCE_SPACE_TYPE_VIEW : XR_REFERENCE_SPACE_TYPE_LOCAL;
  rsci.poseInReferenceSpace.orientation.w = 1.0f;
  XR_CHECK(xrCreateReferenceSpace(xr.session, &rsci, &xr.base_space));
  return true;
}

/** Returns false when the session ended or failed. */
static bool poll_events(void) {
  XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
  while (xrPollEvent(g_instance, &ev) == XR_SUCCESS) {
    if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
      const XrEventDataSessionStateChanged *e = (const XrEventDataSessionStateChanged *) &ev;
      xr.state = e->state;
      printf("STATE %s\n", state_name(xr.state));
      fflush(stdout);
      if (xr.state == XR_SESSION_STATE_READY) {
        XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
        bi.primaryViewConfigurationType = xr.view_config;
        XR_CHECK(xrBeginSession(xr.session, &bi));
        xr.running = true;
      } else if (xr.state == XR_SESSION_STATE_STOPPING) {
        xrEndSession(xr.session);
        xr.running = false;
      } else if (xr.state == XR_SESSION_STATE_EXITING || xr.state == XR_SESSION_STATE_LOSS_PENDING) {
        return false;
      }
    } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
      printf("STATE instance loss pending\n");
      return false;
    } else if (ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
      XrInteractionProfileState ps = {XR_TYPE_INTERACTION_PROFILE_STATE};
      XrPath top;
      xrStringToPath(g_instance, "/user/eyes_ext", &top);
      if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(xr.session, top, &ps))) {
        char s[XR_MAX_PATH_LENGTH] = "(none)";
        uint32_t len = 0;
        if (ps.interactionProfile != XR_NULL_PATH) {
          xrPathToString(g_instance, ps.interactionProfile, sizeof(s), &len, s);
        }
        printf("PROFILE /user/eyes_ext -> %s\n", s);
      }
    }
    ev = (XrEventDataBuffer) {XR_TYPE_EVENT_DATA_BUFFER};
  }
  return true;
}

static XrTime xr_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  XrTime t = 0;
  if (xr.convert_time) {
    xr.convert_time(g_instance, &ts, &t);
  }
  return t;
}

static struct {
  uint32_t seq;
  uint32_t samples, valid, unfocused;
  double window_start;
} stats;

static void sample_gaze(XrTime time) {
  bool focused = xr.state == XR_SESSION_STATE_FOCUSED;
  uint16_t flags = focused ? GAZE_FLAG_FOCUSED : 0;
  float dir[3] = {0, 0, 0};
  XrTime sample_time = time;

  XrActiveActionSet aas = {xr.action_set, XR_NULL_PATH};
  XrActionsSyncInfo si = {XR_TYPE_ACTIONS_SYNC_INFO};
  si.countActiveActionSets = 1;
  si.activeActionSets = &aas;
  XrResult sr = xrSyncActions(xr.session, &si);

  XrActionStatePose ps = {XR_TYPE_ACTION_STATE_POSE};
  XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
  gi.action = xr.gaze_action;
  xrGetActionStatePose(xr.session, &gi, &ps);

  XrEyeGazeSampleTimeEXT st = {XR_TYPE_EYE_GAZE_SAMPLE_TIME_EXT};
  XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
  loc.next = &st;
  XrResult lr = xrLocateSpace(xr.gaze_space, xr.base_space, time, &loc);
  const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
  bool valid = XR_SUCCEEDED(lr) && ps.isActive && (loc.locationFlags & need) == need;
  if (valid) {
    // Gaze looks along -Z of the gaze pose. Rotate (0,0,-1) by q.
    const XrQuaternionf q = loc.pose.orientation;
    dir[0] = -2.0f * (q.x * q.z + q.w * q.y);
    dir[1] = -2.0f * (q.y * q.z - q.w * q.x);
    dir[2] = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    float len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (len > 0.0f) {
      dir[0] /= len;
      dir[1] /= len;
      dir[2] /= len;
    }
    flags |= GAZE_FLAG_VALID;
    if (st.time != 0) {
      sample_time = st.time;
      flags |= GAZE_FLAG_SAMPLE_TIME;
    }
  }

  udp_send(stats.seq, flags, sample_time, dir);

  stats.samples++;
  stats.valid += valid;
  stats.unfocused += !focused;
  if (!opt.quiet) {
    // Yaw/pitch in degrees: positive yaw = right, positive pitch = up.
    double yaw = atan2(dir[0], -dir[2]) * 57.29578;
    double pitch = asin(fmax(-1.0, fmin(1.0, dir[1]))) * 57.29578;
    printf(
      "GAZE seq=%u valid=%d state=%s dir=(%+.3f %+.3f %+.3f) yaw=%+6.1f pitch=%+6.1f pos=(%+.3f %+.3f %+.3f) active=%d flags=0x%llx sync=%s rtt_us=%u\n",
      stats.seq,
      valid,
      state_name(xr.state),
      dir[0],
      dir[1],
      dir[2],
      valid ? yaw : 0.0,
      valid ? pitch : 0.0,
      loc.pose.position.x,
      loc.pose.position.y,
      loc.pose.position.z,
      ps.isActive,
      (unsigned long long) loc.locationFlags,
      sr == XR_SUCCESS ? "ok" : xr_str(sr),
      g_rtt_us
    );
  }
  stats.seq++;

  double now = mono_s();
  if (now - stats.window_start >= 1.0) {
    double dt = now - stats.window_start;
    printf(
      "STATS rate=%.1fHz valid=%.0f%% unfocused=%.0f%% acks=%u rtt_us=%u state=%s\n",
      stats.samples / dt,
      stats.samples ? 100.0 * stats.valid / stats.samples : 0.0,
      stats.samples ? 100.0 * stats.unfocused / stats.samples : 0.0,
      g_acks,
      g_rtt_us,
      state_name(xr.state)
    );
    fflush(stdout);
    stats.samples = stats.valid = stats.unfocused = 0;
    g_acks = 0;
    stats.window_start = now;
  }
}

static bool run(void) {
  double start = mono_s();
  stats.window_start = start;
  const XrFrameEndInfo no_layers_template = {XR_TYPE_FRAME_END_INFO};
  struct timespec next_tick;
  clock_gettime(CLOCK_MONOTONIC, &next_tick);
  const long period_ns = (long) (1e9 / opt.rate_hz);
  while (!g_stop) {
    if (opt.duration_s > 0 && mono_s() - start >= opt.duration_s) {
      break;
    }
    if (!poll_events()) {
      break;
    }
    if (!xr.running) {
      struct timespec d = {0, 10 * 1000000};
      nanosleep(&d, NULL);
      clock_gettime(CLOCK_MONOTONIC, &next_tick);
      continue;
    }
    if (xr.headless) {
      sample_gaze(xr_now());
      // Absolute deadlines, so the rate does not drift below --rate.
      next_tick.tv_nsec += period_ns;
      while (next_tick.tv_nsec >= 1000000000L) {
        next_tick.tv_nsec -= 1000000000L;
        next_tick.tv_sec++;
      }
      clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_tick, NULL);
      continue;
    }
    // Frame loop. xrWaitFrame paces us at the display rate. We submit no layers.
    XrFrameState fs = {XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo wi = {XR_TYPE_FRAME_WAIT_INFO};
    XR_CHECK(xrWaitFrame(xr.session, &wi, &fs));
    XrFrameBeginInfo bi = {XR_TYPE_FRAME_BEGIN_INFO};
    XR_CHECK(xrBeginFrame(xr.session, &bi));
    sample_gaze(fs.predictedDisplayTime);
    XrFrameEndInfo ei = no_layers_template;
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = xr.blend;
    ei.layerCount = 0;
    XR_CHECK(xrEndFrame(xr.session, &ei));
  }
  return true;
}

/* ---- main -------------------------------------------------------------------------------- */

static void usage(const char *argv0) {
  fprintf(
    stderr,
    "Usage: %s HOST PORT [options]\n"
    "  HOST PORT          where gaze_listener.py runs (the streaming PC)\n"
    "  --mode M           auto|headless|overlay|egl (default auto: headless, then overlay, then egl)\n"
    "  --space S          view|local (default view: gaze relative to the head)\n"
    "  --rate HZ          sample rate in headless mode (default 90; other modes follow the display)\n"
    "  --duration SEC     stop after SEC seconds (default: run until Ctrl+C)\n"
    "  --quiet            print one STATS line per second instead of every sample\n"
    "  --force            continue even if the system reports no eye gaze support (testing only)\n",
    argv0
  );
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  int pos = 0;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    const char *v = i + 1 < argc ? argv[i + 1] : NULL;
    if (!strcmp(a, "--mode") && v) {
      opt.mode = !strcmp(v, "headless") ? MODE_HEADLESS : !strcmp(v, "overlay") ? MODE_OVERLAY :
                 !strcmp(v, "egl")      ? MODE_EGL :
                                          MODE_AUTO;
      i++;
    } else if (!strcmp(a, "--space") && v) {
      opt.view_space = strcmp(v, "local") != 0;
      i++;
    } else if (!strcmp(a, "--rate") && v) {
      opt.rate_hz = atof(v);
      i++;
    } else if (!strcmp(a, "--duration") && v) {
      opt.duration_s = atof(v);
      i++;
    } else if (!strcmp(a, "--quiet")) {
      opt.quiet = true;
    } else if (!strcmp(a, "--force")) {
      opt.force = true;
    } else if (a[0] == '-') {
      usage(argv[0]);
      return 2;
    } else if (pos == 0) {
      opt.host = a;
      pos++;
    } else if (pos == 1) {
      opt.port = a;
      pos++;
    }
  }
  if (!opt.host || !opt.port || opt.rate_hz <= 0) {
    usage(argv[0]);
    return 2;
  }
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  if (!udp_open()) {
    return 1;
  }
  printf("Sending gaze samples to %s:%s\n", opt.host, opt.port);

  bool ok = load_runtime() && create_instance() && create_session() && run();

  if (xr.session != XR_NULL_HANDLE && p_xrDestroySession) {
    if (xr.running) {
      xrRequestExitSession(xr.session);
      xrEndSession(xr.session);
    }
    xrDestroySession(xr.session);
  }
  if (g_instance != XR_NULL_HANDLE && p_xrDestroyInstance) {
    xrDestroyInstance(g_instance);
  }
  close(g_sock);
  printf(ok ? "DONE\n" : "FAILED (see FAIL lines above)\n");
  return ok ? 0 : 1;
}
