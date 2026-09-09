// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Tests for the QtLikeSignalGui X11 backend: adopting a window an external library created, and
//! the promise that releasing it leaves that window standing.
//!
//! A file of its own rather than more cases in QtLikeSignal-test-gui.cpp, and for a reason that is
//! not organisational. Xlib typedefs `Window` at global scope and QtLikeSignalGui declares a class
//! of the same name, so a translation unit with `using namespace QtLikeSignalGui;` in it cannot
//! include <X11/Xlib.h> at all -- every mention of the name inside Xlib's own header becomes
//! ambiguous. Nothing here is unqualified, which is what makes both names usable side by side.

#include <gtest/gtest.h>

// The whole file, because the X11 headers it needs exist only where the backend was built. HAVE_X11
// comes from check_cfg() at configure time and is the same condition the backend itself is compiled
// under, so these tests cannot be built against a library that does not have it.
#if defined( HAVE_X11 )


    #include "QtLikeSignalGui/GuiApplication.hpp"
    #include "QtLikeSignalGui/Window.hpp"

    #include "QtLikeSignalGui-test-support.hpp"

    #include <chrono>
    #include <thread>

    #include <X11/Xlib.h>

    // Xlib's None is a bare `0L` macro and MouseButton has a None enumerator. The QtLikeSignalGui
    // headers are included above, before the macro exists, so the enumerator is declared safely;
    // undefining it here keeps the rest of this file able to name it.
    #undef None

    namespace
    {
        //! Opens a display and creates a window, standing in for the external library.
        //!
        //! Everything the X11 backend adopts is made this way in a real program: by somebody else,
        //! who keeps owning it. Destroying it is this helper's job for the same reason.
        //!
        //! **Declare one of these before the GuiApplication that adopts from it, always.**
        //! Destruction runs in reverse, so declaring it first is what makes it outlive the
        //! application -- and the application's destructor releases its windows, which talks to
        //! this connection. The other way round the display is closed first and the release
        //! dereferences it, which is a segfault rather than a diagnosable error. Real code has the
        //! same obligation, which is why the demo destroys its Window before calling XCloseDisplay.
        class ForeignX11Window
        {
        public:
            //! Creates the window, or leaves display() null if there is no X server to talk to.
            ForeignX11Window
                (
                int aWidth, //!< Width in pixels.
                int aHeight //!< Height in pixels.
                )
            {
                mDisplay = XOpenDisplay( nullptr );
                if( mDisplay == nullptr )
                {
                    return;
                }

                const int screen = DefaultScreen( mDisplay );
                mWindow = XCreateSimpleWindow( mDisplay, RootWindow( mDisplay, screen ), 0, 0,
                    static_cast<unsigned int>( aWidth ), static_cast<unsigned int>( aHeight ), 0,
                    BlackPixel( mDisplay, screen ), BlackPixel( mDisplay, screen ) );
                XFlush( mDisplay );
            }

            //! Destroys the window and closes the connection.
            ~ForeignX11Window()
            {
                if( mDisplay != nullptr )
                {
                    if( mWindow != 0 )
                    {
                        XDestroyWindow( mDisplay, mWindow );
                    }
                    XCloseDisplay( mDisplay );
                }
            }

            //! Gets the connection, or nullptr if none could be opened.
            Display* display() const
            {
                return mDisplay;
            }

            //! Gets the window id.
            ::Window window() const
            {
                return mWindow;
            }

            //! Builds the descriptor the library adopts.
            QtLikeSignalGui::NativeWindow native() const
            {
                QtLikeSignalGui::NativeWindow native;
                native.mDisplay  = mDisplay;
                native.mWindowId = mWindow;
                return native;
            }

        private:
            Display* mDisplay { nullptr }; //!< The connection.
            ::Window mWindow { 0 };      //!< The window.
        };
    }

    //! A created window has the requested size, a resource id, and the connection behind it.
    TEST( GuiX11Test, CreatedWindowHasTheRequestedSize )
    {
        QtLikeSignalGuiTest::FakeCommandLine commandLine( { "-p", "x11" } );
        QtLikeSignalGui::GuiApplication application( commandLine.argc(), commandLine.argv() );

        if( !application.hasPlatform() || application.nativeDisplay() == nullptr )
        {
            GTEST_SKIP() << "no X server available (DISPLAY unset?)";
        }

        QtLikeSignalGui::WindowSettings settings;
        settings.mWidth  = 512;
        settings.mHeight = 384;
        settings.mTitle  = "created";

        QtLikeSignalGui::Window* const window = application.createWindow( settings );
        ASSERT_NE( nullptr, window );

        EXPECT_EQ( 512, window->width() );
        EXPECT_EQ( 384, window->height() );
        EXPECT_NE( 0u, window->nativeWindowId() );

        // The same connection the caller was handed, which is what makes it usable for a GL library
        // that chose its config on it before the window existed.
        EXPECT_EQ( application.nativeDisplay(), window->nativeDisplay() );

        // Created unmapped, so everything that has to happen before the first frame can happen
        // first.
        EXPECT_FALSE( window->isVisible() );

        // Checked against the server rather than against the library's own bookkeeping.
        Display* const display = static_cast<Display*>( application.nativeDisplay() );
        XWindowAttributes attributes {};
        ASSERT_NE( 0, XGetWindowAttributes( display,
            static_cast< ::Window >( window->nativeWindowId() ), &attributes ) );
        EXPECT_EQ( 512, attributes.width );
        EXPECT_EQ( 384, attributes.height );
    }

    //! A created window really is destroyed with its Window, unlike an adopted one.
    //!
    //! Verified from a *separate* connection, because the application's own goes away with it --
    //! and a resource id outlives the connection that made it only in the sense that the server can
    //! be asked about it, which is exactly what this asks.
    TEST( GuiX11Test, DestroyingACreatedWindowDestroysIt )
    {
        unsigned long windowId = 0;

        {
            QtLikeSignalGuiTest::FakeCommandLine commandLine( { "-p", "x11" } );
            QtLikeSignalGui::GuiApplication application( commandLine.argc(), commandLine.argv() );

            if( !application.hasPlatform() || application.nativeDisplay() == nullptr )
            {
                GTEST_SKIP() << "no X server available (DISPLAY unset?)";
            }

            QtLikeSignalGui::Window* const window = application.createWindow(
                QtLikeSignalGui::WindowSettings() );
            ASSERT_NE( nullptr, window );

            windowId = window->nativeWindowId();
            delete window;
        }

        Display* const verifier = XOpenDisplay( nullptr );
        ASSERT_NE( nullptr, verifier );

        // Silenced for the duration: asking about a destroyed window is a BadWindow, and Xlib's
        // default handler prints it. That is the answer this test wants, not a fault to be
        // reported.
        XErrorHandler previous = XSetErrorHandler(
            []( Display*, XErrorEvent* ) -> int
            {
                return 0;
            } );

        XWindowAttributes attributes {};
        const int found = XGetWindowAttributes( verifier, static_cast< ::Window >( windowId ),
            &attributes );

        XSetErrorHandler( previous );
        XCloseDisplay( verifier );

        EXPECT_EQ( 0, found );
    }

    //! A visual id no screen has is refused, rather than quietly falling back to the default.
    //!
    //! Falling back would be the worst outcome available: the window would be created with a visual
    //! the caller's GL config does not match, and the failure would surface later as a BadMatch
    //! from glXCreateWindow with nothing to connect it to.
    TEST( GuiX11Test, AnUnknownVisualIsRefused )
    {
        QtLikeSignalGuiTest::FakeCommandLine commandLine( { "-p", "x11" } );
        QtLikeSignalGui::GuiApplication application( commandLine.argc(), commandLine.argv() );

        if( !application.hasPlatform() || application.nativeDisplay() == nullptr )
        {
            GTEST_SKIP() << "no X server available (DISPLAY unset?)";
        }

        QtLikeSignalGui::WindowSettings settings;
        settings.mVisualId = 0x7fffffff;

        EXPECT_EQ( nullptr, application.createWindow( settings ) );
    }

    //! The screen's default visual is accepted by id, which is the ordinary GL path spelled out.
    TEST( GuiX11Test, AWindowCanBeCreatedWithAnExplicitVisual )
    {
        QtLikeSignalGuiTest::FakeCommandLine commandLine( { "-p", "x11" } );
        QtLikeSignalGui::GuiApplication application( commandLine.argc(), commandLine.argv() );

        if( !application.hasPlatform() || application.nativeDisplay() == nullptr )
        {
            GTEST_SKIP() << "no X server available (DISPLAY unset?)";
        }

        // Stands in for the visual a GL library would have got from its chosen FBConfig: the point
        // is that an id obtained from the connection round-trips into a window created with it.
        Display* const display = static_cast<Display*>( application.nativeDisplay() );
        const int screen       = DefaultScreen( display );

        QtLikeSignalGui::WindowSettings settings;
        settings.mVisualId = XVisualIDFromVisual( DefaultVisual( display, screen ) );

        QtLikeSignalGui::Window* const window = application.createWindow( settings );
        ASSERT_NE( nullptr, window );

        XWindowAttributes attributes {};
        ASSERT_NE( 0, XGetWindowAttributes( display,
            static_cast< ::Window >( window->nativeWindowId() ), &attributes ) );
        EXPECT_EQ( settings.mVisualId, XVisualIDFromVisual( attributes.visual ) );
    }

    //! An adopted window reports the size the server says it has.
    TEST( GuiX11Test, AdoptedWindowTakesItsSizeFromTheServer )
    {
        // Declared before the application, so it is destroyed after it. See the note on
        // ForeignX11Window.
        ForeignX11Window foreign( 640, 480 );
        if( foreign.display() == nullptr )
        {
            GTEST_SKIP() << "XOpenDisplay() failed";
        }

        QtLikeSignalGuiTest::FakeCommandLine commandLine( { "-p", "x11" } );
        QtLikeSignalGui::GuiApplication application( commandLine.argc(), commandLine.argv() );

        if( !application.hasPlatform() )
        {
            GTEST_SKIP() << "no X server available (DISPLAY unset?)";
        }
        ASSERT_EQ( QtLikeSignalGui::PlatformType::X11, application.platformType() );

        QtLikeSignalGui::Window* const window = application.adoptWindow( foreign.native() );
        ASSERT_NE( nullptr, window );

        // Queried, not taken on trust from the caller: NativeWindow carries a size for the backends
        // that cannot ask, and X11 can.
        EXPECT_EQ( 640, window->width() );
        EXPECT_EQ( 480, window->height() );

        // A window is an integer on X11, not a pointer, which is why the descriptor has both
        // fields.
        EXPECT_EQ( foreign.window(), window->nativeWindowId() );
        EXPECT_EQ( foreign.display(), window->nativeDisplay() );
        EXPECT_EQ( nullptr, window->nativeHandle() );
    }

    //! Destroying an adopted Window leaves the native window standing.
    //!
    //! The whole contract of an adopting backend. The external library still owns the window and
    //! very likely still has a GLX context bound to it, so destroying it here would break code this
    //! library does not own -- and XGetWindowAttributes succeeding afterwards is what proves it did
    //! not.
    TEST( GuiX11Test, ReleasingAnAdoptedWindowDoesNotDestroyIt )
    {
        ForeignX11Window foreign( 320, 200 );
        if( foreign.display() == nullptr )
        {
            GTEST_SKIP() << "XOpenDisplay() failed";
        }

        QtLikeSignalGuiTest::FakeCommandLine commandLine( { "-p", "x11" } );
        QtLikeSignalGui::GuiApplication application( commandLine.argc(), commandLine.argv() );

        if( !application.hasPlatform() )
        {
            GTEST_SKIP() << "no X server available (DISPLAY unset?)";
        }

        QtLikeSignalGui::Window* const window = application.adoptWindow( foreign.native() );
        ASSERT_NE( nullptr, window );

        delete window;

        XWindowAttributes attributes {};
        const int found = XGetWindowAttributes( foreign.display(), foreign.window(), &attributes );

        EXPECT_NE( 0, found );
        EXPECT_EQ( 320, attributes.width );
        EXPECT_EQ( 200, attributes.height );
    }

    //! Adopting a window from a second connection is refused rather than silently going deaf.
    //!
    //! Only one descriptor is registered with the loop, so a window on another connection would
    //! never be polled -- and a window that receives nothing looks exactly like one that is simply
    //! not being used, which is the failure worth turning into a message.
    TEST( GuiX11Test, AdoptingFromASecondDisplayIsRefused )
    {
        ForeignX11Window first( 200, 100 );
        ForeignX11Window second( 200, 100 );
        if( first.display() == nullptr || second.display() == nullptr )
        {
            GTEST_SKIP() << "XOpenDisplay() failed";
        }

        QtLikeSignalGuiTest::FakeCommandLine commandLine( { "-p", "x11" } );
        QtLikeSignalGui::GuiApplication application( commandLine.argc(), commandLine.argv() );

        if( !application.hasPlatform() )
        {
            GTEST_SKIP() << "no X server available (DISPLAY unset?)";
        }

        ASSERT_NE( nullptr, application.adoptWindow( first.native() ) );
        EXPECT_EQ( nullptr, application.adoptWindow( second.native() ) );
    }

    //! setClientSize() on X11 asks the server, and the size follows once the server agrees.
    TEST( GuiX11Test, SetClientSizeResizesTheWindow )
    {
        ForeignX11Window foreign( 400, 300 );
        if( foreign.display() == nullptr )
        {
            GTEST_SKIP() << "XOpenDisplay() failed";
        }

        QtLikeSignalGuiTest::FakeCommandLine commandLine( { "-p", "x11" } );
        QtLikeSignalGui::GuiApplication application( commandLine.argc(), commandLine.argv() );

        if( !application.hasPlatform() )
        {
            GTEST_SKIP() << "no X server available (DISPLAY unset?)";
        }

        QtLikeSignalGui::Window* const window = application.adoptWindow( foreign.native() );
        ASSERT_NE( nullptr, window );

        window->setClientSize( 500, 350 );

        // Asked of the server rather than of the library: an X11 resize is a *request*, and what
        // this checks is that the request was actually made and honoured. The library's own figure
        // follows from the ConfigureNotify, which arrives through the loop and so is not available
        // in a test that never runs one.
        //
        // Waited for rather than read once. XSync flushes the request and waits for the round trip,
        // but that only guarantees the server has processed it -- under a window manager the
        // geometry can settle a moment later, and reading immediately made this test pass alone and
        // fail behind another one. A bounded wait asserts the same thing without asserting how fast
        // it happens.
        XWindowAttributes attributes {};
        bool resized = false;

        for( int attempt = 0; attempt < 100 && !resized; ++attempt )
        {
            XSync( foreign.display(), False );
            ASSERT_NE( 0, XGetWindowAttributes( foreign.display(), foreign.window(), &attributes ) )
            ;
            resized = ( attributes.width == 500 && attributes.height == 350 );

            if( !resized )
            {
                std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
            }
        }

        EXPECT_EQ( 500, attributes.width );
        EXPECT_EQ( 350, attributes.height );
    }

#endif // HAVE_X11
