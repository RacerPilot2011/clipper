#include <QApplication>
#include <QMessageBox>
#include "MainWindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    
    app.setApplicationName("Screen Clip Recorder");
    app.setOrganizationName("ScreenClip");
    app.setApplicationVersion("1.0.0");
    
    // Check for required permissions on macOS
#ifdef __APPLE__
    // Note: macOS requires explicit permission requests
    // These should be handled in Info.plist
#endif
    
    MainWindow window;
    window.show();
    
    return app.exec();
}