#pragma once

#include <QByteArrayView>
#include <QtGlobal>

namespace niconeon {
// Select the same real OpenGL backend in the application and integration tests.
// Qt tries EGL first and retains GLX as fallback. Explicit user selection wins.
inline void configureGraphicsEnvironment() {
#ifdef Q_OS_LINUX
    if (qEnvironmentVariableIsEmpty("QT_XCB_GL_INTEGRATION"))
        qputenv("QT_XCB_GL_INTEGRATION", "xcb_egl");
#endif
#ifdef Q_OS_WIN
    // System GPU drivers ignore this Mesa-specific variable. If Qt falls back
    // to bundled opengl32sw.dll, choose its CPU rasterizer, not D3D12/WARP.
    if (qEnvironmentVariableIsEmpty("GALLIUM_DRIVER"))
        qputenv("GALLIUM_DRIVER", "llvmpipe");
#endif
}
} // namespace niconeon
