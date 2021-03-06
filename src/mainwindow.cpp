/***************************************************************************
**
** Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies).
** All rights reserved.
** Contact: Nokia Corporation (directui@nokia.com)
**
** This file is part of mhome.
**
** If you have questions regarding the use of this file, please contact
** Nokia at directui@nokia.com.
**
** This library is free software; you can redistribute it and/or
** modify it under the terms of the GNU Lesser General Public
** License version 2.1 as published by the Free Software Foundation
** and appearing in the file LICENSE.LGPL included in the packaging
** of this file.
**
****************************************************************************/

#include "mainwindow.h"
#include "homeapplication.h"
#include <QDBusInterface>
#include <QX11Info>
#include <QFile>
#include <QGLWidget>

#include "x11wrapper.h"

MainWindow *MainWindow::mainWindowInstance = NULL;
const QString MainWindow::CONTENT_SEARCH_DBUS_SERVICE = "com.nokia.maemo.meegotouch.ContentSearch";
const QString MainWindow::CONTENT_SEARCH_DBUS_PATH = "/";
const QString MainWindow::CONTENT_SEARCH_DBUS_INTERFACE = "com.nokia.maemo.meegotouch.ContentSearchInterface";
const QString MainWindow::CONTENT_SEARCH_DBUS_METHOD = "launch";

const QString MainWindow::CALL_UI_DBUS_SERVICE = "com.nokia.telephony.callhistory";
const QString MainWindow::CALL_UI_DBUS_PATH = "/callhistory";
const QString MainWindow::CALL_UI_DBUS_INTERFACE = "com.nokia.telephony.callhistory";
const QString MainWindow::CALL_UI_DBUS_METHOD = "dialer";

MainWindow::MainWindow(QWidget *parent) :
    QDeclarativeView(parent),
    home(NULL),
    externalServiceService(NULL),
    externalServicePath(NULL),
    externalServiceInterface(NULL),
    externalServiceMethod(NULL)
{
    mainWindowInstance = this;
    if (qgetenv("MEEGOHOME_DESKTOP") != "0") {
        // Dont Set the window type to desktop if MEEGOHOME_DESKTOP is set to 0
        setAttribute(Qt::WA_X11NetWmWindowTypeDesktop);
    }

#ifdef Q_WS_X11
    // Visibility change messages are required to make the appVisible() signal work
    WId window = winId();
    XWindowAttributes attributes;
    Display *display = QX11Info::display();
    X11Wrapper::XGetWindowAttributes(display, window, &attributes);
    X11Wrapper::XSelectInput(display, window, attributes.your_event_mask | VisibilityChangeMask);
#endif

    excludeFromTaskBar();

    // TODO: disable this for non-debug builds
    if (QFile::exists("main.qml"))
        setSource(QUrl::fromLocalFile("./main.qml"));
    else
        setSource(QUrl("qrc:/qml/main.qml"));

    setResizeMode(SizeRootObjectToView);

    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_NoSystemBackground);
    setViewport(new QGLWidget);
}

MainWindow::~MainWindow()
{
    mainWindowInstance = NULL;
}

MainWindow *MainWindow::instance(bool create)
{
    if (mainWindowInstance == NULL && create) {
        // The static instance variable is set in the constructor
        new MainWindow;
    }

    return mainWindowInstance;
}

void MainWindow::excludeFromTaskBar()
{
    // Tell the window to not to be shown in the switcher
    Atom skipTaskbarAtom = X11Wrapper::XInternAtom(QX11Info::display(), "_NET_WM_STATE_SKIP_TASKBAR", False);
    changeNetWmState(true, skipTaskbarAtom);

    // Also set the _NET_WM_STATE window property to ensure Home doesn't try to
    // manage this window in case the window manager fails to set the property in time
    Atom netWmStateAtom = X11Wrapper::XInternAtom(QX11Info::display(), "_NET_WM_STATE", False);
    QVector<Atom> atoms;
    atoms.append(skipTaskbarAtom);
    X11Wrapper::XChangeProperty(QX11Info::display(), internalWinId(), netWmStateAtom, XA_ATOM, 32, PropModeReplace, (unsigned char *)atoms.data(), atoms.count());
}

void MainWindow::changeNetWmState(bool set, Atom one, Atom two)
{
    XEvent e;
    e.xclient.type = ClientMessage;
    Display *display = QX11Info::display();
    Atom netWmStateAtom = X11Wrapper::XInternAtom(display, "_NET_WM_STATE", FALSE);
    e.xclient.message_type = netWmStateAtom;
    e.xclient.display = display;
    e.xclient.window = internalWinId();
    e.xclient.format = 32;
    e.xclient.data.l[0] = set ? 1 : 0;
    e.xclient.data.l[1] = one;
    e.xclient.data.l[2] = two;
    e.xclient.data.l[3] = 0;
    e.xclient.data.l[4] = 0;
    X11Wrapper::XSendEvent(display, RootWindow(display, x11Info().screen()), FALSE, (SubstructureNotifyMask | SubstructureRedirectMask), &e);
}

bool MainWindow::isCallUILaunchingKey(int key)
{
    // Numbers, *, + and # will launch the call UI
    return ((key >= Qt::Key_0 && key <= Qt::Key_9) || key == Qt::Key_Asterisk || key == Qt::Key_Plus || key == Qt::Key_NumberSign);
}

void MainWindow::keyPressEvent(QKeyEvent *event)
{
    int key = event->key();
    if (key < Qt::Key_Escape && !event->modifiers().testFlag(Qt::ControlModifier)) {
        // Special keys and CTRL-anything should do nothing
        QString keyPresses = event->text();
        if (!keyPresses.isEmpty()) {
            // Append keypresses to the presses to be sent
            keyPressesToBeSent.append(keyPresses);

            if (keyPressesBeingSent.isEmpty()) {
                // Select the service to send the keypresses to
                if (isCallUILaunchingKey(key)) {
                    setupExternalService(CALL_UI_DBUS_SERVICE, CALL_UI_DBUS_PATH, CALL_UI_DBUS_INTERFACE, CALL_UI_DBUS_METHOD);
                } else {
                    setupExternalService(CONTENT_SEARCH_DBUS_SERVICE, CONTENT_SEARCH_DBUS_PATH, CONTENT_SEARCH_DBUS_INTERFACE, CONTENT_SEARCH_DBUS_METHOD);
                }

                // Call the external service
                sendKeyPresses();
            }
        }
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // Don't allow closing the main window
    event->ignore();
}

void MainWindow::setupExternalService(const QString &service, const QString &path, const QString &interface, const QString &method)
{
    externalServiceService = &service;
    externalServicePath = &path;
    externalServiceInterface = &interface;
    externalServiceMethod = &method;
}

void MainWindow::sendKeyPresses()
{
    // Only one external service launch may be active at a time
    if (keyPressesBeingSent.isEmpty() && !keyPressesToBeSent.isEmpty() && externalServiceService != NULL && externalServicePath != NULL && externalServiceInterface != NULL && externalServiceMethod != NULL) {
        // Make an asynchronous call to the external service and send the keypresses to be sent
        QDBusInterface interface(*externalServiceService, *externalServicePath, *externalServiceInterface, QDBusConnection::sessionBus());
        interface.callWithCallback(*externalServiceMethod, (QList<QVariant>() << keyPressesToBeSent), this, SLOT(markKeyPressesSentAndSendRemainingKeyPresses()), SLOT(markKeyPressesNotSent()));

        // Keypresses that need to be sent are now being sent
        keyPressesBeingSent = keyPressesToBeSent;
        keyPressesToBeSent.clear();
    }
}

void MainWindow::markKeyPressesSentAndSendRemainingKeyPresses()
{
    // The keypresses that were being sent have now been sent
    keyPressesBeingSent.clear();

    // Send the remaining keypresses still to be sent (if any)
    sendKeyPresses();
}

void MainWindow::markKeyPressesNotSent()
{
    // Since the external service didn't launch prepend the sent keypresses to the keypresses to be sent but don't retry
    keyPressesToBeSent.prepend(keyPressesBeingSent);
    keyPressesBeingSent.clear();
}

