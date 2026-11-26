/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef UdmabufSurface_h_
#define UdmabufSurface_h_

#include <stdint.h>

#include <functional>

#include "GLTypes.h"
#include "ImageContainer.h"
#include "mozilla/Mutex.h"
#include "mozilla/Span.h"
#include "mozilla/gfx/Types.h"
#include "mozilla/webgpu/ffi/wgpu.h"
#include "mozilla/widget/BufferSurface.h"
#include "mozilla/widget/DMABufFormats.h"
#include "mozilla/widget/va_drmcommon.h"
#include "nsISupportsImpl.h"

typedef void* EGLImageKHR;
typedef void* EGLSyncKHR;

namespace mozilla {
namespace gfx {
class DataSourceSurface;
class FileHandleWrapper;
}  // namespace gfx
namespace layers {
class MemoryOrShmem;
class SurfaceDescriptor;
class SurfaceDescriptorBuffer;
class SurfaceDescriptorDMABuf;
}  // namespace layers
namespace gl {
class GLContext;
}
namespace webgpu {
namespace ffi {
struct WGPUDMABufInfo;
}
}  // namespace webgpu
namespace widget {
class DMABufDeviceLock;
}  // namespace widget
}  // namespace mozilla

namespace mozilla::layers {
class PlanarYCbCrImage;
}

class UdmabufSurface : public BufferSurface {
 public:
  static already_AddRefed<UdmabufSurface> CreateUdmabufSurface(
      const mozilla::layers::SurfaceDescriptor& aDesc);

  // Export surface to another process via. SurfaceDescriptor.
  virtual bool Serialize(
      mozilla::layers::SurfaceDescriptor& aOutDescriptor) = 0;

  bool IsMapped() { return (mMappedRegion != nullptr); };
  void Unmap();

  UdmabufSurface* GetAsUdmabufSurface() override { return this; }
  already_AddRefed<mozilla::gfx::DataSourceSurface> GetAsSourceSurface()
      override;

  virtual nsresult BuildSurfaceDescriptorBuffer(
      mozilla::layers::SurfaceDescriptorBuffer& aSdBuffer,
      mozilla::layers::Image::BuildSdbFlags aFlags,
      const std::function<mozilla::layers::MemoryOrShmem(uint32_t)>& aAllocate);

  void FenceSet();
  void FenceWait(mozilla::gl::GLContext* aGLContext = nullptr);
  static void FenceWaitFd(RefPtr<mozilla::gl::GLContext> aGL,
                          RefPtr<mozilla::gfx::FileHandleWrapper> aSyncFd);

  void MaybeSemaphoreWait(GLuint aGlTexture);
  void SetSemaphoreFd(int aDuppedRawFd, bool aIsSyncFd = false);

  // Creates a global reference counter objects attached to the surface.
  // It's created as unreferenced, i.e. IsGlobalRefSet() returns false
  // right after GlobalRefCountCreate() call.
  //
  // The counter is shared by all surface instances across processes
  // so it tracks global surface usage.
  //
  // The counter is automatically referenced when a new surface instance is
  // created with SurfaceDescriptor (usually copied to another process over IPC)
  // and it's unreferenced when surface is deleted.
  //
  // So without any additional GlobalRefAdd()/GlobalRefRelease() calls
  // the IsGlobalRefSet() returns true if any other process use the surface.
  void GlobalRefCountCreate();
  void GlobalRefCountDelete();

  // If global reference counter was created by GlobalRefCountCreate()
  // returns true when there's an active surface reference.
  bool IsGlobalRefSet();

  // Returns the file descriptor of the global reference counter,
  // or 0 if the counter was not created. The descriptor stays owned
  // by the surface.
  int GetGlobalRefCountFd();

  // Query GlobalRefSet() once by a single poll() call and stores one result
  // per file descriptor to aRefSet. aRefCountFds holds descriptors obtained
  // by GetGlobalRefCountFd().
  static void GetGlobalRefsSet(mozilla::Span<const int> aRefCountFds,
                               nsTArray<bool>& aRefSet);

  // Add/Remove additional reference to the surface global reference counter.
  void GlobalRefAdd();
  void GlobalRefAddLocked(const mozilla::MutexAutoLock& aProofOfLock);

  void GlobalRefRelease();

  // Release all underlying data.
  virtual void ReleaseSurface() = 0;


  UdmabufSurface(SurfaceType aSurfaceType);

 protected:
  virtual bool Create(const mozilla::layers::SurfaceDescriptor& aDesc) = 0;

  // Import global ref count object from IPC by file descriptor.
  // This adds global ref count reference to the surface.
  void GlobalRefCountImport(int aFd);
  // Export global ref count object by file descriptor.
  int GlobalRefCountExport();

  // Returns true if the mem was actually released as it can be called
  // for empty surface too.
  [[nodiscard]] bool ReleaseDMABuf();

  void* MapInternal(uint32_t aX, uint32_t aY, uint32_t aWidth, uint32_t aHeight,
                    uint32_t* aStride);

  bool CloseFileDescriptors();

  virtual ~UdmabufSurface();

  RefPtr<mozilla::gfx::FileHandleWrapper> mDmabufFd;

  void* mMappedRegion;
  void* mMappedRegionData;
  uint32_t mMappedRegionStride;

  size_t mSize;

  RefPtr<mozilla::gfx::FileHandleWrapper> mSyncFd;
  RefPtr<mozilla::gfx::FileHandleWrapper> mSemaphoreFd;
  bool mSemaphoreFdIsSyncFd = false;

  // Inter process properties, used to share DMABuf among various processes
  // like RDD/Main.

  // Global refcount tracks DMABuf usage by rendering process,
  // it's used for surface recycle.
  int mGlobalRefCountFd = 0;

  mozilla::Mutex mSurfaceLock MOZ_UNANNOTATED;
};

class UdmabufSurfaceRGBA final : public UdmabufSurface {
 public:
  static already_AddRefed<UdmabufSurfaceRGBA> CreateUdmabufSurface(
      int aWidth, int aHeight,
      RefPtr<mozilla::widget::DRMFormat> aFormat = nullptr);

  bool Serialize(mozilla::layers::SurfaceDescriptor& aOutDescriptor) override;

  void ReleaseSurface() override;

  void* Map(uint32_t aX, uint32_t aY, uint32_t aWidth, uint32_t aHeight,
            uint32_t* aStride = nullptr);
  void* Map(uint32_t* aStride = nullptr);
  // Maps the surface and returns a DrawTarget for it. The surface is
  // unmapped when the DrawTarget is destroyed.
  already_AddRefed<mozilla::gfx::DrawTarget> Lock() override;

  UdmabufSurfaceRGBA();
  UdmabufSurfaceRGBA(const UdmabufSurfaceRGBA&) = delete;
  UdmabufSurfaceRGBA& operator=(const UdmabufSurfaceRGBA&) = delete;

 private:
  ~UdmabufSurfaceRGBA();

  bool CreateTexture(mozilla::gl::GLContext* aGLContext, int aPlane) override;

  bool Create(int aWidth, int aHeight,
              RefPtr<mozilla::widget::DRMFormat> aFormat = nullptr);

  bool Create(const mozilla::layers::SurfaceDescriptor& aDesc) override;

  bool ImportSurfaceDescriptor(const mozilla::layers::SurfaceDescriptor& aDesc);
};

#endif
