// The application imports XCB itself. Import the already-built minimal plugin
// as well so help/version and GUI smoke checks work without an X server.
#include <QtPlugin>

Q_IMPORT_PLUGIN(QMinimalIntegrationPlugin)
