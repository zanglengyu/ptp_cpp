#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <chrono>
#include <thread>

#include "ptp.h"
int main(int argc, char* argv[]) {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif
    QGuiApplication app(argc, argv);

    QQmlApplicationEngine engine;
    const QUrl url(QStringLiteral("qrc:/main.qml"));
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreated, &app,
        [url](QObject* obj, const QUrl& objUrl) {
            if (!obj && url == objUrl)
                QCoreApplication::exit(-1);
        },
        Qt::QueuedConnection);
    engine.load(url);
    Ptp ptp;

    Nanosecond n1;
    Nanosecond n2;

    n1.seconds = 1679996994;
    n1.na_seconds = 400000000;
    n2.seconds = 1679996995;
    n2.na_seconds = 800000000;
    Nanosecond ns2 = n2 / 2;
    ns2.printf_value();
    //    std::thread thread([&]() {
    //        Nanosecond ns0;
    //        for (int i = i; i != 10; ++i) {
    //            ns0 = ptp.get_cur_nanosecond();
    //            Nanosecond ns1 = ptp.get_cur_nanosecond();

    //            Nanosecond n2 = ns0 - ns1;
    //            ns0.printf_value();
    //            ns1.printf_value();

    //            n2.printf_value();
    //            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    //        }
    //    });

    // thread.detach();
    ptp.start();
    return app.exec();
}
