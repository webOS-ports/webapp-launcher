/*
 * Copyright (C) 2013 Simon Busch <morphis@gravedo.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */

#include <QDebug>
#include <QQmlContext>
#include <QQmlComponent>
#include <QtWebKit/private/qquickwebview_p.h>
#ifndef WITH_UNMODIFIED_QTWEBKIT
#include <QtWebKit/private/qwebnewpagerequest_p.h>
#endif
#include <QtGui/QGuiApplication>
#include <QtGui/qpa/qplatformnativeinterface.h>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include <QScreen>

#include <Settings.h>

#include "applicationdescription.h"
#include "webapplication.h"
#include "webapplicationwindow.h"
#include "webapplicationplugin.h"

#include "extensions/palmsystemextension.h"
#include "extensions/palmservicebridgeextension.h"

namespace luna
{

WebApplicationWindow::WebApplicationWindow(WebApplication *application, const QUrl& url,
                                           const QString& windowType, const QSize& size,
                                           bool headless, QObject *parent) :
    ApplicationEnvironment(parent),
    mApplication(application),
    mEngine(this),
    mRootItem(0),
    mWindow(0),
    mHeadless(headless),
    mUrl(url),
    mWindowType(windowType),
    mKeepAlive(false),
    mStagePreparing(true),
    mStageReady(false),
    mShowWindowTimer(this),
    mSize(size)
{
    connect(&mShowWindowTimer, SIGNAL(timeout()), this, SLOT(onShowWindowTimeout()));
    mShowWindowTimer.setSingleShot(true);

    assignCorrectTrustScope();

    createAndSetup();
}

WebApplicationWindow::~WebApplicationWindow()
{
    delete mRootItem;
}

void WebApplicationWindow::assignCorrectTrustScope()
{
    if (mUrl.scheme() == "file")
        mTrustScope = TrustScopeSystem;
    else
        mTrustScope = TrustScopeRemote;
}

void WebApplicationWindow::setWindowProperty(const QString &name, const QVariant &value)
{
    QPlatformNativeInterface *nativeInterface = QGuiApplication::platformNativeInterface();
    nativeInterface->setWindowProperty(mWindow->handle(), name, value);
}

void WebApplicationWindow::createAndSetup()
{
    if (mTrustScope == TrustScopeSystem) {
        mUserScripts.append(QUrl("qrc:///qml/webos-api.js"));
        createDefaultExtensions();
    }

    mEngine.rootContext()->setContextProperty("webApp", mApplication);
    mEngine.rootContext()->setContextProperty("webAppWindow", this);
    mEngine.rootContext()->setContextProperty("webAppUrl", mUrl);

    connect(&mEngine, &QQmlEngine::quit, [=]() {
        mWindow->close();
    });

    QQmlComponent windowComponent(&mEngine,
        QUrl(QString("qrc:///qml/%1.qml").arg(mHeadless ? "ApplicationContainer" : "Window")));
    if (windowComponent.isError()) {
        qCritical() << "Errors while loading window component:";
        qCritical() << windowComponent.errors();
        return;
    }

    mRootItem = windowComponent.create();
    if (!mRootItem) {
        qCritical() << "Failed to create application window:";
        qCritical() << windowComponent.errors();
        return;
    }

    if (!mHeadless) {
        mWindow = static_cast<QQuickWindow*>(mRootItem);
        mWindow->installEventFilter(this);

        mWindow->reportContentOrientationChange(QGuiApplication::primaryScreen()->primaryOrientation());

        mWindow->setSurfaceType(QSurface::OpenGLSurface);
        QSurfaceFormat surfaceFormat = mWindow->format();
        surfaceFormat.setAlphaBufferSize(8);
        surfaceFormat.setRenderableType(QSurfaceFormat::OpenGLES);
        mWindow->setFormat(surfaceFormat);

        // make sure the platform window gets created to be able to set it's
        // window properties
        mWindow->create();

        // set different information bits for our window
        setWindowProperty(QString("appId"), QVariant(mApplication->id()));
        setWindowProperty(QString("type"), QVariant(mWindowType));
    }

    mWebView = mRootItem->findChild<QQuickWebView*>("webView");

    connect(mWebView, SIGNAL(loadingChanged(QWebLoadRequest*)),
            this, SLOT(onLoadingChanged(QWebLoadRequest*)));

#ifndef WITH_UNMODIFIED_QTWEBKIT
    connect(mWebView->experimental(), SIGNAL(createNewPage(QWebNewPageRequest*)),
            this, SLOT(onCreateNewPage(QWebNewPageRequest*)));
    connect(mWebView->experimental(), SIGNAL(closePage()), this, SLOT(onClosePage()));
    connect(mWebView->experimental(), SIGNAL(syncMessageReceived(const QVariantMap&, QString&)),
            this, SLOT(onSyncMessageReceived(const QVariantMap&, QString&)));
#endif

    connect(mWindow, &QQuickWindow::activeChanged, [=]() {
        emit activeChanged();
    });

    if (mTrustScope == TrustScopeSystem)
        initializeAllExtensions();

    /* If we're running a remote site mark the window as fully loaded */
    if (mTrustScope == TrustScopeRemote)
        stageReady();
}

void WebApplicationWindow::onShowWindowTimeout()
{
    qDebug() << __PRETTY_FUNCTION__;

    // we got no stage ready call yet so go forward showing the window
    stageReady();
}

void WebApplicationWindow::setupPage()
{
    qreal zoomFactor = Settings::LunaSettings()->layoutScale;

    // correct zoom factor for some applications which are not scaled properly (aka
    // the Open webOS core-apps ...)
    if (Settings::LunaSettings()->compatApps.find(mApplication->id().toStdString()) !=
        Settings::LunaSettings()->compatApps.end())
        zoomFactor = Settings::LunaSettings()->layoutScaleCompat;

    mWebView->setZoomFactor(zoomFactor);

    show();

    // We need to finish the stage preparation in case of a remote entry point
    // otherwise it will never stop loading
    if (mApplication->hasRemoteEntryPoint())
        stageReady();
}

void WebApplicationWindow::notifyAppAboutFocusState(bool focus)
{
    qDebug() << "DEBUG: We become" << (focus ? "focused" : "unfocused");

    QString action = focus ? "stageActivated" : "stageDeactivated";

    if (mTrustScope == TrustScopeSystem)
        executeScript(QString("if (window.Mojo && Mojo.%1) Mojo.%1()").arg(action));

    mApplication->changeActivityFocus(focus);
}

void WebApplicationWindow::onLoadingChanged(QWebLoadRequest *request)
{
    switch (request->status()) {
    case QQuickWebView::LoadStartedStatus:
        setupPage();
        return;
    case QQuickWebView::LoadStoppedStatus:
    case QQuickWebView::LoadFailedStatus:
        return;
    case QQuickWebView::LoadSucceededStatus:
        break;
    }

    Q_FOREACH(BaseExtension *extension, mExtensions.values())
        extension->initialize();

    // If we're a headless app we don't show the window and in case of an
    // application with an remote entry point it's already visible at
    // this point
    if (mHeadless || mApplication->hasRemoteEntryPoint())
        return;

    // If we don't got stageReady() start a timeout to wait for it
    else if (mStagePreparing && !mStageReady && !mShowWindowTimer.isActive())
        mShowWindowTimer.start(3000);
}

#ifndef WITH_UNMODIFIED_QTWEBKIT

void WebApplicationWindow::onCreateNewPage(QWebNewPageRequest *request)
{
    mApplication->createWindow(request);
}

void WebApplicationWindow::onClosePage()
{
    qDebug() << __PRETTY_FUNCTION__;
    mWindow->close();
}

void WebApplicationWindow::onSyncMessageReceived(const QVariantMap& message, QString& response)
{
    if (!message.contains("data"))
        return;

    QString data = message.value("data").toString();

    QJsonDocument document = QJsonDocument::fromJson(data.toUtf8());

    if (!document.isObject())
        return;

    QJsonObject rootObject = document.object();

    QString messageType;
    if (!rootObject.contains("messageType") || !rootObject.value("messageType").isString())
        return;

    messageType = rootObject.value("messageType").toString();
    if (messageType != "callSyncExtensionFunction")
        return;

    if (!(rootObject.contains("extension") && rootObject.value("extension").isString()) ||
        !(rootObject.contains("func") && rootObject.value("func").isString()) ||
        !(rootObject.contains("params") && rootObject.value("params").isArray()))
        return;

    QString extensionName = rootObject.value("extension").toString();
    QString funcName = rootObject.value("func").toString();
    QJsonArray params = rootObject.value("params").toArray();

    if (!mExtensions.contains(extensionName))
        return;

    BaseExtension *extension = mExtensions.value(extensionName);
    response = extension->handleSynchronousCall(funcName, params);
}

#endif

void WebApplicationWindow::createDefaultExtensions()
{
    addExtension(new PalmSystemExtension(this));
    // addExtension(new PalmServiceBridgeExtension(this));

    if (!mApplication->plugin())
        return;

    QList<BaseExtension*> extensions = mApplication->plugin()->createExtensions(this);
    Q_FOREACH(BaseExtension *extension, extensions) {
        addExtension(extension);
    }
}

void WebApplicationWindow::addExtension(BaseExtension *extension)
{
    qDebug() << "Adding extension" << extension->name();
    mExtensions.insert(extension->name(), extension);
}

void WebApplicationWindow::initializeAllExtensions()
{
    foreach(BaseExtension *extension, mExtensions.values()) {
        qDebug() << "Initializing extension" << extension->name();
        emit extensionWantsToBeAdded(extension->name(), extension);
    }
}

void WebApplicationWindow::onClosed()
{
    emit closed();
}

bool WebApplicationWindow::eventFilter(QObject *object, QEvent *event)
{
    if (object == mWindow) {
        switch (event->type()) {
        case QEvent::Close:
            QTimer::singleShot(0, this, SLOT(onClosed()));
            break;
        case QEvent::FocusIn:
            notifyAppAboutFocusState(true);
            break;
        case QEvent::FocusOut:
            notifyAppAboutFocusState(false);
            break;
        default:
            break;
        }
    }

    return false;
}

QString WebApplicationWindow::getIdentifierForFrame(const QString& id, const QString& url)
{
    QString identifier = mApplication->identifier();

    if (url.startsWith("file:///usr/lib/luna/system/luna-systemui/"))
        identifier = QString("com.palm.systemui %1").arg(mApplication->processId());

    qDebug() << __PRETTY_FUNCTION__ << "Decided identifier for frame" << id << "is" << identifier;

    return identifier;
}

void WebApplicationWindow::stagePreparing()
{
    mStagePreparing = true;
    emit readyChanged();
}

void WebApplicationWindow::stageReady()
{
    mStagePreparing = false;
    mStageReady = true;

    emit readyChanged();

    mShowWindowTimer.stop();
}

void WebApplicationWindow::show()
{
    if (!mWindow)
        return;

    mWindow->show();
}

void WebApplicationWindow::hide()
{
    if (!mWindow)
        return;

    mWindow->hide();
}

void WebApplicationWindow::focus()
{
    if (!mWindow)
        return;

    /* When we're closed we have to make sure we're visible before
     * raising ourself */
    if (!mWindow->isVisible())
        mWindow->show();

    mWindow->raise();
}

void WebApplicationWindow::unfocus()
{
    if (!mWindow)
        return;

    mWindow->lower();
}

void WebApplicationWindow::executeScript(const QString &script)
{
    emit javaScriptExecNeeded(script);
}

void WebApplicationWindow::registerUserScript(const QUrl &path)
{
    mUserScripts.append(path);
}


WebApplication* WebApplicationWindow::application() const
{
    return mApplication;
}

QQuickWebView *WebApplicationWindow::webView() const
{
    return mWebView;
}

void WebApplicationWindow::setKeepAlive(bool keepAlive)
{
    mKeepAlive = keepAlive;
}

bool WebApplicationWindow::keepAlive() const
{
    return mKeepAlive;
}

bool WebApplicationWindow::headless() const
{
    return mHeadless;
}

QList<QUrl> WebApplicationWindow::userScripts() const
{
    return mUserScripts;
}

bool WebApplicationWindow::ready() const
{
    return mStageReady && !mStagePreparing;
}

QSize WebApplicationWindow::size() const
{
    return mSize;
}

bool WebApplicationWindow::active() const
{
    return mWindow->isActive();
}

QString WebApplicationWindow::trustScope() const
{
    if (mTrustScope == TrustScopeSystem)
        return QString("system");

    return QString("remote");
}

} // namespace luna
