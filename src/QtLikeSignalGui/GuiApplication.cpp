// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! GuiApplication implementation: choosing a backend, making windows, and the close policy.

#include "QtLikeSignalGui/GuiApplication.hpp"

#include "QtLikeSignalGui/PlatformIntegration.hpp"
#include "QtLikeSignalGui/WindowSystemInterface.hpp"

#include "QtLikeSignal/Log.hpp"
#include "QtLikeSignalGui/LogCategories.hpp"

#include <cstdio>

namespace QtLikeSignalGui
{
    GuiApplication* GuiApplication::sGuiInstance { nullptr };

    //! Constructs an application with no command-line arguments.
    //!
    //! The backend is then chosen from the environment alone, since there is no `-p` to read.
    //! Useful for a test or for a program that embeds the loop and has no argv to pass on.
    GuiApplication::GuiApplication()
        : QtLikeSignal::CoreApplication()
    {
        initPlatform();
    }

    //! Constructs an application from the program's arguments, and selects a backend using them.
    GuiApplication::GuiApplication
        (
        int aArgc,     //!< Argument count, as handed to main().
        char** aArgv   //!< Argument vector, as handed to main().
        )
        : QtLikeSignal::CoreApplication( aArgc, aArgv )
    {
        initPlatform();
    }

    //! Destroys the application, its windows, and then the backend that made them.
    //!
    //! **That order is the reason this destructor exists at all.** Windows are children of the
    //! application, so ~Object() would eventually delete them -- but ~Object() runs after this
    //! body, by which time mIntegration would already have been released, and ~Window() calls into
    //! the backend to destroy its native window. Deleting them here, first, is what keeps the
    //! backend alive for exactly as long as something can still ask it for anything.
    //!
    //! Deleting a child detaches it from the parent as it goes, so the later sweep in ~Object()
    //! finds nothing left to do rather than a list of freed pointers.
    GuiApplication::~GuiApplication()
    {
        const std::vector<Window*> owned = windows();
        for( Window* window : owned )
        {
            delete window;
        }

        mIntegration.reset();

        if( sGuiInstance == this )
        {
            sGuiInstance = nullptr;
        }
    }

    //! Registers this as the instance and creates the platform backend.
    //!
    //! Shared by both constructors. A failure to find a backend is reported and survived rather
    //! than being fatal: the event loop, the timers and the signals all work without a window
    //! system, and a program that finds itself on a headless machine is better served by a null
    //! createWindow() it can check than by a process that will not start. Qt calls qFatal() here,
    //! but QGuiApplication exists only to have windows.
    void GuiApplication::initPlatform()
    {
        if( sGuiInstance == nullptr )
        {
            sGuiInstance = this;
        }
        else
        {
            // CoreApplication warns about this too, and for the same reason keeps the first
            // instance rather than aborting. Reported separately because the two statics are
            // independent: a program could construct a CoreApplication and a GuiApplication.
            qCWarning( gLogGui )
                << "GuiApplication: there should be only one application object; the existing one"
                << "is kept and this one will not be reachable through instance()";
        }

        mPlatformType = choosePlatform( arguments() );
        mIntegration  = PlatformIntegration::create( mPlatformType );

        if( !mIntegration )
        {
            // Cleared so that platformType() and hasPlatform() cannot disagree: a value naming a
            // backend that was never created would be the one piece of state a caller could read
            // and act on wrongly.
            mPlatformType = PlatformType::Unknown;
        }
    }

    //! Gets the application instance, or nullptr if none has been constructed.
    GuiApplication* GuiApplication::instance()
    {
        return sGuiInstance;
    }

    //! Decides which backend to use. See PlatformIntegration::choosePlatform().
    PlatformType GuiApplication::choosePlatform
        (
        const std::vector<std::string>& aArgs   //!< The program's arguments, argv[0] included.
        )
    {
        return PlatformIntegration::choosePlatform( aArgs );
    }

    //! Creates a window, parented to this application.
    //!
    //! The window starts hidden, so that everything which has to happen before the first frame --
    //! initialising a GL context against nativeHandle(), connecting the signals -- can happen
    //! without a half-configured window being on screen. Call Window::show() when ready.
    //!
    //! @return the new Window, owned by this application, or nullptr if it could not be created.
    Window* GuiApplication::createWindow
        (
        const WindowSettings& aSettings   //!< Requested client size and caption.
        )
    {
        if( !mIntegration )
        {
            qCWarning( gLogGui )
                << "GuiApplication::createWindow: no platform backend; there is no window system to"
                << "create a window on";
            return nullptr;
        }

        if( !mIntegration->canCreateWindows() )
        {
            qCWarning( gLogGui )
                << "GuiApplication::createWindow: this backend does not create windows; it adopts"
                << "one created elsewhere. Backend"
                << platformTypeName( mIntegration->type() );
            return nullptr;
        }

        Window* const window = mIntegration->createWindow( aSettings );
        if( window == nullptr )
        {
            return nullptr;
        }

        // Parented so the application owns it, which is what makes windows() work and what
        // guarantees the window is destroyed before the backend that made it. A failure here would
        // mean the window is unowned, so it is destroyed rather than leaked.
        if( !window->setParent( this ) )
        {
            qCWarning( gLogGui )
                << "GuiApplication::createWindow: could not parent the new window to the"
                << "application";
            delete window;
            return nullptr;
        }

        return window;
    }

    //! Takes charge of a window an external library created, parenting it to this application.
    //!
    //! The counterpart of createWindow() for the platforms where this library does not create the
    //! window: on X11 and Wayland an external library makes the window and its GL context, and
    //! hands over the connection and the identifier. From that point the window behaves exactly
    //! like a created one -- same signals, same lifetime, same place in windows() -- except that
    //! destroying it stops the listening rather than destroying the window.
    //!
    //! @code
    //!   NativeWindow native;
    //!   native.mDisplay  = myLibrary.x11Display();    // the Display* the window was created on
    //!   native.mWindowId = myLibrary.x11Window();     // its Window id
    //!
    //!   Window* window = app.adoptWindow( native );
    //! @endcode
    //!
    //! **The connection must outlive this application.** Destroying the application releases every
    //! window it holds, and on X11 that release talks to the Display -- so closing the connection
    //! first leaves the release dereferencing freed memory, which is a crash rather than a
    //! diagnosable error. In practice: destroy the Window, or let the application go, before the
    //! external library closes anything. Declaring the library's objects before the GuiApplication
    //! is the simplest way to get that ordering, since destruction runs in reverse.
    //!
    //! @return the new Window, owned by this application, or nullptr if it could not be adopted.
    Window* GuiApplication::adoptWindow
        (
        const NativeWindow& aNative   //!< The native window to take charge of.
        )
    {
        if( !mIntegration )
        {
            qCWarning( gLogGui )
                << "GuiApplication::adoptWindow: no platform backend; there is no window system to"
                << "adopt a window from";
            return nullptr;
        }

        if( !mIntegration->canAdoptWindows() )
        {
            qCWarning( gLogGui )
                << "GuiApplication::adoptWindow: this backend creates its own windows; use"
                << "createWindow(). Backend" << platformTypeName( mIntegration->type() );
            return nullptr;
        }

        Window* const window = mIntegration->adoptWindow( aNative );
        if( window == nullptr )
        {
            return nullptr;
        }

        if( !window->setParent( this ) )
        {
            qCWarning( gLogGui )
                << "GuiApplication::adoptWindow: could not parent the adopted window to the"
                << "application";
            delete window;
            return nullptr;
        }

        return window;
    }

    //! Destroys one window now, rather than leaving it for the application's destructor.
    //!
    //! The explicit counterpart to the automatic sweep in ~GuiApplication(): it destroys exactly
    //! the window given -- native side and all -- and detaches it from the application. Use it to
    //! release a window, and free whatever the program bound to its handle, at a point of the
    //! program's choosing instead of only at application teardown.
    //!
    //! **Any GL context made current against the window's handle must be torn down first**, for the
    //! same reason ~Window() gives: destroying the window destroys the native window with it, so a
    //! context still bound to nativeHandle() would outlive the handle it was made for.
    //!
    //! Refused, with false, for a null pointer or a window this application does not own -- closing
    //! through the application that did not create it would delete a pointer this object has no
    //! claim to.
    //!
    //! **It does not apply quitOnLastWindowClosed().** Destroying the last window does not stop the
    //! event loop; only a close request from the window system does. Qt behaves the same way:
    //! QWindow::close() can quit the application, and deleting a QWindow cannot. A program that
    //! destroys its last window this way and wants to stop calls
    //! QtLikeSignal::CoreApplication::quit().
    //!
    //! @return true if the window was owned by this application and has been destroyed.
    bool GuiApplication::closeWindow
        (
        Window* aWindow   //!< A window from this application's createWindow() or adoptWindow().
        )
    {
        if( aWindow == nullptr )
        {
            qCWarning( gLogGui )
                << "GuiApplication::closeWindow: ignoring a null window";
            return false;
        }

        if( aWindow->parent() != this )
        {
            qCWarning( gLogGui )
                << "GuiApplication::closeWindow: this window is not owned by this application,"
                << "so it will not be closed";
            return false;
        }

        // Deleting a child detaches it from the parent as it goes, so windows() stops reporting it
        // and the destructor's sweep will not reach it a second time.
        delete aWindow;
        return true;
    }

    //! Gets the window system connection: the X11 Display*. Null where there is no such thing.
    //!
    //! **Ask for this before creating a window, if a GL library is involved.** On X11 the window's
    //! visual is fixed at creation and has to match the config the GL context was made for, so the
    //! order is: take the connection from here, let the library choose its FBConfig or EGLConfig on
    //! it, and pass the resulting visual to createWindow() through WindowSettings::mVisualId.
    //!
    //! @code
    //!   Display* display = static_cast<Display*>( app.nativeDisplay() );
    //!   GLXFBConfig config = myLibrary.chooseFbConfig( display );
    //!
    //!   WindowSettings settings;
    //!   settings.mVisualId = glXGetVisualFromFBConfig( display, config )->visualid;
    //!
    //!   Window* window = app.createWindow( settings );
    //!   myLibrary.initGlx( display, window->nativeWindowId() );
    //! @endcode
    //!
    //! Asking is what opens the connection, so this is not a passive query: on X11 the first call
    //! connects to the server. Null on Win32, which has no connection object, and on a build with
    //! no backend.
    //!
    //! @return the Display*, or nullptr.
    void* GuiApplication::nativeDisplay()
    {
        return mIntegration ? mIntegration->nativeDisplay() : nullptr;
    }

    //! Gets every window this application owns, oldest last.
    //!
    //! Read from the object tree rather than from a list of its own. A second list would be a
    //! second thing to keep in step with ~Window(), and the one that went stale would be the one
    //! holding freed pointers; the child list is already maintained by Object for exactly this.
    std::vector<Window*> GuiApplication::windows() const
    {
        std::vector<Window*> found;

        for( QtLikeSignal::Object* child = firstChild(); child != nullptr;
            child = child->nextSibling() )
        {
            Window* const window = dynamic_cast<Window*>( child );
            if( window != nullptr )
            {
                found.push_back( window );
            }
        }

        return found;
    }

    //! Applies the quit-on-last-window-closed policy after a close was requested.
    //!
    //! Hiding the window rather than destroying it is the difference between this and Qt, and it is
    //! deliberate: an application here owns its Window and may well still hold a GL context bound
    //! to its handle, so destroying it out from underneath that would be the library breaking
    //! something it does not own. Hidden is enough for the policy question -- "is any window still
    //! up?" -- and the destruction remains the application's to time.
    void GuiApplication::handleCloseRequested
        (
        Window* aWindow   //!< The window whose close was requested.
        )
    {
        if( !mQuitOnLastWindowClosed )
        {
            return;
        }

        aWindow->hide();

        const std::vector<Window*> owned = windows();
        for( const Window* window : owned )
        {
            if( window->isVisible() )
            {
                return;
            }
        }

        QtLikeSignal::CoreApplication::quit();
    }

    //! Gets every mouse button currently held. See WindowSystemInterface::mouseButtons().
    MouseButtons GuiApplication::mouseButtons()
    {
        return WindowSystemInterface::mouseButtons();
    }

    //! Gets the window with keyboard focus, or nullptr.
    Window* GuiApplication::focusWindow()
    {
        return WindowSystemInterface::focusWindow();
    }
}
