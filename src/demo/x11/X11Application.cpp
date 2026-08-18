// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! X11Application implementation: the display connection, its registration with the loop, and the
//! drain that turns X events into signals.

#include "X11Application.hpp"

#include "QtLikeSignal/EventDispatcherLinux.hpp"
#include "QtLikeSignal/Thread.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>

#include <poll.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

// Xlib defines None as a bare `0L` macro, and MouseEvent::Button has a None enumerator; the two
// cannot coexist. X11Application.hpp is included above, before the macro exists, so the enumerator
// is declared safely -- and undefining the macro here makes it usable again in the code below.
// Nothing in this file passes None to Xlib: where a null resource id is wanted, a literal 0 says
// exactly the same thing to the server without the macro.
#undef None

namespace QtLikeSignalDemo
{
    namespace
    {
        //! The events this demo asks the server to send.
        //!
        //! StructureNotifyMask is not optional even though the demo does not care about being
        //! moved: it is what delivers ConfigureNotify, and without the size it carries the back
        //! buffer would stay at its original size after the window manager resized the window.
        const long kEventMask = ExposureMask | ButtonPressMask | ButtonReleaseMask
            | PointerMotionMask | KeyPressMask | StructureNotifyMask;

        //! Maps an X11 button number to the button the demo reports.
        //!
        //! Buttons 4 and 5 are the wheel in X11's numbering rather than physical buttons, and this
        //! demo has nothing to scroll, so they fall through to None like any other high button.
        MouseEvent::Button buttonOf
            (
            unsigned int aButton   //!< XButtonEvent::button, 1-based.
            )
        {
            switch( aButton )
            {
            case Button1:
                return MouseEvent::Button::Left;

            case Button2:
                return MouseEvent::Button::Middle;

            case Button3:
                return MouseEvent::Button::Right;

            default:
                return MouseEvent::Button::None;
            }
        }

        //! Gets the running thread's dispatcher as an EventDispatcherLinux, or null if it is not
        //! one.
        //!
        //! It will not be one if no CoreApplication has adopted this thread yet, which is the
        //! mistake worth catching by name rather than by a crash later.
        std::shared_ptr<QtLikeSignal::EventDispatcherLinux> currentLinuxDispatcher()
        {
            QtLikeSignal::Thread* current = QtLikeSignal::Thread::currentThread();
            if( current == nullptr )
            {
                return nullptr;
            }
            return std::dynamic_pointer_cast<QtLikeSignal::EventDispatcherLinux>(
                current->eventDispatcher() );
        }
    }

    //! Constructs the application. The display connection is opened separately, by createWindow().
    //!
    //! Split for the same reason the Win32 demo splits it: opening a display can fail for reasons
    //! entirely outside the program -- no DISPLAY, no server, a rejected cookie -- and a
    //! constructor cannot report that.
    X11Application::X11Application()
        : QtLikeSignal::CoreApplication()
    {
    }

    //! Constructs the application with the program's command line, reachable through arguments().
    X11Application::X11Application
        (
        int aArgc,     //!< Argument count, as main() received it.
        char** aArgv   //!< Argument values, as main() received them.
        )
        : QtLikeSignal::CoreApplication( aArgc, aArgv )
    {
    }

    //! Unregisters the connection from the loop and tears the window down.
    X11Application::~X11Application()
    {
        // Unregistered first, and from this thread -- which is the loop's own thread, so
        // EventDispatcherLinux's contract makes the call synchronous: the callback will not run
        // again, not even for a readiness the current poll() round has already observed. That is
        // what makes it safe to free everything the callback touches immediately afterwards.
        if( mConnectionFd >= 0 )
        {
            const std::shared_ptr<QtLikeSignal::EventDispatcherLinux> dispatcher
                = currentLinuxDispatcher();
            if( dispatcher )
            {
                dispatcher->unregisterEventSource( mConnectionFd );
            }
            mConnectionFd = -1;
        }

        if( mDisplay != nullptr )
        {
            Display* const display = static_cast<Display*>( mDisplay );

            if( mBackBuffer != 0 )
            {
                XFreePixmap( display, mBackBuffer );
                mBackBuffer = 0;
            }
            if( mGraphicsContext != nullptr )
            {
                XFreeGC( display, static_cast<GC>( mGraphicsContext ) );
                mGraphicsContext = nullptr;
            }
            if( mWindow != 0 )
            {
                XDestroyWindow( display, mWindow );
                mWindow = 0;
            }

            XCloseDisplay( display );
            mDisplay = nullptr;
        }
    }

    //! Opens the display, creates and maps the window, and joins the connection to the event loop.
    //!
    //! The last step is the one that matters: XConnectionNumber() is handed to
    //! EventDispatcherLinux::registerEventSource(), after which the socket is polled together with
    //! the dispatcher's own wakeup descriptor and its timer deadline. Nothing in this program ever
    //! calls XNextEvent() from a loop of its own.
    //! @return true if the window exists and the connection is registered.
    bool X11Application::createWindow
        (
        const char* aTitle,   //!< Window title, as the window manager will show it.
        int aWidth,           //!< Requested width in pixels.
        int aHeight           //!< Requested height in pixels.
        )
    {
        const std::shared_ptr<QtLikeSignal::EventDispatcherLinux> dispatcher
            = currentLinuxDispatcher();
        if( !dispatcher )
        {
            std::fprintf( stderr,
                "X11Application: this thread is not running EventDispatcherLinux, so the display "
                "connection has no poll() set to join. Construct the application first.\n" );
            return false;
        }

        Display* const display = XOpenDisplay( nullptr );
        if( display == nullptr )
        {
            const char* const wanted = std::getenv( "DISPLAY" );
            std::fprintf( stderr, "X11Application: XOpenDisplay() failed (DISPLAY=%s)\n",
                wanted != nullptr ? wanted : "<unset>" );
            return false;
        }
        mDisplay = display;

        const int screen = DefaultScreen( display );

        mWindow = XCreateSimpleWindow( display, RootWindow( display, screen ), 0, 0,
            static_cast<unsigned int>( aWidth ), static_cast<unsigned int>( aHeight ), 0,
            BlackPixel( display, screen ), BlackPixel( display, screen ) );
        if( mWindow == 0 )
        {
            std::fprintf( stderr, "X11Application: XCreateSimpleWindow() failed\n" );
            return false;
        }
        mWidth  = aWidth;
        mHeight = aHeight;

        // No background, so the server does not clear the window before sending Expose. The paint
        // slot covers every pixel from the back buffer instead, and the clear would only be visible
        // as a flash between the two. Exactly what returning 1 from WM_ERASEBKGND does in the Win32
        // demo, said in X11's vocabulary.
        XSetWindowBackgroundPixmap( display, mWindow, 0 );

        XStoreName( display, mWindow, aTitle );
        XSelectInput( display, mWindow, kEventMask );

        // The close button. Without this the window manager closes the connection instead, which
        // reaches Xlib as an I/O error and takes the process down from inside its default handler.
        // With it the click arrives as an ordinary ClientMessage on the socket the loop is already
        // polling, and the program stops the way every other quit in it does.
        Atom deleteWindow = XInternAtom( display, "WM_DELETE_WINDOW", False );
        XSetWMProtocols( display, mWindow, &deleteWindow, 1 );
        mDeleteWindowAtom = deleteWindow;

        mGraphicsContext = XCreateGC( display, mWindow, 0, nullptr );
        if( mGraphicsContext == nullptr )
        {
            std::fprintf( stderr, "X11Application: XCreateGC() failed\n" );
            return false;
        }

        resizeBackBuffer( aWidth, aHeight );

        XMapWindow( display, mWindow );
        XFlush( display );

        mConnectionFd = XConnectionNumber( display );
        if( !dispatcher->registerEventSource( mConnectionFd, POLLIN,
            [this]( short aEvents )
            {
                pumpDisplay( aEvents );
            } ) )
        {
            std::fprintf( stderr, "X11Application: registerEventSource( %d ) was refused\n",
                mConnectionFd );
            mConnectionFd = -1;
            return false;
        }

        // One drain before the loop ever blocks. Everything above talked to the server, and a reply
        // read off the socket can bring events along with it -- they are in Xlib's queue now, with
        // an empty socket behind them. poll() would have nothing to report and this first batch
        // would wait for whatever unrelated thing happened next. Same reasoning as the residual
        // check at the end of pumpDisplay(), applied to the gap before the first pass.
        pumpDisplay( POLLIN );
        return true;
    }

    //! Drains every event the connection has and delivers each one, then flushes what the slots
    //! asked for.
    //!
    //! Called by the dispatcher, on the dispatcher's thread, whenever poll() reports the connection
    //! ready -- and once from createWindow() before the loop starts.
    //!
    //! **The loop is not decoration.** poll() reports that the *socket* has bytes, but Xlib turns
    //! those bytes into events and holds them in a queue inside this process. Handle one per
    //! readiness and the rest stay in that queue behind an empty socket: poll() has nothing left to
    //! report, so the loop blocks with events already in hand and the window appears frozen until
    //! something unrelated wakes it. XPending() flushes the output buffer, then hands back whatever
    //! is queued or reads more, so looping on it is what empties both the queue and the socket.
    void X11Application::pumpDisplay
        (
        short aEvents   //!< poll(2) revents for the connection.
        )
    {
        Display* const display = static_cast<Display*>( mDisplay );
        if( display == nullptr )
        {
            return;
        }

        if( ( aEvents & ( POLLERR | POLLHUP | POLLNVAL ) ) != 0 )
        {
            // The server went away. Reading further would hit Xlib's I/O error handler, which
            // calls exit() and would skip every destructor in the program.
            std::fprintf( stderr, "X11Application: the display connection dropped (revents 0x%x); "
                "quitting\n", static_cast<unsigned int>( aEvents ) );
            QtLikeSignal::CoreApplication::quit();
            return;
        }

        while( XPending( display ) > 0 )
        {
            XEvent event;
            XNextEvent( display, &event );
            dispatchNativeEvent( &event );
        }

        // The slots have just run and will have asked for drawing. Their requests are sitting in
        // Xlib's output buffer, and nothing else is about to flush it, so push them out before the
        // loop goes back to sleep -- otherwise the screen lags one event behind the program.
        XFlush( display );

        // Residual check, and cheap: XQLength() is the length of Xlib's own queue and touches no
        // descriptor. It can only be non-zero here if a slot put something there -- a round trip
        // such as XSync() reads events off the socket as a side effect of waiting for its reply.
        // Those events would be invisible to poll(), so take another pass through our own queue
        // rather than recursing here, which keeps this pass bounded and lets timers and posted work
        // interleave.
        if( XQLength( display ) > 0 )
        {
            QtLikeSignal::CoreApplication::post( [this]()
                {
                    pumpDisplay( POLLIN );
                } );
        }
    }

    //! Translates one native event into a signal emission.
    //!
    //! The whole platform vocabulary stops here: everything downstream sees MouseEvent, a KeySym,
    //! or a Drawable to paint on. That is the split the Win32 demo makes in its window procedure,
    //! and the split Qt makes at QWindowSystemInterface.
    void X11Application::dispatchNativeEvent
        (
        void* aEvent   //!< The XEvent just taken off the queue.
        )
    {
        const XEvent& event = *static_cast<const XEvent*>( aEvent );

        switch( event.type )
        {
        case Expose:
        {
            // The server sends one Expose per exposed rectangle and this demo redraws the whole
            // window regardless, so only the last of a burst is worth acting on. count is how
            // many are still to come.
            if( event.xexpose.count == 0 )
            {
                paintNow();
            }
            break;
        }

        case ConfigureNotify:
        {
            if( event.xconfigure.width != mWidth || event.xconfigure.height != mHeight )
            {
                resizeBackBuffer( event.xconfigure.width, event.xconfigure.height );
                paintNow();
            }
            break;
        }

        case MotionNotify:
        {
            MouseEvent moved;
            moved.mX      = event.xmotion.x;
            moved.mY      = event.xmotion.y;
            moved.mButton = MouseEvent::Button::None;
            mMouseMoved.emit( moved );
            break;
        }

        case ButtonPress:
        {
            MouseEvent pressed;
            pressed.mX      = event.xbutton.x;
            pressed.mY      = event.xbutton.y;
            pressed.mButton = buttonOf( event.xbutton.button );
            mMousePressed.emit( pressed );
            break;
        }

        case ButtonRelease:
        {
            MouseEvent released;
            released.mX      = event.xbutton.x;
            released.mY      = event.xbutton.y;
            released.mButton = buttonOf( event.xbutton.button );
            mMouseReleased.emit( released );
            break;
        }

        case KeyPress:
        {
            // Index 0 is the unshifted symbol, which is all this demo needs. A program taking
            // text would go through XLookupString or an input method instead.
            const KeySym key = XLookupKeysym( const_cast<XKeyEvent*>( &event.xkey ), 0 );
            mKeyPressed.emit( key );
            if( key == XK_Escape )
            {
                QtLikeSignal::CoreApplication::quit();
            }
            break;
        }

        case ClientMessage:
        {
            if( static_cast<Atom>( event.xclient.data.l[0] ) == mDeleteWindowAtom )
            {
                // quit(), and for the same reason the Win32 demo calls quit() from WM_CLOSE
                // rather than PostQuitMessage(): Thread::exec() loops on its own exit flag and
                // never reads what processEvents() returned, so nothing a platform event can
                // return will stop it. An application built on this library ends its loop
                // through quit(). That divergence from Qt is R22 in
                // history/OPEN-RISKS-20260816.md.
                QtLikeSignal::CoreApplication::quit();
            }
            break;
        }

        default:
            break;
        }
    }

    //! Asks for a repaint, coalescing every request made before the next pass into one.
    //!
    //! The repaint is posted to *our own* event queue, not to the server: the Win32 demo's
    //! InvalidateRect() hands the request to the OS and gets a WM_PAINT back, and X11's equivalent
    //! would be a round trip to ask the server to send us an Expose. There is no need to leave the
    //! process for something the process already knows, and going through CoreApplication::post()
    //! shows the other half of this demo -- a repaint driven by the library's own event queue,
    //! interleaved by the same loop with the events coming off the X socket.
    //!
    //! The posted task holds `this`, so the application has to outlive its loop. It does: exec()
    //! returns before ~X11Application() runs in main().
    void X11Application::update()
    {
        if( mDisplay == nullptr )
        {
            return;
        }

        // One repaint per pass. A drag delivers a MotionNotify every few milliseconds and the timer
        // adds its own, so without this the queue would fill with repaints of the same pixels.
        if( mRepaintPosted.exchange( true ) )
        {
            return;
        }

        QtLikeSignal::CoreApplication::post( [this]()
            {
                mRepaintPosted.store( false );
                paintNow();
            } );
    }

    //! Emits the paint signal against the back buffer and copies the result to the window.
    void X11Application::paintNow()
    {
        Display* const display = static_cast<Display*>( mDisplay );
        if( display == nullptr || mBackBuffer == 0 || mGraphicsContext == nullptr )
        {
            return;
        }

        mPaintRequested.emit( mBackBuffer );

        XCopyArea( display, mBackBuffer, mWindow, static_cast<GC>( mGraphicsContext ),
            0, 0, static_cast<unsigned int>( mWidth ), static_cast<unsigned int>( mHeight ),
            0, 0 );
        XFlush( display );
    }

    //! Replaces the back buffer with one matching the window's current size.
    void X11Application::resizeBackBuffer
        (
        int aWidth,   //!< New width in pixels.
        int aHeight   //!< New height in pixels.
        )
    {
        Display* const display = static_cast<Display*>( mDisplay );
        if( display == nullptr || mWindow == 0 )
        {
            return;
        }

        // A pixmap of either dimension zero is a BadValue, and a window manager will hand out a
        // zero-sized configure while a window is being unmapped.
        mWidth  = aWidth > 0 ? aWidth : 1;
        mHeight = aHeight > 0 ? aHeight : 1;

        if( mBackBuffer != 0 )
        {
            XFreePixmap( display, mBackBuffer );
            mBackBuffer = 0;
        }

        mBackBuffer = XCreatePixmap( display, mWindow, static_cast<unsigned int>( mWidth ),
            static_cast<unsigned int>( mHeight ),
            static_cast<unsigned int>( DefaultDepth( display, DefaultScreen( display ) ) ) );
    }

    //! Gets the display connection as a void*, for a slot that needs to call Xlib on it.
    void* X11Application::nativeDisplay() const
    {
        return mDisplay;
    }

    //! Gets the window's resource id.
    unsigned long X11Application::nativeWindow() const
    {
        return mWindow;
    }

    //! Gets the graphics context the paint slot should draw with, as a void*.
    void* X11Application::nativeGraphicsContext() const
    {
        return mGraphicsContext;
    }

    //! Gets the mouse-press signal.
    QtLikeSignal::SignalView<MouseEvent>& X11Application::getMousePressed() const
    {
        return mMousePressed.view();
    }

    //! Gets the mouse-release signal.
    QtLikeSignal::SignalView<MouseEvent>& X11Application::getMouseReleased() const
    {
        return mMouseReleased.view();
    }

    //! Gets the mouse-move signal.
    QtLikeSignal::SignalView<MouseEvent>& X11Application::getMouseMoved() const
    {
        return mMouseMoved.view();
    }

    //! Gets the key-press signal, which carries the KeySym.
    QtLikeSignal::SignalView<unsigned long>& X11Application::getKeyPressed() const
    {
        return mKeyPressed.view();
    }

    //! Gets the paint signal, which carries the Drawable to draw into.
    QtLikeSignal::SignalView<unsigned long>& X11Application::getPaintRequested() const
    {
        return mPaintRequested.view();
    }
}
