// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! A real X11 program on QtLikeSignalGui, in both of the shapes the library supports on Linux.
//!
//! By default QtLikeSignalGui creates the window and the program draws on it. With **--adopt** an
//! external library creates it instead -- that is the block marked "stand-in for the external
//! library", which opens a Display and a window and would in a real program also set up GLX or EGL
//! -- and QtLikeSignalGui takes over the listening. Everything after that point is identical in
//! both shapes: connect signals, draw on expose. **Neither half knows about the other**, which is
//! the seam this demo exists to show.
//!
//! The only visible difference is cleanup. A created window is destroyed with its Window; an
//! adopted one is left standing for whoever made it.
//!
//! There is no event loop in this program and no XNextEvent call. exec() is the only loop, and
//! PlatformIntegrationX11 registers the display's socket with EventDispatcherLinux so that one
//! poll() waits on the X connection, the dispatcher's eventfd and the timer deadline together.
//! When nothing is happening the process uses no CPU at all -- check it in top while the window
//! sits idle, then move the mouse.
//!
//! Controls: move, click and scroll anywhere in the window; the close box quits, and so does
//! Ctrl+C -- a SignalWatcher turns it into a signal on the loop, so the teardown runs.

#include "QtLikeSignalGui/GuiApplication.hpp"
#include "QtLikeSignalGui/Window.hpp"

#include "QtLikeSignal/CoreApplication.hpp"
#include "QtLikeSignal/Object.hpp"
#include "QtLikeSignal/SignalWatcher.hpp"
#include "QtLikeSignal/Timer.hpp"

#include "QtLikeSignal/Log.hpp"

#include <cstdio>
#include <deque>
#include <iomanip>
#include <sstream>
#include <string>

#include <X11/Xlib.h>

//! This demo's category.
QTLIKESIGNAL_DEFINE_LOG_CATEGORY( gLogDemo, "demo.gui.x11", "DGX1" )

// Xlib's None macro collides with MouseButton::None, and its Window typedef collides with
// QtLikeSignalGui::Window. The QtLikeSignalGui headers are included above, before either exists, so
// the declarations are safe; undefining the macro makes the enumerator usable again below, and the
// namespace alias is what keeps the two Windows apart without a using-directive that would make
// every mention of the name ambiguous.
#undef None

namespace Gui = QtLikeSignalGui;

namespace
{
    //! How many recent events the on-screen log keeps.
    const int kLogLines = 16;

    //! Draws the window and records what it is told about.
    //!
    //! An ordinary QtLikeSignal::Object. It was handed a Display and a Window and it draws on them,
    //! which is the position an external rendering library is in -- it never sees an XEvent.
    class Renderer : public QtLikeSignal::Object
    {
    public:
        //! Constructs the renderer against the window it draws in.
        Renderer
            (
            Gui::Window* aWindow,   //!< The adopted window. Not owned.
            Display* aDisplay,      //!< The connection it lives on. Not owned.
            ::Window aDrawable,     //!< Its resource id.
            GC aGraphicsContext     //!< A context to draw with. Not owned.
            )
            : mWindow( aWindow )
            , mDisplay( aDisplay )
            , mDrawable( aDrawable )
            , mGraphicsContext( aGraphicsContext )
        {
            mBackground = allocateColour( 24, 26, 32 );
            mText       = allocateColour( 235, 235, 240 );
            mAccent     = allocateColour( 120, 200, 255 );
            mDim        = allocateColour( 150, 155, 170 );
            mGood       = allocateColour( 160, 230, 160 );

            resizeBackBuffer( aWindow->width(), aWindow->height() );
        }

        //! Frees the back buffer.
        ~Renderer() override
        {
            if( mBackBuffer != 0 )
            {
                XFreePixmap( mDisplay, mBackBuffer );
            }
        }

        //! Records a mouse press.
        void onMousePressed
            (
            Gui::MouseEvent aEvent   //!< The press.
            )
        {
            ++mPressCount;
            append( "press  ", aEvent );
        }

        //! Records a mouse release.
        void onMouseReleased
            (
            Gui::MouseEvent aEvent   //!< The release.
            )
        {
            ++mReleaseCount;
            append( "release", aEvent );
        }

        //! Records a mouse move, logging only every few pixels but tracking every one.
        void onMouseMoved
            (
            Gui::MouseEvent aEvent   //!< The move.
            )
        {
            ++mMoveCount;
            mLast = aEvent;

            const int dx = aEvent.mPos.mX - mLastLoggedX;
            const int dy = aEvent.mPos.mY - mLastLoggedY;
            if( ( dx * dx + dy * dy ) >= ( 60 * 60 ) )
            {
                mLastLoggedX = aEvent.mPos.mX;
                mLastLoggedY = aEvent.mPos.mY;
                append( "move   ", aEvent );
            }
            else
            {
                mWindow->requestUpdate();
            }
        }

        //! Records the pointer entering the window.
        void onMouseEntered
            (
            Gui::MouseEvent aEvent   //!< Where it entered.
            )
        {
            mInside = true;
            append( "enter  ", aEvent );
        }

        //! Records the pointer leaving the window.
        void onMouseLeft()
        {
            mInside = false;
            push( "  leave" );
        }

        //! Records a wheel turn.
        void onWheel
            (
            Gui::WheelEvent aEvent   //!< The rotation.
            )
        {
            mWheelTotal += aEvent.mAngleDeltaY;

            std::ostringstream entry;
            entry << "  wheel    " << std::setw( 6 ) << aEvent.mAngleDeltaY
                  << "  at " << std::setw( 4 ) << aEvent.mPos.mX
                  << ", " << std::setw( 4 ) << aEvent.mPos.mY;
            push( entry.str() );
        }

        //! Rebuilds the back buffer at the new size. A GL renderer would resize its viewport here.
        void onResized
            (
            int aWidth,   //!< New width.
            int aHeight   //!< New height.
            )
        {
            resizeBackBuffer( aWidth, aHeight );

            std::ostringstream entry;
            entry << "  resize   " << aWidth << " x " << aHeight;
            push( entry.str() );
        }

        //! Counts one second of uptime and repaints.
        //!
        //! Driven by a QtLikeSignal::Timer sharing the loop with the X connection. It keeps ticking
        //! while the mouse floods the socket, which is the point: one poll() is servicing both.
        void onSecond()
        {
            ++mSeconds;
            mWindow->requestUpdate();
        }

        //! Paints the whole window from the back buffer.
        void onExposed()
        {
            const int width  = mWindow->width();
            const int height = mWindow->height();
            if( mBackBuffer == 0 || width <= 0 || height <= 0 )
            {
                return;
            }

            XSetForeground( mDisplay, mGraphicsContext, mBackground );
            XFillRectangle( mDisplay, mBackBuffer, mGraphicsContext, 0, 0,
                static_cast<unsigned int>( width ), static_cast<unsigned int>( height ) );

            int y = 24;
            const int lineHeight = 18;

            auto line = [&]( unsigned long aColour, const std::string& aText )
                {
                    XSetForeground( mDisplay, mGraphicsContext, aColour );
                    XDrawString( mDisplay, mBackBuffer, mGraphicsContext, 16, y, aText.c_str(),
                        static_cast<int>( aText.size() ) );
                    y += lineHeight;
                };

            line( mAccent,
                "QtLikeSignalGui on X11 -- window adopted, connection polled by the loop" );
            line( mDim, "No XNextEvent loop in this program. GuiApplication::exec() is the only "
                "loop." );
            line( mDim, "Move, click and scroll.  The close box quits." );
            y += lineHeight;

            {
                std::ostringstream text;
                text << "window size   : " << width << " x " << height;
                line( mText, text.str() );
            }
            {
                std::ostringstream text;
                text << "cursor        : " << std::setw( 4 ) << mLast.mPos.mX << ", "
                     << std::setw( 4 ) << mLast.mPos.mY
                     << "   (root " << mLast.mGlobalPos.mX << ", " << mLast.mGlobalPos.mY << ")";
                line( mText, text.str() );
            }
            line( mText, "buttons held  : " + describeButtons() );
            line( mText, std::string( "pointer       : " )
                + ( mInside ? "inside" : "outside" ) );
            {
                std::ostringstream text;
                text << "presses       : " << mPressCount << "    releases : " << mReleaseCount;
                line( mText, text.str() );
            }
            {
                std::ostringstream text;
                text << "moves         : " << mMoveCount << "    wheel total : " << mWheelTotal;
                line( mText, text.str() );
            }
            {
                std::ostringstream text;
                text << "timer uptime  : " << mSeconds
                     << " s   (QtLikeSignal::Timer, sharing one poll() with the X socket)";
                line( mGood, text.str() );
            }
            y += lineHeight;

            line( mDim, "recent events" );
            for( const std::string& entry : mLog )
            {
                line( mText, entry );
            }

            // A crosshair at the last known pointer position, so the coordinates are visibly the
            // ones the server reported rather than something invented.
            XSetForeground( mDisplay, mGraphicsContext, mAccent );
            XDrawLine( mDisplay, mBackBuffer, mGraphicsContext, mLast.mPos.mX - 12, mLast.mPos.mY,
                mLast.mPos.mX + 12, mLast.mPos.mY );
            XDrawLine( mDisplay, mBackBuffer, mGraphicsContext, mLast.mPos.mX, mLast.mPos.mY - 12,
                mLast.mPos.mX, mLast.mPos.mY + 12 );

            XCopyArea( mDisplay, mBackBuffer, mDrawable, mGraphicsContext, 0, 0,
                static_cast<unsigned int>( width ), static_cast<unsigned int>( height ), 0, 0 );
        }

    private:
        //! Allocates a colour from the default colormap, falling back to white.
        unsigned long allocateColour
            (
            unsigned short aRed,     //!< 0-255.
            unsigned short aGreen,   //!< 0-255.
            unsigned short aBlue     //!< 0-255.
            )
        {
            const int screen = DefaultScreen( mDisplay );
            Colormap colormap = DefaultColormap( mDisplay, screen );

            // X wants 16-bit components; the shift is the standard widening, not a scale factor.
            XColor colour {};
            colour.red   = static_cast<unsigned short>( aRed << 8 );
            colour.green = static_cast<unsigned short>( aGreen << 8 );
            colour.blue  = static_cast<unsigned short>( aBlue << 8 );
            colour.flags = DoRed | DoGreen | DoBlue;

            if( XAllocColor( mDisplay, colormap, &colour ) == 0 )
            {
                return WhitePixel( mDisplay, screen );
            }

            return colour.pixel;
        }

        //! Rebuilds the off-screen pixmap at the given size.
        //!
        //! Without a back buffer the window flickers under a stream of motion: every repaint would
        //! clear the window and then draw over it, and the cleared state is visible.
        void resizeBackBuffer
            (
            int aWidth,   //!< New width in pixels.
            int aHeight   //!< New height in pixels.
            )
        {
            if( aWidth <= 0 || aHeight <= 0 )
            {
                return;
            }

            if( mBackBuffer != 0 )
            {
                XFreePixmap( mDisplay, mBackBuffer );
                mBackBuffer = 0;
            }

            const int screen = DefaultScreen( mDisplay );
            mBackBuffer = XCreatePixmap( mDisplay, mDrawable,
                static_cast<unsigned int>( aWidth ), static_cast<unsigned int>( aHeight ),
                static_cast<unsigned int>( DefaultDepth( mDisplay, screen ) ) );
        }

        //! Renders the held-button set as text.
        std::string describeButtons() const
        {
            const Gui::MouseButtons held = Gui::GuiApplication::mouseButtons();
            if( !held.any() )
            {
                return "none";
            }

            std::string text;
            if( held.test( Gui::MouseButton::Left ) )
            {
                text += "left ";
            }
            if( held.test( Gui::MouseButton::Middle ) )
            {
                text += "middle ";
            }
            if( held.test( Gui::MouseButton::Right ) )
            {
                text += "right ";
            }
            return text;
        }

        //! Adds one line to the log and repaints.
        void push
            (
            const std::string& aLine   //!< The line.
            )
        {
            mLog.push_back( aLine );
            while( static_cast<int>( mLog.size() ) > kLogLines )
            {
                mLog.pop_front();
            }
            mWindow->requestUpdate();
        }

        //! Adds one line describing a mouse event, and repaints.
        void append
            (
            const char* aKind,             //!< Event kind, already padded to a fixed width.
            const Gui::MouseEvent& aEvent   //!< The event to describe.
            )
        {
            const char* button = "none  ";
            switch( aEvent.mButton )
            {
            case Gui::MouseButton::Left:   button = "left  "; break;
            case Gui::MouseButton::Middle: button = "middle"; break;
            case Gui::MouseButton::Right:  button = "right "; break;
            case Gui::MouseButton::Extra1: button = "extra1"; break;
            case Gui::MouseButton::Extra2: button = "extra2"; break;
            case Gui::MouseButton::None:   break;
            }

            std::ostringstream entry;
            entry << "  " << aKind << "  " << button << "  at "
                  << std::setw( 4 ) << aEvent.mPos.mX << ", "
                  << std::setw( 4 ) << aEvent.mPos.mY;
            push( entry.str() );
        }

        Gui::Window* mWindow;               //!< The adopted window. Not owned.
        Display* mDisplay;                  //!< The connection. Not owned.
        ::Window mDrawable;                 //!< The window's resource id.
        GC mGraphicsContext;                //!< Drawing context. Not owned.
        Pixmap mBackBuffer { 0 };           //!< Off-screen buffer, blitted on every frame.
        std::deque<std::string> mLog;       //!< Most recent event descriptions.
        Gui::MouseEvent mLast;              //!< Most recent pointer position, for the crosshair.
        unsigned long mBackground { 0 };    //!< Background pixel.
        unsigned long mText { 0 };          //!< Body text pixel.
        unsigned long mAccent { 0 };        //!< Heading and crosshair pixel.
        unsigned long mDim { 0 };           //!< Secondary text pixel.
        unsigned long mGood { 0 };          //!< Timer line pixel.
        bool mInside { false };             //!< Whether the pointer is over the window.
        int mPressCount { 0 };              //!< Total presses seen.
        int mReleaseCount { 0 };            //!< Total releases seen.
        int mMoveCount { 0 };               //!< Total moves seen.
        int mWheelTotal { 0 };              //!< Accumulated vertical wheel rotation.
        int mSeconds { 0 };                 //!< Seconds since start, from the timer.
        int mLastLoggedX { 0 };             //!< Pointer X when a move was last logged.
        int mLastLoggedY { 0 };             //!< Pointer Y when a move was last logged.
    };
}

//! Creates a window the way an external library would, adopts it, and runs the one loop there is.
int main
    (
    int argc,     //!< Argument count.
    char** argv   //!< Argument vector; -p x11 selects this backend.
    )
{
    Gui::GuiApplication app( argc, argv );

    if( !app.hasPlatform() || app.platformType() != Gui::PlatformType::X11 )
    {
        qCCritical( gLogDemo )
            << "this demo needs the x11 backend; pass -p x11. The platform is"
            << app.platformName();
        return 1;
    }

    // Both ways in, so one program shows both. --adopt is the shape a GLX program that lets its
    // rendering library create the window ends up with; the default is the shape one that lets
    // QtLikeSignalGui create it ends up with, and the only visible difference is who cleans up.
    bool adopting = false;
    for( int i = 1; i < argc; ++i )
    {
        if( std::string( argv[i] ) == "--adopt" )
        {
            adopting = true;
        }
    }

    Display* display = nullptr;
    ::Window drawable = 0;
    Gui::Window* window = nullptr;

    if( adopting )
    {
        // -----------------------------------------------------------------------------------
        // Stand-in for the external library: it opens the connection, creates the window, and
        // sets up whatever it needs to draw -- a GC here, a GLX or EGL context in a real
        // program. QtLikeSignalGui knows nothing about any of it until handed the two ids below.
        // -----------------------------------------------------------------------------------
        display = XOpenDisplay( nullptr );
        if( display == nullptr )
        {
            qCWarning( gLogDemo ) << "XOpenDisplay() failed; is DISPLAY set?";
            return 1;
        }

        const int screen = DefaultScreen( display );
        drawable = XCreateSimpleWindow( display, RootWindow( display, screen ),
            0, 0, 1280, 720, 0, BlackPixel( display, screen ), BlackPixel( display, screen ) );
        // ---------------------------- end of the stand-in --------------------------------

        Gui::NativeWindow native;
        native.mDisplay  = display;
        native.mWindowId = drawable;

        window = app.adoptWindow( native );
    }
    else
    {
        // The connection first, because a GL program needs one before there is a window: it is
        // what glXChooseFBConfig is called on, and the visual that config implies is what the
        // window then has to be created with. This demo has no config to choose, so it leaves
        // WindowSettings::mVisualId at zero and takes the screen's default visual.
        display = static_cast<Display*>( app.nativeDisplay() );
        if( display == nullptr )
        {
            qCWarning( gLogDemo ) << "no X11 connection";
            return 1;
        }

        Gui::WindowSettings settings;
        settings.mWidth  = 1280;
        settings.mHeight = 720;
        settings.mTitle  = "QtLikeSignalGui demo -- X11 created window";

        window = app.createWindow( settings );
        if( window != nullptr )
        {
            drawable = static_cast< ::Window >( window->nativeWindowId() );
        }
    }

    if( window == nullptr )
    {
        qCCritical( gLogDemo )
            << "failed to" << ( adopting ? "adopt" : "create" ) << "the window";
        return 1;
    }

    const GC graphicsContext = XCreateGC( display, drawable, 0, nullptr );

    if( adopting )
    {
        window->setTitle( "QtLikeSignalGui demo -- X11 adopted window" );
    }

    Renderer renderer( window, display, drawable, graphicsContext );

    // Direct connections: the drain runs on this thread, inside this thread's dispatch pass, so the
    // slots run there too. Auto would resolve to Direct for the same reason.
    QtLikeSignal::Object::connect( window->getMousePressed(), &renderer, &Renderer::onMousePressed,
        QtLikeSignal::ConnectionType::Direct );
    QtLikeSignal::Object::connect( window->getMouseReleased(), &renderer,
        &Renderer::onMouseReleased, QtLikeSignal::ConnectionType::Direct );
    QtLikeSignal::Object::connect( window->getMouseMoved(), &renderer, &Renderer::onMouseMoved,
        QtLikeSignal::ConnectionType::Direct );
    QtLikeSignal::Object::connect( window->getMouseEntered(), &renderer, &Renderer::onMouseEntered,
        QtLikeSignal::ConnectionType::Direct );
    QtLikeSignal::Object::connect( window->getMouseLeft(), &renderer, &Renderer::onMouseLeft,
        QtLikeSignal::ConnectionType::Direct );
    QtLikeSignal::Object::connect( window->getWheel(), &renderer, &Renderer::onWheel,
        QtLikeSignal::ConnectionType::Direct );
    QtLikeSignal::Object::connect( window->getResized(), &renderer, &Renderer::onResized,
        QtLikeSignal::ConnectionType::Direct );
    QtLikeSignal::Object::connect( window->getExposed(), &renderer, &Renderer::onExposed,
        QtLikeSignal::ConnectionType::Direct );

    QtLikeSignal::Timer second;
    QtLikeSignal::Object::connect( second.getTimeout(), &renderer, &Renderer::onSecond,
        QtLikeSignal::ConnectionType::Direct );
    second.start( 1000 );

    window->show();

    // Ctrl+C, and the SIGTERM a service manager sends, come back here as an ordinary signal on this
    // thread instead of stopping the process where it stands. That is what lets the teardown below
    // run at all, so this demo shows the whole shutdown path and not only the close button. Press
    // Ctrl+C two times and the second one stops the process, whatever the first one is still doing.
    QtLikeSignal::SignalWatcher shutdown;
    QtLikeSignal::Object::connect( shutdown.getTriggered(), &renderer, []( int aSignal )
        {
            std::printf( "signal %d received; leaving the loop\n", aSignal );
            QtLikeSignal::CoreApplication::quit();
        }, QtLikeSignal::ConnectionType::Direct );

    std::printf( "QtLikeSignalGui X11 demo running.\n" );
    std::printf( "  platform        : %s\n", app.platformName() );
    std::printf( "  window          : 0x%lx on fd %d\n", window->nativeWindowId(),
        XConnectionNumber( display ) );
    std::printf( "  event loop      : GuiApplication::exec()\n" );
    std::printf( "  X socket        : in the dispatcher's poll() set\n" );
    std::printf( "  device pixels   : %.2f per unit, so %d x %d real pixels\n",
        window->devicePixelRatio(),
        static_cast<int>( window->width() * window->devicePixelRatio() ),
        static_cast<int>( window->height() * window->devicePixelRatio() ) );
    std::printf( "  shutdown        : %s\n",
        shutdown.isWatching() ? "SIGINT and SIGTERM watched" : "not watched" );
    std::printf( "  no XNextEvent loop in this program\n" );

    // Flushed, because the loop below can run for hours and stdout is block-buffered the
    // moment it is redirected to a file -- so without this the status above appears only
    // when the program exits, which is exactly when it stops being useful.
    std::fflush( stdout );

    const int result = app.exec();

    std::printf( "loop finished, exit code %d\n", result );

    // Ordered so nothing outlives the connection it needs. Deleting the Window is what releases the
    // native side either way -- it destroys a created window, and merely stops listening to an
    // adopted one, which is why only the adopting path has anything left to clean up afterwards.
    delete window;
    XFreeGC( display, graphicsContext );

    if( adopting )
    {
        XDestroyWindow( display, drawable );
        XCloseDisplay( display );
    }

    return result;
}
