// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignalGui::GuiApplication -- a CoreApplication that owns a window system.

#ifndef QT_LIKE_SIGNAL_GUI_GUIAPPLICATION_HPP
#define QT_LIKE_SIGNAL_GUI_GUIAPPLICATION_HPP

#include "QtLikeSignal/CoreApplication.hpp"

#include "QtLikeSignalGui/InputEvents.hpp"
#include "QtLikeSignalGui/PlatformType.hpp"
#include "QtLikeSignalGui/Window.hpp"

#include <memory>
#include <string>
#include <vector>

namespace QtLikeSignalGui
{
    class PlatformIntegration;

    //! An application with a window system: CoreApplication plus windows and input.
    //!
    //! The same shape as Qt's QGuiApplication over QCoreApplication -- it selects a platform
    //! backend, owns it, creates windows through it, and holds the global input state -- but
    //! deliberately much smaller. There is no screen list, no palette, no clipboard, no cursor
    //! stack, and no style hints, because none of those are what this library is for.
    //!
    //! @code
    //!   int main( int argc, char** argv )
    //!   {
    //!       GuiApplication app( argc, argv );
    //!
    //!       Window* window = app.createWindow( { 1280, 800, "My App" } );
    //!       if( window == nullptr )
    //!       {
    //!           return 1;
    //!       }
    //!
    //!       myLibrary.initWgl( window->nativeHandle() );   // WGL/EGL belongs to the caller
    //!
    //!       Renderer renderer;
    //!       QtLikeSignal::Object::connect( window->getExposed(), &renderer, &Renderer::onDraw );
    //!       QtLikeSignal::Object::connect( window->getResized(), &renderer, &Renderer::onResize );
    //!
    //!       window->show();
    //!       const int result = app.exec();
    //!
    //!       myLibrary.shutdownWgl();     // before the window, never after
    //!       return result;
    //!   }
    //! @endcode
    //!
    //! **There is no message loop in a program built on this.** exec() runs the inherited
    //! QtLikeSignal loop, and it is EventDispatcherWin32::processPlatformEvents() that pumps the OS
    //! messages inside it. An external library that offers a pollEvents() of its own must not be
    //! asked to pump on Windows: two PeekMessage loops on one thread queue is how the dispatcher's
    //! own wakeup message gets eaten by the wrong reader.
    //!
    //! **Which backend runs** is decided by choosePlatform(): the `-p` argument first (`-p x11`,
    //! `-p wayland`), then the QTLIKESIGNAL_PLATFORM environment variable, then autodetection. On
    //! Windows there is only ever one answer.
    //!
    //! Must be constructed on the thread that will call exec(), like CoreApplication itself, and
    //! for the extra reason that a native window belongs to the thread that creates it.
    class GuiApplication : public QtLikeSignal::CoreApplication
    {
    public:
        GuiApplication();

        GuiApplication
            (
            int aArgc,
            char** aArgv
            );

        ~GuiApplication() override;

        static GuiApplication* instance();

        //! Gets which window system is in use, or Unknown if no backend could be created.
        //!
        //! Compare against this rather than against platformName(): the name exists for messages,
        //! and comparing it would put back the string matching PlatformType removes.
        PlatformType platformType() const
        {
            return mPlatformType;
        }

        //! Gets a name for the backend in use, for a diagnostic or a log line. Never null.
        const char* platformName() const
        {
            return platformTypeName( mPlatformType );
        }

        //! Returns true if a platform backend was created and windows can be made.
        //!
        //! False means the process has a working event loop but no window system -- the state a
        //! Linux build is in until the X11 and Wayland backends land. Checking this is friendlier
        //! than discovering it from a null createWindow().
        bool hasPlatform() const
        {
            return mIntegration != nullptr;
        }

        Window* createWindow
            (
            const WindowSettings& aSettings
            );

        Window* adoptWindow
            (
            const NativeWindow& aNative
            );

        void* nativeDisplay();

        std::vector<Window*> windows() const;

        //! Sets whether the loop quits when the last visible window is closed. Default: true.
        void setQuitOnLastWindowClosed
            (
            bool aQuit   //!< True to quit automatically.
            )
        {
            mQuitOnLastWindowClosed = aQuit;
        }

        //! Returns whether the loop quits when the last visible window is closed.
        bool quitOnLastWindowClosed() const
        {
            return mQuitOnLastWindowClosed;
        }

        static MouseButtons mouseButtons();

        static Window* focusWindow();

        static PlatformType choosePlatform
            (
            const std::vector<std::string>& aArgs
            );

    private:
        void initPlatform();

        void handleCloseRequested
            (
            Window* aWindow
            );

        //! The process-wide instance, as CoreApplication keeps its own.
        //!
        //! A second static rather than down-casting CoreApplication::instance(), which may legally
        //! be a plain CoreApplication that this class must not pretend to be.
        static GuiApplication* sGuiInstance;

        //! The backend, or null when none could be created. Outlives every window it made.
        std::unique_ptr<PlatformIntegration> mIntegration;

        //! Which window system is in use, or Unknown when no backend could be created.
        PlatformType mPlatformType { PlatformType::Unknown };

        //! See quitOnLastWindowClosed().
        bool mQuitOnLastWindowClosed { true };

        //! Reaches handleCloseRequested() when a window's close box is used.
        friend class WindowSystemInterface;
    };
}

#endif // QT_LIKE_SIGNAL_GUI_GUIAPPLICATION_HPP
