/*
 *
 * Copyright Bupa Australia Pty Ltd
 * Author: Semih Cemiloglu
 *
 */
#include "SignalHandler.hh"

#include <QCoreApplication>
#include <QDebug>

// ── Platform-specific includes ────────────────────────────────────────────────
#ifdef Q_OS_WIN
#  include <windows.h>        // SetConsoleCtrlHandler, CTRL_C_EVENT
#  include <QMetaObject>      // QMetaObject::invokeMethod for cross-thread call
#else
#  include <unistd.h>         // pipe, read, write
#  include <signal.h>         // sigaction, SIGINT, SIGTERM
#endif


// ── POSIX-only static members ─────────────────────────────────────────────────
#ifndef Q_OS_WIN
int SignalHandler::signalFd[2] = {-1, -1};
#endif


// ── Windows: global pointer so the console handler can reach the instance ─────
#ifdef Q_OS_WIN
static SignalHandler *g_signalHandler = nullptr;
#endif


// ──────────────────────────────────────────────────────────────────────────────
// Constructor
// ──────────────────────────────────────────────────────────────────────────────
SignalHandler::SignalHandler(QObject *parent)
    : QObject(parent)
{
#ifdef Q_OS_WIN
    g_signalHandler = this;
#else
    notifier = new QSocketNotifier(signalFd[0],
                                   QSocketNotifier::Read,
                                   this);
    connect(notifier, &QSocketNotifier::activated,
            this,     &SignalHandler::handleSignal);
#endif
}


// ──────────────────────────────────────────────────────────────────────────────
// setupSignalHandlers  — call once before constructing a SignalHandler instance
// ──────────────────────────────────────────────────────────────────────────────
void SignalHandler::setupSignalHandlers()
{
#ifdef Q_OS_WIN
    // Install a Windows console control handler.
    // It runs on a separate thread created by Windows, so we use
    // invokeMethod(Qt::QueuedConnection) to safely cross into the Qt main thread.
    SetConsoleCtrlHandler([](DWORD ctrlType) -> BOOL {
        if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
            if (g_signalHandler) {
                QMetaObject::invokeMethod(g_signalHandler,
                                          "handleSignal",
                                          Qt::QueuedConnection);
            }
            return TRUE;  // signal handled
        }
        return FALSE;     // pass other events to the next handler
    }, TRUE);

#else
    // POSIX: write one byte into a self-pipe from the signal handler,
    // then let QSocketNotifier wake up the Qt event loop safely.
    if (::pipe(signalFd) != 0)
        qFatal("SignalHandler: failed to create self-pipe");

    struct sigaction sa{};
    sa.sa_handler = [](int /*sig*/) {
        const char data = 1;
        // write() is async-signal-safe
        (void)::write(signalFd[1], &data, sizeof(data));
    };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    if (::sigaction(SIGINT,  &sa, nullptr) != 0)
        qFatal("SignalHandler: failed to install SIGINT handler");
    if (::sigaction(SIGTERM, &sa, nullptr) != 0)
        qFatal("SignalHandler: failed to install SIGTERM handler");
#endif
}


// ──────────────────────────────────────────────────────────────────────────────
// handleSignal  — always called on the Qt main thread
// ──────────────────────────────────────────────────────────────────────────────
void SignalHandler::handleSignal()
{
#ifndef Q_OS_WIN
    // Drain the pipe byte written by the signal handler
    char data;
    (void)::read(signalFd[0], &data, sizeof(data));
#endif

    qDebug() << "Signal received — requesting application quit";
    emit quitRequested();
}


