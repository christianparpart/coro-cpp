// SPDX-License-Identifier: Apache-2.0
//
// Pure Qt/QML demo: a button kicks off three sequential simulated network
// fetches written as a linear coroutine script in FetchBackend, while the
// QML UI stays fully responsive (the BusyIndicator never stops spinning).
// A Cancel button aborts the in-flight sequence via std::stop_token.

#include <QtGui/QGuiApplication>
#include <QtQml/QQmlApplicationEngine>

/// Entry point: start the Qt event loop and load the QML scene. The
/// FetchBackend element is registered with QML by the build's
/// qt_add_qml_module (QML_ELEMENT in the header).
/// @param argc Argument count.
/// @param argv Argument vector.
/// @return Qt application exit code.
int main(int argc, char* argv[])
{
    auto app = QGuiApplication { argc, argv };

    auto engine = QQmlApplicationEngine {};
    // The CoroDemo QML module is embedded as a Qt resource; make its prefix
    // discoverable so loadFromModule can resolve it regardless of the
    // resource-prefix policy the Qt version defaulted to.
    engine.addImportPath(":/");
    engine.addImportPath(":/qt/qml");
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        [] { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);
    engine.loadFromModule("CoroDemo", "Main");

    return QGuiApplication::exec();
}
