// Isolated diagnostic, never linked into the application or acceptance benchmark.
// Intentional glFinish brackets establish an independent completed-work CPU clock.
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QOffscreenSurface>
#include <QOpenGLBuffer>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFramebufferObject>
#include <QOpenGLShaderProgram>
#include <QOpenGLTimerQuery>
#include <QSurfaceFormat>

int main(int argc, char **argv) {
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CompatibilityProfile);
    QSurfaceFormat::setDefaultFormat(format);
    QGuiApplication app(argc, argv);
    if (argc != 2)
        return 2;
    QOpenGLContext context;
    context.setFormat(format);
    if (!context.create())
        return 3;
    QOffscreenSurface surface;
    surface.setFormat(context.format());
    surface.create();
    if (!surface.isValid() || !context.makeCurrent(&surface) || context.isOpenGLES())
        return 4;
    auto *gl = context.extraFunctions();
    gl->initializeOpenGLFunctions();
    QOpenGLShaderProgram program;
    if (!program.addShaderFromSourceCode(QOpenGLShader::Vertex,
                                         "#version 150\nin vec2 p; void main(){gl_Position=vec4(p,0,1);}") ||
        !program.addShaderFromSourceCode(QOpenGLShader::Fragment,
                                         "#version 150\nout vec4 color; void main(){color=vec4(0.01);}"))
        return 5;
    program.bindAttributeLocation("p", 0);
    if (!program.link() || !program.bind())
        return 6;
    QOpenGLBuffer vertices;
    if (!vertices.create() || !vertices.bind())
        return 7;
    const float triangle[]{-1, -1, 3, -1, -1, 3};
    vertices.allocate(triangle, sizeof(triangle));
    gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    gl->glEnableVertexAttribArray(0);
    QOpenGLTimerQuery start, end;
    const bool startCreated = start.create();
    const bool endCreated = end.create();
    if (!startCreated || !endCreated)
        return 8;
    QJsonArray trials;
    bool valid = true;
    for (int size : {64, 256, 768}) {
        QOpenGLFramebufferObject fbo(size, size);
        if (!fbo.isValid() || !fbo.bind())
            return 9;
        gl->glViewport(0, 0, size, size);
        gl->glEnable(GL_BLEND);
        gl->glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        for (bool partial : {false, true}) {
            for (int repeat = 0; repeat < 3; ++repeat) {
                gl->glDisable(GL_SCISSOR_TEST);
                gl->glClearColor(0, 0, 0, 0);
                gl->glClear(GL_COLOR_BUFFER_BIT);
                if (partial) {
                    gl->glEnable(GL_SCISSOR_TEST);
                    gl->glScissor(0, 0, size / 2, size / 2);
                }
                gl->glFinish();
                QElapsedTimer cpu;
                cpu.start();
                start.recordTimestamp();
                for (int draw = 0; draw < 128; ++draw)
                    gl->glDrawArrays(GL_TRIANGLES, 0, 3);
                end.recordTimestamp();
                gl->glFinish();
                const qint64 completedCpuNs = cpu.nsecsElapsed();
                const bool available = start.isResultAvailable() && end.isResultAvailable();
                const quint64 from = available ? start.waitForResult() : 0;
                const quint64 to = available ? end.waitForResult() : 0;
                unsigned char pixel[4]{};
                gl->glReadPixels(size / 4, size / 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
                const auto error = gl->glGetError();
                trials.append(QJsonObject{{"size", size},
                                          {"partial_quarter", partial},
                                          {"repeat", repeat},
                                          {"draws", 128},
                                          {"completed_cpu_ns", completedCpuNs},
                                          {"query_available", available},
                                          {"query_ns", qint64(to - from)},
                                          {"pixel_r", pixel[0]},
                                          {"gl_error", qint64(error)}});
                if (!available || to < from || pixel[0] < 100 || error != GL_NO_ERROR)
                    valid = false;
            }
        }
    }
    QFile output(QString::fromLocal8Bit(argv[1]));
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return 11;
    QJsonObject report{
        {"kind", "independent-gl-timestamp-control"},
        {"valid_control_draws", valid},
        {"gl_renderer", QString::fromLatin1(reinterpret_cast<const char *>(gl->glGetString(GL_RENDERER)))},
        {"gl_version", QString::fromLatin1(reinterpret_cast<const char *>(gl->glGetString(GL_VERSION)))},
        {"trials", trials},
        {"scope", "Known completed draws with intentional glFinish; diagnostic only, not app timing"}};
    output.write(QJsonDocument(report).toJson());
    return valid ? 0 : 10;
}
