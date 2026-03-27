#pragma once
/*
 * Copyright Bupa Australia Pty Ltd
 * Author: Semih Cemiloglu
 *
 */
#ifndef SIGNALHANDLER_HH
#define SIGNALHANDLER_HH

#include <QObject>
#ifndef Q_OS_WIN
#  include <QSocketNotifier>
#endif

class SignalHandler : public QObject {
    Q_OBJECT
public:
    explicit SignalHandler(QObject *parent = nullptr);
    static void setupSignalHandlers();
    
signals:
    void quitRequested();

private slots:
    void handleSignal();

private:
#ifndef Q_OS_WIN
    static int       signalFd[2];
    QSocketNotifier *notifier = nullptr;
#endif

};

#endif 
