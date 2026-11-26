/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "UdmabufSurface.h"

#include "DMABufDevice.h"
#include "DMABufFormats.h"

#ifdef MOZ_WAYLAND
#  include "nsWaylandDisplay.h"
#endif

#include <dlfcn.h>
#include <fcntl.h>
#include <gbm.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef HAVE_EVENTFD
#  include <sys/eventfd.h>
#endif
#include <poll.h>
#ifdef HAVE_SYSIOCCOM_H
#  include <sys/ioccom.h>
#endif
#include <sys/ioctl.h>
#ifdef MOZ_LOGGING
#  include "gfxUtils.h"
#endif

// DMABufDevice defines its own version of this which collides with the
// official version in drm_fourcc.h
#ifdef DRM_FORMAT_MOD_INVALID
#  undef DRM_FORMAT_MOD_INVALID
#endif
#include <libdrm/drm_fourcc.h>
#ifndef DRM_FORMAT_MOD_INVALID
#  define DRM_FORMAT_MOD_INVALID ((1ULL << 56) - 1)
#endif

#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
/*#ifdef MOZ_UDMABUF
#include <linux/udmabuf.h>
#endif*/
#include <unistd.h>
#include <sys/ioctl.h>

#include "base/linux_memfd_defs.h"
#include "GLContextEGL.h"
#include "GLContextProvider.h"
#include "GLContextTypes.h"  // for GLContext, etc
#include "ImageContainer.h"
#include "ScopedGLHelpers.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/gfx/2D.h"
#include "mozilla/gfx/FileHandleWrapper.h"
#include "mozilla/gfx/gfxVars.h"
#include "mozilla/layers/ImageDataSerializer.h"
#include "mozilla/layers/LayersSurfaces.h"
#include "nsGtkUtils.h"

/* C++ / C typecast macros for special EGL handle values */
#if defined(__cplusplus)
#  define EGL_CAST(type, value) (static_cast<type>(value))
#else
#  define EGL_CAST(type, value) ((type)(value))
#endif


#if !defined(HAVE_MEMFD_CREATE) && defined(XP_LINUX) && \
    defined(SYS_memfd_create)

// Older libc versions (e.g., glibc before 2.27) don't have the
// wrapper, but we can supply our own; see `linux_memfd_defs.h`.

static int memfd_create(const char* aName, unsigned int aFlags) {
  return syscall(SYS_memfd_create, aName, aFlags);
}

#  define HAVE_MEMFD_CREATE 1
#endif

#define UDMABUF_FLAGS_CLOEXEC	0x01

struct udmabuf_create {
	__u32 memfd;
	__u32 flags;
	__u64 offset;
	__u64 size;
};

struct udmabuf_create_item {
	__u32 memfd;
	__u32 __pad;
	__u64 offset;
	__u64 size;
};

struct udmabuf_create_list {
	__u32 flags;
	__u32 count;
	struct udmabuf_create_item list[];
};

#define UDMABUF_CREATE       _IOW('u', 0x42, struct udmabuf_create)
#define UDMABUF_CREATE_LIST  _IOW('u', 0x43, struct udmabuf_create_list)

using namespace mozilla;
using namespace mozilla::widget;
using namespace mozilla::gl;
using namespace mozilla::layers;
using namespace mozilla::gfx;

#undef LOGDMABUF
#undef LOGDMABUFREF
#ifdef MOZ_LOGGING
#  include "Units.h"
#  include "mozilla/Logging.h"
#  include "nsTArray.h"

extern mozilla::LazyLogModule gDmabufLog;
#  define LOGDMABUF(str, ...)                     \
    MOZ_LOG(gDmabufLog, mozilla::LogLevel::Debug, \
            ("%s: " str, GetDebugTag().get(), ##__VA_ARGS__))
#  define LOGDMAVERBOSES(str, ...) \
    MOZ_LOG(gDmabufLog, mozilla::LogLevel::Verbose, (str, ##__VA_ARGS__))
#  define LOGDMABUFS(str, ...) \
    MOZ_LOG(gDmabufLog, mozilla::LogLevel::Debug, (str, ##__VA_ARGS__))
static LazyLogModule gDmabufRefLog("DmabufRef");
#  define LOGDMABUFREF(str, ...)                     \
    MOZ_LOG(gDmabufRefLog, mozilla::LogLevel::Debug, \
            ("%s: " str, GetDebugTag().get(), ##__VA_ARGS__))
#else
#  define LOGDMABUF(str, ...)
#  define LOGDMAVERBOSES(str, ...)
#  define LOGDMABUFS(str, ...)
#  define LOGDMABUFREF(str, ...)
#endif /* MOZ_LOGGING */

static const std::string FormatEGLError(EGLint err) {
  switch (err) {
    case LOCAL_EGL_NOT_INITIALIZED:
      return "EGL_NOT_INITIALIZED";
    case LOCAL_EGL_BAD_ACCESS:
      return "EGL_BAD_ACCESS";
    case LOCAL_EGL_BAD_ALLOC:
      return "EGL_BAD_ALLOC";
    case LOCAL_EGL_BAD_ATTRIBUTE:
      return "EGL_BAD_ATTRIBUTE";
    case LOCAL_EGL_BAD_CONTEXT:
      return "EGL_BAD_CONTEXT";
    case LOCAL_EGL_BAD_CONFIG:
      return "EGL_BAD_CONFIG";
    case LOCAL_EGL_BAD_CURRENT_SURFACE:
      return "EGL_BAD_CURRENT_SURFACE";
    case LOCAL_EGL_BAD_DISPLAY:
      return "EGL_BAD_DISPLAY";
    case LOCAL_EGL_BAD_SURFACE:
      return "EGL_BAD_SURFACE";
    case LOCAL_EGL_BAD_MATCH:
      return "EGL_BAD_MATCH";
    case LOCAL_EGL_BAD_PARAMETER:
      return "EGL_BAD_PARAMETER";
    case LOCAL_EGL_BAD_NATIVE_PIXMAP:
      return "EGL_BAD_NATIVE_PIXMAP";
    case LOCAL_EGL_BAD_NATIVE_WINDOW:
      return "EGL_BAD_NATIVE_WINDOW";
    case LOCAL_EGL_CONTEXT_LOST:
      return "EGL_CONTEXT_LOST";
    default:
      return "EGL error code: " + std::to_string(err);
  }
}


bool UdmabufSurface::IsGlobalRefSet() {
  MutexAutoLock lock(mSurfaceLock);
  if (!mGlobalRefCountFd) {
    return false;
  }
  struct pollfd pfd;
  pfd.fd = mGlobalRefCountFd;
  pfd.events = POLLIN;
  return poll(&pfd, 1, 0) == 1;
}

int UdmabufSurface::GetGlobalRefCountFd() {
  MutexAutoLock lock(mSurfaceLock);
  return mGlobalRefCountFd;
}

/* static */
void UdmabufSurface::GetGlobalRefsSet(Span<const int> aRefCountFds,
                                     nsTArray<bool>& aRefSet) {
  // poll() ignores negative fds and reports no events for them, so surfaces
  // without a global refcount still get a slot and the two arrays map 1:1.
  AutoTArray<struct pollfd, 32> pfds;
  pfds.SetCapacity(aRefCountFds.Length());
  for (int fd : aRefCountFds) {
    pfds.AppendElement(pollfd{fd ? fd : -1, POLLIN, 0});
  }
  aRefSet.SetCapacity(aRefCountFds.Length());
  bool ret = poll(pfds.Elements(), pfds.Length(), 0) > 0;
  for (const auto& pfd : pfds) {
    aRefSet.AppendElement(ret && (pfd.revents & POLLIN));
  }
}

void UdmabufSurface::GlobalRefRelease() {
#ifdef HAVE_EVENTFD
  MutexAutoLock lock(mSurfaceLock);
  if (!mGlobalRefCountFd) {
    return;
  }
  LOGDMABUFREF("UdmabufSurface::GlobalRefRelease UID %d", mUID);
  uint64_t counter;
  if (read(mGlobalRefCountFd, &counter, sizeof(counter)) != sizeof(counter)) {
    if (errno == EAGAIN) {
      LOGDMABUFREF("  GlobalRefRelease failed: already zero reference! UID %d",
                   mUID);
    }
    // EAGAIN means the refcount is already zero. It happens when we release
    // last reference to the surface.
    if (errno != EAGAIN) {
      NS_WARNING(nsPrintfCString("Failed to unref dmabuf global ref count: %s",
                                 strerror(errno))
                     .get());
    }
  }
#endif
}

void UdmabufSurface::GlobalRefAddLocked(const MutexAutoLock& aProofOfLock) {
#ifdef HAVE_EVENTFD
  LOGDMABUFREF("UdmabufSurface::GlobalRefAddLocked UID %d", mUID);
  MOZ_DIAGNOSTIC_ASSERT(mGlobalRefCountFd);
  uint64_t counter = 1;
  if (write(mGlobalRefCountFd, &counter, sizeof(counter)) != sizeof(counter)) {
    NS_WARNING(nsPrintfCString("Failed to ref dmabuf global ref count: %s",
                               strerror(errno))
                   .get());
  }
#endif
}

void UdmabufSurface::GlobalRefAdd() {
  LOGDMABUFREF("UdmabufSurface::GlobalRefAdd UID %d", mUID);
  MutexAutoLock lock(mSurfaceLock);
  GlobalRefAddLocked(lock);
}

void UdmabufSurface::GlobalRefCountCreate() {
#ifdef HAVE_EVENTFD
  LOGDMABUFREF("UdmabufSurface::GlobalRefCountCreate UID %d", mUID);
  MutexAutoLock lock(mSurfaceLock);
  MOZ_DIAGNOSTIC_ASSERT(!mGlobalRefCountFd);
  // Create global ref count initialized to 0,
  // i.e. is not referenced after create.
  mGlobalRefCountFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE);
  if (mGlobalRefCountFd < 0) {
    NS_WARNING(nsPrintfCString("Failed to create dmabuf global ref count: %s",
                               strerror(errno))
                   .get());
    mGlobalRefCountFd = 0;
    return;
  }
#endif
}

void UdmabufSurface::GlobalRefCountImport(int aFd) {
#ifdef HAVE_EVENTFD
  MutexAutoLock lock(mSurfaceLock);
  mGlobalRefCountFd = aFd;
  if (mGlobalRefCountFd) {
    LOGDMABUFREF("UdmabufSurface::GlobalRefCountImport UID %d", mUID);
    GlobalRefAddLocked(lock);
  }
#endif
}

int UdmabufSurface::GlobalRefCountExport() {
  MutexAutoLock lock(mSurfaceLock);
#ifdef MOZ_LOGGING
  if (mGlobalRefCountFd) {
    LOGDMABUFREF("UdmabufSurface::GlobalRefCountExport UID %d", mUID);
  }
#endif
  return mGlobalRefCountFd;
}

void UdmabufSurface::GlobalRefCountDelete() {
  MutexAutoLock lock(mSurfaceLock);
  if (mGlobalRefCountFd) {
    LOGDMABUFREF("UdmabufSurface::GlobalRefCountDelete UID %d", mUID);
    close(mGlobalRefCountFd);
    mGlobalRefCountFd = 0;
  }
}

bool UdmabufSurface::ReleaseDMABuf() {
  LOGDMABUF("UdmabufSurface::ReleaseDMABuf() UID %d", mUID);

  Unmap();

  bool released = CloseFileDescriptors();

  mBufferPlaneCount = 0;

  return released;
}

MOZ_RUNINIT static Atomic<int> gNewSurfaceUID(getpid());

UdmabufSurface::UdmabufSurface(SurfaceType aSurfaceType)
    : mSurfaceLock("UdmabufSurface") {
  mUID = gNewSurfaceUID++;
  MOZ_COUNT_CTOR(UdmabufSurface);
}

UdmabufSurface::~UdmabufSurface() {
  MOZ_COUNT_DTOR(DMABufSurface);
  GlobalRefRelease();
  GlobalRefCountDelete();
}

already_AddRefed<UdmabufSurface> UdmabufSurface::CreateUdmabufSurface(
    const mozilla::layers::SurfaceDescriptor& aDesc) {
  const SurfaceDescriptorDMABuf& desc = aDesc.get_SurfaceDescriptorDMABuf();
  RefPtr<UdmabufSurface> surf;

  surf = new UdmabufSurfaceRGBA();

  if (!surf->Create(desc)) {
    return nullptr;
  }
  return surf.forget();
}

void UdmabufSurface::FenceSet() {
  MutexAutoLock lock(mSurfaceLock);

  if (!mGL) {
    gfxCriticalNoteOnce
        << "UdmabufSurface::FenceSet() failed: missing GL context";
    return;
  }

  mGL->MakeCurrent();

  const auto& gle = gl::GLContextEGL::Cast(mGL);
  const auto& egl = gle->mEgl;

  LOGDMABUF("UdmabufSurface::FenceSet() UID %d", mUID);

  if (egl->IsExtensionSupported(EGLExtension::KHR_fence_sync) &&
      egl->IsExtensionSupported(EGLExtension::ANDROID_native_fence_sync)) {
    if (EGLSyncKHR sync =
            egl->fCreateSyncKHR(LOCAL_EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr)) {
      auto rawFd = egl->fDupNativeFenceFDANDROID(sync);
      mSyncFd = new gfx::FileHandleWrapper(UniqueFileHandle(rawFd));
      egl->fDestroySync(sync);
      mGL->fFlush();
      return;
    }
  }

  mGL->fFinish();
}

// Waits for the sync_file fd to become readable, which happens when the
// fence signals.
static void PollSyncFd(int aRawFd) {
  if (aRawFd < 0) {
    return;
  }
  constexpr int kSyncPollTimeoutMs = 100;
  struct pollfd pfd = {aRawFd, POLLIN, 0};
  int ret = poll(&pfd, 1, kSyncPollTimeoutMs);
  if (!(ret == 1 && (pfd.revents & POLLIN))) {
    gfxCriticalNoteOnce << "poll failed: " << ret;
  }
}

/* static */
void UdmabufSurface::FenceWaitFd(RefPtr<gl::GLContext> aGL,
                                RefPtr<gfx::FileHandleWrapper> aSyncFd) {
  const auto& gle = gl::GLContextEGL::Cast(aGL);
  const auto& egl = gle->mEgl;
  auto syncFd = aSyncFd->ClonePlatformHandle();

  const EGLint attribs[] = {LOCAL_EGL_SYNC_NATIVE_FENCE_FD_ANDROID,
                            syncFd.get(), LOCAL_EGL_NONE};
  EGLSync sync =
      egl->fCreateSyncKHR(LOCAL_EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
  if (!sync) {
    gfxCriticalNoteOnce << "CreateSync failed";
    // CreateSync failed, so we cannot use eglClientWaitSync.
    PollSyncFd(syncFd.get());
    return;
  }

  // syncFd is owned by GLFence so clear local reference to avoid double.
  (void)syncFd.release();

  egl->fClientWaitSync(sync, 0, LOCAL_EGL_FOREVER);
  egl->fDestroySync(sync);
}

void UdmabufSurface::FenceWait(mozilla::gl::GLContext* aGLContext) {
  RefPtr<gl::GLContext> gl;
  RefPtr<gfx::FileHandleWrapper> syncFd;
  {
    MutexAutoLock lock(mSurfaceLock);
    if (!mSyncFd) {
      return;
    }
    gl = mGL;
    // eglCreateSyncKHR() can use any GL context to sync and wait to finish.
    if (!gl) {
      gl = aGLContext;
    }
    syncFd = mSyncFd.forget();
  }

  LOGDMABUF("UdmabufSurface::FenceWait() UID %d", mUID);
  if (!gl) {
    // Without a GL context, e.g. in the parent process when rendering happens
    // in the GPU process, wait for the fence on the CPU.
    PollSyncFd(syncFd->GetHandle());
    return;
  }
  FenceWaitFd(std::move(gl), std::move(syncFd));
}

void UdmabufSurface::SetSemaphoreFd(int aDuppedRawFd, bool aIsSyncFd) {
  MutexAutoLock lock(mSurfaceLock);
  mSemaphoreFd = new gfx::FileHandleWrapper(UniqueFileHandle(aDuppedRawFd));
  mSemaphoreFdIsSyncFd = aIsSyncFd;
}

void UdmabufSurface::MaybeSemaphoreWait(GLuint aGlTexture) {
  MOZ_ASSERT(aGlTexture);

  RefPtr<gl::GLContext> gl;
  RefPtr<gfx::FileHandleWrapper> semFdWrapper;
  bool isSyncFd;
  {
    MutexAutoLock lock(mSurfaceLock);
    if (!mSemaphoreFd) {
      return;
    }
    if (!mGL) {
      MOZ_DIAGNOSTIC_ASSERT(
          mGL, "UdmabufSurface::SemaphoreWait() missing GL context!");
      return;
    }
    gl = mGL;
    semFdWrapper = mSemaphoreFd.forget();
    isSyncFd = mSemaphoreFdIsSyncFd;
  }

  auto fd = semFdWrapper->ClonePlatformHandle();

  if (isSyncFd) {
    const auto& gle = gl::GLContextEGL::Cast(gl);
    const auto& egl = gle->mEgl;
    if (egl->IsExtensionSupported(EGLExtension::ANDROID_native_fence_sync)) {
      FenceWaitFd(gl, new gfx::FileHandleWrapper(std::move(fd)));
    }
    return;
  }

  if (!gl->IsExtensionSupported(gl::GLContext::EXT_semaphore) ||
      !gl->IsExtensionSupported(gl::GLContext::EXT_semaphore_fd)) {
    MOZ_ASSERT_UNREACHABLE("unexpected to be called");
    gfxCriticalNoteOnce << "EXT_semaphore_fd is not supported";
    return;
  }

  GLuint semaphoreHandle = 0;
  gl->fGenSemaphoresEXT(1, &semaphoreHandle);
  gl->fImportSemaphoreFdEXT(semaphoreHandle, LOCAL_GL_HANDLE_TYPE_OPAQUE_FD_EXT,
                            fd.release());
  auto error = gl->fGetError();
  if (error != LOCAL_GL_NO_ERROR) {
    gfxCriticalNoteOnce << "glImportSemaphoreFdEXT failed: " << error;
    return;
  }

  GLenum srcLayout = LOCAL_GL_LAYOUT_COLOR_ATTACHMENT_EXT;
  gl->fWaitSemaphoreEXT(semaphoreHandle, 0, nullptr, 1, &aGlTexture,
                        &srcLayout);
  error = gl->fGetError();
  if (error != LOCAL_GL_NO_ERROR) {
    gfxCriticalNoteOnce << "glWaitSemaphoreEXT failed: " << error;
    gl->fDeleteSemaphoresEXT(1, &semaphoreHandle);
    return;
  }
  gl->fDeleteSemaphoresEXT(1, &semaphoreHandle);
  LOGDMABUF("MaybeSemaphoreWait: done (GL semaphore wait)");
}

bool UdmabufSurface::CloseFileDescriptors() {
  bool released = false;

  if (mDmabufFd) {
    mDmabufFd = nullptr;
    released = true;
  }
  return released;
}

already_AddRefed<gfx::DataSourceSurface> UdmabufSurface::GetAsSourceSurface() {
  LOGDMABUF("UdmabufSurface::GetAsSourceSurface UID %d", mUID);
  return nullptr;
}

UdmabufSurfaceRGBA::UdmabufSurfaceRGBA()
    : UdmabufSurface(SURFACE_RGBA) {}

UdmabufSurfaceRGBA::~UdmabufSurfaceRGBA() {
  LOGDMABUF("UdmabufSurfaceRGBA::~UdmabufSurfaceRGBA() UID %d", mUID);
  ReleaseSurface();
}

#define UDMABUF_ALIGNMENT_MASK (256 - 1)

bool UdmabufSurfaceRGBA::Create(int aWidth, int aHeight,
                                RefPtr<DRMFormat> aFormat) {
  int udmabufDevFd = open("/dev/udmabuf", O_RDONLY | O_CLOEXEC, 0);
  if (udmabufDevFd == -1) {
    fprintf(stderr, "%s no udmabuf\n", __func__);
    return false;
  }

  if (!aFormat) {
    fprintf(stderr, "%s no format!\n", __func__);
    mFOURCCFormat = GBM_FORMAT_ARGB8888;
    aFormat = GetGlobalDMABufFormats()->GetDRMFormat(mFOURCCFormat);
    if (!aFormat) {
      LOGDMABUF("UdmabufSurfaceRGBA::Create(): Missing drm format 0x%x!",
                mFOURCCFormat);
      return false;
    }
  } else {
    mFOURCCFormat = aFormat->GetFormat();
  }

  mStrides[0] = (aWidth * GetFormatBPP() + UDMABUF_ALIGNMENT_MASK) & ~UDMABUF_ALIGNMENT_MASK;
  size_t size = mStrides[0] * aHeight;
  size_t pageSizeMask = getpagesize();
  mSize = (size + getpagesize()) & ~pageSizeMask;
  mWidth[0] = aWidth;
  mHeight[0] = aHeight;
  mBufferPlaneCount = 1;
  mOffsets[0] = 0;

  fprintf(stderr, "%s %dx%d 0x%u size %lu\n", __func__, aWidth, aHeight, mFOURCCFormat, mSize);

//#ifdef HAVE_MEMFD_CREATE
  int fd = memfd_create ("udmabuf-surface", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (fd < 0) {
    fprintf(stderr, "%s memfd_create() failed: %s\n", __func__, strerror (errno));
    return false;
  }
//#endif

  if (ftruncate (fd, mSize) < 0) {
    fprintf(stderr, "%s ftruncate failed: %s\n", __func__, strerror (errno));
    close (fd);
    return false;
  }

  if (fcntl (fd, F_ADD_SEALS, F_SEAL_SHRINK) < 0) {
    fprintf(stderr, "%s adding seals failed: %s\n", __func__, strerror (errno));
    close (fd);
    return false;
  }

  struct udmabuf_create create;
  create.memfd = fd;
  create.flags = UDMABUF_FLAGS_CLOEXEC;
  create.offset = 0;
  create.size = mSize;

  int ufd = ioctl(udmabufDevFd, UDMABUF_CREATE, &create);
  if (ufd < 0) {
    fprintf(stderr, "%s creating udmabuf failed: %s\n", __func__, strerror (errno));
    close (fd);
    return false;
  }
  mDmabufFd = new gfx::FileHandleWrapper(UniqueFileHandle(ufd));
  close (fd);

  close(udmabufDevFd);
  return true;
}

bool UdmabufSurfaceRGBA::ImportSurfaceDescriptor(
    const SurfaceDescriptor& aDesc) {
  const SurfaceDescriptorDMABuf& desc = aDesc.get_SurfaceDescriptorDMABuf();

  MOZ_RELEASE_ASSERT(desc.fds().Length() == 1);
  if (desc.width().Length() != 1 || desc.height().Length() != 1 ||
      desc.modifier().Length() != 1 ||
      desc.strides().Length() != 1 ||
      desc.offsets().Length() != 1) {
    return false;
  }

  mFOURCCFormat = desc.fourccFormat();
  mWidth[0] = desc.width()[0];
  mHeight[0] = desc.height()[0];
  mUID = desc.uid();
  mPID = desc.pid();
  mHDRMetadata = mozilla::gfx::HDRMetadata();
  mColorRange = desc.colorRange();
  mColorPrimaries = desc.colorPrimaries();
  mTransferFunction = desc.transferFunction();

  LOGDMABUF(
      "DMABufSurfaceRGBA::ImportSurfaceDescriptor() UID %d size %d x %d\n",
      mUID, mWidth[0], mHeight[0]);

  mDmabufFd = desc.fds()[0];
  mStrides[0] = desc.strides()[0];
  mOffsets[0] = desc.offsets()[0];

  if (desc.fence().Length() > 0) {
    mSyncFd = desc.fence()[0];
  }

  if (desc.semaphoreFd()) {
    mSemaphoreFd = desc.semaphoreFd();
    mSemaphoreFdIsSyncFd = desc.semaphoreFdIsSyncFd();
  }

  if (desc.refCount().Length() > 0) {
    GlobalRefCountImport(desc.refCount()[0].ClonePlatformHandle().release());
  }

  LOGDMABUF("  imported size %d x %d format %x planes %d", mWidth[0],
            mHeight[0], mFOURCCFormat, mBufferPlaneCount);

  return true;
}

bool UdmabufSurfaceRGBA::Create(const SurfaceDescriptor& aDesc) {
  return ImportSurfaceDescriptor(aDesc);
}

bool UdmabufSurfaceRGBA::Serialize(
    mozilla::layers::SurfaceDescriptor& aOutDescriptor) {
  AutoTArray<uint32_t, BUFFER_SURFACE_PLANES> width;
  AutoTArray<uint32_t, BUFFER_SURFACE_PLANES> height;
  AutoTArray<NotNull<RefPtr<gfx::FileHandleWrapper>>, BUFFER_SURFACE_PLANES>
      fds;
  AutoTArray<uint32_t, BUFFER_SURFACE_PLANES> strides;
  AutoTArray<uint32_t, BUFFER_SURFACE_PLANES> offsets;
  AutoTArray<uintptr_t, BUFFER_SURFACE_PLANES> images;
  AutoTArray<uint64_t, BUFFER_SURFACE_PLANES> modifiers;
  AutoTArray<NotNull<RefPtr<gfx::FileHandleWrapper>>, 1> fenceFDs;
  AutoTArray<ipc::FileDescriptor, 1> refCountFDs;

  LOGDMABUF("UdmabufSurfaceRGBA::Serialize() UID %d\n", mUID);

  width.AppendElement(mWidth[0]);
  height.AppendElement(mHeight[0]);
  modifiers.AppendElement(0);
  fds.AppendElement(WrapNotNull(mDmabufFd));
  strides.AppendElement(mStrides[0]);
  offsets.AppendElement(mOffsets[0]);

  if (mSyncFd) {
    fenceFDs.AppendElement(WrapNotNull(mSyncFd));
  }

  if (mGlobalRefCountFd) {
    refCountFDs.AppendElement(ipc::FileDescriptor(GlobalRefCountExport()));
  }

  // GCC needs it (Bug 1959653).
  AutoTArray<uint32_t, 1> tmp;
  aOutDescriptor = SurfaceDescriptorDMABuf(
      GetSurfaceType(), mFOURCCFormat, modifiers, 0, fds, width,
      height, width, height, tmp, strides, offsets, GetYUVColorSpace(),
      mColorRange, mColorPrimaries, mTransferFunction, 0, fenceFDs, mUID,
      mCanRecycle ? getpid() : 0, refCountFDs,
      /* semaphoreFd */ nullptr, /* semaphoreFdIsSyncFd */ false, mHDRMetadata);
  return true;
}

bool UdmabufSurfaceRGBA::CreateTexture(GLContext* aGLContext, int aPlane) {
  fprintf(stderr, "%s\n", __func__);

  if (mTexture[0]) {
    MOZ_DIAGNOSTIC_ASSERT(mGL == aGLContext);
    return true;
  }

  LOGDMABUF("UdmabufSurfaceRGBA::CreateTexture() UID %d plane %d\n", mUID,
            aPlane);

  MOZ_DIAGNOSTIC_ASSERT(aGLContext);
  mGL = aGLContext;
  auto releaseTextures = MakeScopeExit([&] { ReleaseTextures(); });

  nsTArray<EGLint> attribs;
  attribs.AppendElement(LOCAL_EGL_WIDTH);
  attribs.AppendElement(mWidth[0]);
  attribs.AppendElement(LOCAL_EGL_HEIGHT);
  attribs.AppendElement(mHeight[0]);
  attribs.AppendElement(LOCAL_EGL_LINUX_DRM_FOURCC_EXT);
  attribs.AppendElement(mFOURCCFormat);
#define ADD_PLANE_ATTRIBS(plane_idx)                                        \
  {                                                                         \
    attribs.AppendElement(LOCAL_EGL_DMA_BUF_PLANE##plane_idx##_FD_EXT);     \
    attribs.AppendElement(mDmabufFd->GetHandle());              \
    attribs.AppendElement(LOCAL_EGL_DMA_BUF_PLANE##plane_idx##_OFFSET_EXT); \
    attribs.AppendElement((int)mOffsets[plane_idx]);                        \
    attribs.AppendElement(LOCAL_EGL_DMA_BUF_PLANE##plane_idx##_PITCH_EXT);  \
    attribs.AppendElement((int)mStrides[plane_idx]);                        \
    attribs.AppendElement(                                                  \
        LOCAL_EGL_DMA_BUF_PLANE##plane_idx##_MODIFIER_LO_EXT);              \
    attribs.AppendElement(0);                                  \
    attribs.AppendElement(                                                  \
        LOCAL_EGL_DMA_BUF_PLANE##plane_idx##_MODIFIER_HI_EXT);              \
    attribs.AppendElement(0);                                         \
  }

  ADD_PLANE_ATTRIBS(0);
  if (mBufferPlaneCount > 1) ADD_PLANE_ATTRIBS(1);
  if (mBufferPlaneCount > 2) ADD_PLANE_ATTRIBS(2);
  if (mBufferPlaneCount > 3) ADD_PLANE_ATTRIBS(3);
#undef ADD_PLANE_ATTRIBS
  attribs.AppendElement(LOCAL_EGL_NONE);

  if (!aGLContext->MakeCurrent()) {
    LOGDMABUF(
        "UdmabufSurfaceRGBA::CreateTexture(): failed to make GL context "
        "current");
    return false;
  }

  const auto& gle = gl::GLContextEGL::Cast(aGLContext);
  const auto& egl = gle->mEgl;

  MOZ_ASSERT(mEGLImage[0] == LOCAL_EGL_NO_IMAGE);
  mEGLImage[0] =
      egl->fCreateImage(LOCAL_EGL_NO_CONTEXT, LOCAL_EGL_LINUX_DMA_BUF_EXT,
                        nullptr, attribs.Elements());

  if (mEGLImage[0] == LOCAL_EGL_NO_IMAGE) {
    LOGDMABUF("  EGLImageKHR creation failed, EGL error %s",
              FormatEGLError(egl->mLib->fGetError()).c_str());
    return false;
  }

  aGLContext->fGenTextures(1, &mTexture[0]);
  SetTextureFilters(aGLContext, mTexture[0]);
  const ScopedBindTexture savedTex(aGLContext, mTexture[0]);
  aGLContext->fEGLImageTargetTexture2D(LOCAL_GL_TEXTURE_2D, mEGLImage[0]);

  releaseTextures.release();
  return true;
}

void UdmabufSurfaceRGBA::ReleaseSurface() {
  LOGDMABUF("UdmabufSurfaceRGBA::ReleaseSurface() UID %d", mUID);
  //MOZ_ASSERT(!IsMapped(), "We can't release mapped buffer!");
  ReleaseTextures();
  if (ReleaseDMABuf()) {
    //LogMemorySubRGBA(GetUID(), GetUsedMemory(mWidth[0], mHeight[0]));
  }
}

// We should synchronize DMA Buffer object access from CPU to avoid potential
// cache incoherency and data loss.
// See
// https://docs.kernel.org/driver-api/dma-buf.html#cpu-access-to-dma-buffer-objects
struct dma_buf_sync {
  uint64_t flags;
};
#define DMA_BUF_SYNC_READ (1 << 0)
#define DMA_BUF_SYNC_WRITE (2 << 0)
#define DMA_BUF_SYNC_START (0 << 2)
#define DMA_BUF_SYNC_END (1 << 2)
#define DMA_BUF_BASE 'b'
#define DMA_BUF_IOCTL_SYNC _IOW(DMA_BUF_BASE, 0, struct dma_buf_sync)

static void SyncDmaBuf(int aFd, uint64_t aFlags) {
  struct dma_buf_sync sync = {0};

  sync.flags = aFlags | DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE;
  while (true) {
    int ret;
    ret = ioctl(aFd, DMA_BUF_IOCTL_SYNC, &sync);
    if (ret == -1 && errno == EINTR) {
      continue;
    } else if (ret == -1) {
      LOGDMABUFS("Failed to synchronize DMA buffer: %s FD %d", strerror(errno),
                 aFd);
      break;
    } else {
      break;
    }
  }
}

void* UdmabufSurface::MapInternal(uint32_t aX, uint32_t aY, uint32_t aWidth,
                                 uint32_t aHeight, uint32_t* aStride) {
  mMappedRegionStride = 0;
  mMappedRegionData = nullptr;
  mMappedRegion = mmap(0, mSize, PROT_READ | PROT_WRITE, MAP_SHARED, mDmabufFd->GetHandle(), 0);
  if (!mMappedRegion) {
    fprintf(stderr, "Surface mapping failed: %s\n", strerror(errno));
    return nullptr;
  }
  if (aStride) {
    *aStride = mStrides[0];
  }

  SyncDmaBuf(mDmabufFd->GetHandle(), DMA_BUF_SYNC_START);
  return mMappedRegion;
}

void* UdmabufSurfaceRGBA::Map(uint32_t aX, uint32_t aY, uint32_t aWidth,
                             uint32_t aHeight, uint32_t* aStride) {
  fprintf(stderr, "Map1\n");
  /*return MapInternal(aX, aY, aWidth, aHeight, aStride,
                     GBM_BO_TRANSFER_READ_WRITE);*/
  return nullptr;
}

void* UdmabufSurfaceRGBA::Map(uint32_t* aStride) {
  return MapInternal(0, 0, mWidth[0], mHeight[0], aStride);
}

already_AddRefed<gfx::DrawTarget> UdmabufSurfaceRGBA::Lock() {
  LOGDMABUF("UdmabufSurfaceRGBA::Lock() UID %d", mUID);
  uint32_t stride = 0;
  void* data = Map(&stride);
  if (!data) {
    return nullptr;
  }
  RefPtr<gfx::DrawTarget> dt = gfx::Factory::CreateDrawTargetForData(
      gfx::BackendType::SKIA, static_cast<unsigned char*>(data), GetSize(),
      stride, GetFormat());
  if (!dt) {
    Unmap();
    return nullptr;
  }
  static gfx::UserDataKey sUnmapKey;
  dt->AddUserData(&sUnmapKey, do_AddRef(this).take(), [](void* aSurface) {
    RefPtr<UdmabufSurfaceRGBA> surface =
        dont_AddRef(static_cast<UdmabufSurfaceRGBA*>(aSurface));
    surface->Unmap();
  });
  return dt.forget();
}

void UdmabufSurface::Unmap() {
  if (mMappedRegion) {
    LOGDMABUF("UdmabufSurface::Unmap() UID %d\n", mUID);
    SyncDmaBuf(mDmabufFd->GetHandle(), DMA_BUF_SYNC_END);
    munmap(mMappedRegion, mSize);
    mMappedRegion = nullptr;
    mMappedRegionData = nullptr;
    mMappedRegionStride = 0;
  }
}

nsresult UdmabufSurface::BuildSurfaceDescriptorBuffer(
    SurfaceDescriptorBuffer& aSdBuffer, Image::BuildSdbFlags aFlags,
    const std::function<MemoryOrShmem(uint32_t)>& aAllocate) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

already_AddRefed<UdmabufSurfaceRGBA> UdmabufSurfaceRGBA::CreateUdmabufSurface(
    int aWidth, int aHeight, RefPtr<mozilla::widget::DRMFormat> aFormat) {
  auto surf = MakeRefPtr<UdmabufSurfaceRGBA>();
  if (!surf->Create(aWidth, aHeight, aFormat)) {
    return nullptr;
  }
  return surf.forget();
}
