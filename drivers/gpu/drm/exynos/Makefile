#
# Makefile for the drm device driver.  This driver provides support for the
# Direct Rendering Infrastructure (DRI) in XFree86 4.1.0 and higher.

ccflags-y := -Iinclude/drm -Idrivers/gpu/drm/exynos
exynosdrm-y := exynos_drm_drv.o exynos_drm_encoder.o exynos_drm_connector.o \
		exynos_drm_crtc.o exynos_drm_fbdev.o exynos_drm_fb.o \
		exynos_drm_buf.o exynos_drm_gem.o exynos_drm_core.o \
		exynos_drm_plane.o

exynosdrm-$(CONFIG_DRM_EXYNOS_IOMMU) += exynos_drm_iommu.o
exynosdrm-$(CONFIG_DRM_EXYNOS_DMABUF) += exynos_drm_dmabuf.o
exynosdrm-$(CONFIG_DRM_EXYNOS_FIMD)	+= exynos_drm_fimd.o
exynosdrm-$(CONFIG_DRM_EXYNOS_HDMI)	+= exynos_hdmi.o exynos_mixer.o \
					   exynos_ddc.o exynos_hdmiphy.o \
					   exynos_drm_hdmi.o
exynosdrm-$(CONFIG_DRM_EXYNOS_IPP) += exynos_drm_ipp.o
exynosdrm-$(CONFIG_DRM_EXYNOS_GSC) += exynos_drm_gsc.o
exynosdrm-$(CONFIG_DRM_EXYNOS_SC) += exynos_drm_sc.o
exynosdrm-$(CONFIG_DRM_EXYNOS_DBG)	+= exynos_drm_dbg.o
exynosdrm-$(CONFIG_DRM_EXYNOS_VIDI)	+= exynos_drm_vidi.o
exynosdrm-$(CONFIG_DRM_EXYNOS_LOGO) += exynos_drm_logo.o
exynosdrm-$(CONFIG_DEBUG_FS) += exynos_drm_debugfs.o

obj-$(CONFIG_DRM_EXYNOS)		+= exynosdrm.o
