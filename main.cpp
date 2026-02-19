#include <QApplication>
#include <QMessageBox>
#include "MainWindow.h"

/*
 * main.cpp
 *
 * Entry point for the Screen Clip Recorder application.
 *
 * Responsibilities and behaviour:
 * - Initialize the Qt application object which manages application-wide
 *   resources (event loop, application metadata, platform integration).
 * - Set stable application metadata used by Qt (application name,
 *   organization name and version). These values are used by Qt classes
 *   such as QSettings and for platform-specific integration points.
 * - Instantiate the top-level `MainWindow` object. The `MainWindow`
 *   encapsulates UI layout, wiring between subsystems (screen capture,
 *   audio capture, encoding) and user-facing commands.
 * - Show the main window and enter the Qt event loop via `app.exec()`.
 *
 * Platform notes:
 * - On macOS the application must also declare and request capability
 *   permissions (screen recording, microphone access) from the system.
 *   Those permission strings and Info.plist entries live outside of C++
 *   code; the application should fail gracefully if permissions are not
 *   granted.
 *
 * Threading and lifetime:
 * - `QApplication` must be created on the main thread. All GUI widgets
 *   are expected to live on the same thread. Worker threads used by the
 *   app (screen capture, audio capture, encoding) are created and
 *   managed by `MainWindow` and use Qt's thread primitives.
 */

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // Provide application metadata early so other subsystems can query it
    app.setApplicationName("Screen Clip Recorder");
    app.setOrganizationName("ScreenClip");
    app.setApplicationVersion("1.0.0");

#ifdef __APPLE__
    // macOS permissions for screen and microphone capture must be set in
    // the application's Info.plist. The runtime should detect missing
    // permissions and present user-facing guidance; that behavior is
    // implemented inside the UI layers (MainWindow) rather than here.
#endif

    MainWindow window; // constructs UI and wires subsystems
    window.show();

    return app.exec();
}