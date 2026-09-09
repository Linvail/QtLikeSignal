// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Tests for QtLikeSignalGui: the portable input types, platform selection, and -- where a window
//! system exists -- real windows and the signals their input arrives on.

#include <gtest/gtest.h>

#include "QtLikeSignalGui/GuiApplication.hpp"
#include "QtLikeSignalGui/PlatformIntegration.hpp"
#include "QtLikeSignalGui/Window.hpp"
#include "QtLikeSignalGui/WindowSystemInterface.hpp"

#include "QtLikeSignal/Object.hpp"

#include "QtLikeSignalGui-test-support.hpp"

#include <string>
#include <vector>

using namespace QtLikeSignalGui;

namespace
{
    //! Counts the signals it is connected to, so a test can assert one was emitted exactly once.
    class SignalRecorder : public QtLikeSignal::Object
    {
    public:
        //! Records a mouse event and keeps the last one seen.
        void onMouse
            (
            MouseEvent aEvent   //!< The event.
            )
        {
            ++mMouseCount;
            mLastMouse = aEvent;
        }

        //! Records a resize and keeps the size.
        void onResized
            (
            int aWidth,   //!< New width.
            int aHeight   //!< New height.
            )
        {
            ++mResizeCount;
            mLastWidth  = aWidth;
            mLastHeight = aHeight;
        }

        //! Records a touch frame.
        void onTouchFrame()
        {
            ++mTouchFrameCount;
        }

        //! Records a close request.
        void onCloseRequested()
        {
            ++mCloseCount;
        }

        int mMouseCount { 0 };        //!< Mouse events seen.
        int mResizeCount { 0 };       //!< Resizes seen.
        int mTouchFrameCount { 0 };   //!< Touch frames seen.
        int mCloseCount { 0 };        //!< Close requests seen.
        MouseEvent mLastMouse;        //!< Most recent mouse event.
        int mLastWidth { 0 };         //!< Width from the most recent resize.
        int mLastHeight { 0 };        //!< Height from the most recent resize.
    };
}

//! An empty set holds nothing, and None is never a member of any set.
TEST( GuiInputTest, MouseButtonsStartEmptyAndNoneIsNeverAMember )
{
    MouseButtons buttons;

    EXPECT_FALSE( buttons.any() );
    EXPECT_EQ( 0u, buttons.bits() );
    EXPECT_FALSE( buttons.test( MouseButton::None ) );
    EXPECT_FALSE( buttons.test( MouseButton::Left ) );

    // None must not test true even once the set has members: it is the absence of a button, not a
    // button, and a set that answers "yes I contain nothing" would make every guard on it useless.
    buttons |= MouseButton::Left;
    EXPECT_FALSE( buttons.test( MouseButton::None ) );
}

//! A set holds exactly the buttons put into it, and each one independently.
TEST( GuiInputTest, MouseButtonsHoldEachButtonIndependently )
{
    MouseButtons buttons = MouseButton::Left;
    EXPECT_TRUE( buttons.test( MouseButton::Left ) );
    EXPECT_FALSE( buttons.test( MouseButton::Right ) );

    buttons |= MouseButton::Right;
    EXPECT_TRUE( buttons.test( MouseButton::Left ) );
    EXPECT_TRUE( buttons.test( MouseButton::Right ) );
    EXPECT_FALSE( buttons.test( MouseButton::Middle ) );
    EXPECT_TRUE( buttons.any() );

    const MouseButtons both = MouseButtons( MouseButton::Left ) | MouseButton::Right;
    EXPECT_EQ( both, buttons );
    EXPECT_NE( both, MouseButtons( MouseButton::Left ) );
}

//! A single button converts implicitly to the set holding just it.
TEST( GuiInputTest, MouseButtonConvertsToASingletonSet )
{
    const MouseButtons buttons = MouseButton::Middle;

    EXPECT_TRUE( buttons.test( MouseButton::Middle ) );
    EXPECT_EQ( MouseButtons( MouseButton::Middle ), buttons );
}

//! The -p argument names the backend, in either of its accepted spellings.
//!
//! On Windows there is one window system and no choice to make, so the argument is ignored -- which
//! is itself the behaviour worth pinning down, since the same argument is meant for the external
//! library that creates the windows and will be present in argv on every platform.
TEST( GuiPlatformSelectionTest, PlatformArgumentNamesTheBackend )
{
    const std::vector<std::string> wayland { "program", "-p", "wayland" };
    const std::vector<std::string> x11 { "program", "--platform=x11" };
    const std::vector<std::string> upper { "program", "-platform", "X11" };

    #if defined( _WIN32 )
        EXPECT_EQ( PlatformType::Windows, PlatformIntegration::choosePlatform( wayland ) );
        EXPECT_EQ( PlatformType::Windows, PlatformIntegration::choosePlatform( x11 ) );
    #else
        EXPECT_EQ( PlatformType::Wayland, PlatformIntegration::choosePlatform( wayland ) );
        EXPECT_EQ( PlatformType::X11, PlatformIntegration::choosePlatform( x11 ) );

        // Folded to lower case, so "-p X11" and "-p x11" reach the same factory entry.
        EXPECT_EQ( PlatformType::X11, PlatformIntegration::choosePlatform( upper ) );
    #endif
}

//! An unknown backend name yields no integration rather than aborting the process.
TEST( GuiPlatformSelectionTest, UnknownBackendNameYieldsNoIntegration )
{
    // A name nothing recognises becomes Unknown at the one boundary that reads names, so the enum
    // never carries a value the rest of the library would have to check for.
    //
    // Windows is exempt because it does not read the name at all: there is one window system, and a
    // `-p` meant for the Linux build is ignored rather than being an error.
    #if !defined( _WIN32 )
        EXPECT_EQ( PlatformType::Unknown,
            PlatformIntegration::choosePlatform( { "test", "-p", "nosuchbackend" } ) );
    #endif

    // And Unknown produces no backend, on every platform.
    EXPECT_EQ( nullptr, PlatformIntegration::create( PlatformType::Unknown ) );
}

//! A modifier set starts empty, and None is never a member of it.
//!
//! The same contract MouseButtons has, tested the same way: KeyModifier::None is the absence of a
//! modifier rather than one of them, and a set that answered "yes, I contain nothing" would make
//! every guard written against it useless.
TEST( GuiInputTest, KeyModifiersStartEmptyAndNoneIsNeverAMember )
{
    KeyModifiers modifiers;

    EXPECT_FALSE( modifiers.any() );
    EXPECT_EQ( 0u, modifiers.bits() );
    EXPECT_FALSE( modifiers.test( KeyModifier::None ) );
    EXPECT_FALSE( modifiers.test( KeyModifier::Shift ) );

    modifiers |= KeyModifier::Shift;
    EXPECT_FALSE( modifiers.test( KeyModifier::None ) );
}

//! A modifier set holds each modifier independently, and remove() takes exactly one out.
TEST( GuiInputTest, KeyModifiersHoldEachModifierIndependently )
{
    KeyModifiers modifiers = KeyModifier::Shift;
    EXPECT_TRUE( modifiers.test( KeyModifier::Shift ) );
    EXPECT_FALSE( modifiers.test( KeyModifier::Control ) );

    modifiers |= KeyModifier::Control;
    EXPECT_TRUE( modifiers.test( KeyModifier::Shift ) );
    EXPECT_TRUE( modifiers.test( KeyModifier::Control ) );
    EXPECT_TRUE( modifiers.any() );

    // remove() is what the backends use to take a modifier key's own release out of the set, so it
    // must leave everything else alone.
    modifiers.remove( KeyModifier::Shift );
    EXPECT_FALSE( modifiers.test( KeyModifier::Shift ) );
    EXPECT_TRUE( modifiers.test( KeyModifier::Control ) );

    const KeyModifiers both = KeyModifiers( KeyModifier::Shift ) | KeyModifier::Control;
    EXPECT_NE( both, modifiers );
}

//! Key values follow Qt's numbering, which is the whole reason the enum is worth having.
//!
//! A program ported from Qt compares these against Qt::Key values it already knows. If the
//! numbering drifts, that comparison silently starts matching the wrong key -- so it is pinned
//! here rather than left as a claim in a comment.
TEST( GuiInputTest, KeyValuesUseQtNumbering )
{
    EXPECT_EQ( 0x41u, static_cast<unsigned int>( Key::A ) );
    EXPECT_EQ( 0x5au, static_cast<unsigned int>( Key::Z ) );
    EXPECT_EQ( 0x30u, static_cast<unsigned int>( Key::Digit0 ) );
    EXPECT_EQ( 0x39u, static_cast<unsigned int>( Key::Digit9 ) );
    EXPECT_EQ( 0x20u, static_cast<unsigned int>( Key::Space ) );

    // Everything without an ASCII meaning lives above the printable range, so no non-printing key
    // can ever collide with a character.
    EXPECT_GT( static_cast<unsigned int>( Key::Escape ), 0xffu );
    EXPECT_GT( static_cast<unsigned int>( Key::F1 ), 0xffu );
    EXPECT_EQ( 0u, static_cast<unsigned int>( Key::Unknown ) );
}

//! A KeyEvent starts empty, with no key, no modifiers, no text and no repeat.
//!
//! mText in particular has to start NUL-terminated: a backend that fills in nothing must leave a
//! readable empty string behind, not a buffer of whatever was on the stack.
TEST( GuiInputTest, KeyEventDefaultsAreEmpty )
{
    const KeyEvent event;

    EXPECT_EQ( Key::Unknown, event.mKey );
    EXPECT_FALSE( event.mModifiers.any() );
    EXPECT_EQ( 0u, event.mNativeCode );
    EXPECT_FALSE( event.mAutoRepeat );
    EXPECT_EQ( 0u, event.mTimestampMs );
    EXPECT_STREQ( "", event.mText );
}

//! A key press reaches the window's signal, carrying everything the backend put in it.
TEST( GuiWindowSystemInterfaceTest, KeyPressReachesTheWindow )
{
    GuiApplication app;
    if( !app.hasPlatform() )
    {
        GTEST_SKIP() << "no window system on this machine";
    }

    WindowSettings settings;
    settings.mWidth  = 320;
    settings.mHeight = 240;
    settings.mTitle  = "key test window";

    Window* const window = app.createWindow( settings );
    ASSERT_NE( nullptr, window );

    KeyEvent seen;
    int calls = 0;
    QtLikeSignal::Object::connect( window->getKeyPressed(), window,
        [&seen, &calls]( KeyEvent aEvent )
        {
            seen = aEvent;
            ++calls;
        } );

    KeyEvent sent;
    sent.mKey        = Key::A;
    sent.mModifiers  = KeyModifier::Shift;
    sent.mNativeCode = 0x41;
    sent.mText[0]    = 'A';
    sent.mAutoRepeat = true;

    WindowSystemInterface::handleKeyPressed( window, sent );

    EXPECT_EQ( 1, calls );
    EXPECT_EQ( Key::A, seen.mKey );
    EXPECT_TRUE( seen.mModifiers.test( KeyModifier::Shift ) );
    EXPECT_TRUE( seen.mAutoRepeat );
    EXPECT_STREQ( "A", seen.mText );
}

//! A release goes to its own signal, and never to the press one.
TEST( GuiWindowSystemInterfaceTest, KeyReleaseIsASeparateSignal )
{
    GuiApplication app;
    if( !app.hasPlatform() )
    {
        GTEST_SKIP() << "no window system on this machine";
    }

    WindowSettings settings;
    settings.mWidth  = 320;
    settings.mHeight = 240;
    settings.mTitle  = "key test window";

    Window* const window = app.createWindow( settings );
    ASSERT_NE( nullptr, window );

    int presses = 0;
    int releases = 0;
    QtLikeSignal::Object::connect( window->getKeyPressed(), window,
        [&presses]( KeyEvent )
        {
            ++presses;
        } );
    QtLikeSignal::Object::connect( window->getKeyReleased(), window,
        [&releases]( KeyEvent )
        {
            ++releases;
        } );

    KeyEvent event;
    event.mKey = Key::Escape;

    WindowSystemInterface::handleKeyReleased( window, event );

    EXPECT_EQ( 0, presses );
    EXPECT_EQ( 1, releases );
}

//! keyModifiers() reports what the last delivered event carried.
//!
//! The set is taken from the event rather than accumulated here, so what this really pins down is
//! that the funnel records it at all -- which is what lets a program ask about modifiers outside of
//! a key handler.
TEST( GuiWindowSystemInterfaceTest, KeyModifiersFollowTheLastEvent )
{
    KeyEvent event;
    event.mModifiers = KeyModifiers( KeyModifier::Control ) | KeyModifier::Alt;

    WindowSystemInterface::handleKeyPressed( nullptr, event );

    EXPECT_TRUE( WindowSystemInterface::keyModifiers().test( KeyModifier::Control ) );
    EXPECT_TRUE( WindowSystemInterface::keyModifiers().test( KeyModifier::Alt ) );
    EXPECT_FALSE( WindowSystemInterface::keyModifiers().test( KeyModifier::Shift ) );

    // Recorded even for a null window: the state is the seat's, not the window's, and a key
    // arriving for a window that has already gone still changed what is held.
    KeyEvent cleared;
    WindowSystemInterface::handleKeyReleased( nullptr, cleared );
    EXPECT_FALSE( WindowSystemInterface::keyModifiers().any() );
}

//! Every reporting function tolerates a null window.
//!
//! Not defensiveness for its own sake. A backend routes an event by looking up the window it
//! arrived for, and a lookup that finds nothing -- an event for a window already destroyed, or for
//! a foreign surface on Wayland -- is an ordinary outcome rather than a bug. Qt's own wl_touch
//! handler returns early on exactly that case.
TEST( GuiWindowSystemInterfaceTest, NullWindowsAreIgnored )
{
    const MouseEvent mouse;
    const WheelEvent wheel;
    const TouchDownEvent down;
    const TouchUpEvent up;
    const TouchMotionEvent motion;

    WindowSystemInterface::handleMousePressed( nullptr, mouse );
    WindowSystemInterface::handleMouseReleased( nullptr, mouse );
    WindowSystemInterface::handleMouseMoved( nullptr, mouse );
    WindowSystemInterface::handleMouseEntered( nullptr, mouse );
    WindowSystemInterface::handleMouseLeft( nullptr );
    WindowSystemInterface::handleWheel( nullptr, wheel );
    WindowSystemInterface::handleKeyPressed( nullptr, KeyEvent() );
    WindowSystemInterface::handleKeyReleased( nullptr, KeyEvent() );
    WindowSystemInterface::handleTouchDown( nullptr, down );
    WindowSystemInterface::handleTouchUp( nullptr, up );
    WindowSystemInterface::handleTouchMotion( nullptr, motion );
    WindowSystemInterface::handleTouchFrame( nullptr );
    WindowSystemInterface::handleTouchCancel( nullptr );
    WindowSystemInterface::handleResize( nullptr, 10, 10 );
    WindowSystemInterface::handleDevicePixelRatioChanged( nullptr, 2.0 );
    WindowSystemInterface::handleExpose( nullptr );
    WindowSystemInterface::handleCloseRequest( nullptr );
    WindowSystemInterface::handleFocusChange( nullptr, true );
    WindowSystemInterface::handleWindowDestroyed( nullptr );

    SUCCEED();
}

#if defined( _WIN32 )

    // The tests below check the library's answers against Windows' own -- GetClientRect is what a
    // renderer would size its viewport from, so it is the authority on whether the drawable came
    // out right. That is the one thing worth reaching past the abstraction to verify.
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #include <tchar.h>

    //! A created window has a native handle and the client size that was asked for.
    TEST( GuiWindowTest, CreatedWindowHasAHandleAndTheRequestedClientSize )
    {
        GuiApplication application;
        ASSERT_TRUE( application.hasPlatform() );
        EXPECT_EQ( PlatformType::Windows, application.platformType() );

        WindowSettings settings;
        settings.mWidth  = 640;
        settings.mHeight = 480;
        settings.mTitle  = "test window";

        Window* const window = application.createWindow( settings );
        ASSERT_NE( nullptr, window );

        // The handle is what an external OpenGL library is handed, so it has to exist before
        // anything is shown and without the loop having run.
        EXPECT_NE( nullptr, window->nativeHandle() );
        EXPECT_EQ( 640, window->width() );
        EXPECT_EQ( 480, window->height() );

        // Created hidden, so a caller can finish setting the window up before it appears.
        EXPECT_FALSE( window->isVisible() );
    }

    //! Windows are owned by the application and listed by it.
    TEST( GuiWindowTest, ApplicationOwnsAndListsItsWindows )
    {
        GuiApplication application;
        ASSERT_TRUE( application.hasPlatform() );

        Window* const first  = application.createWindow( WindowSettings() );
        Window* const second = application.createWindow( WindowSettings() );
        ASSERT_NE( nullptr, first );
        ASSERT_NE( nullptr, second );

        EXPECT_EQ( 2u, application.windows().size() );
        EXPECT_EQ( &application, first->parent() );

        // Destroying one takes it out of the list, so the list cannot hand back a freed window.
        delete first;
        ASSERT_EQ( 1u, application.windows().size() );
        EXPECT_EQ( second, application.windows().front() );

        // The other is destroyed with the application, before the backend that made it is released.
    }

    //! show() and hide() track what was asked for, and the handle survives both.
    TEST( GuiWindowTest, ShowAndHideTrackVisibility )
    {
        GuiApplication application;
        ASSERT_TRUE( application.hasPlatform() );

        Window* const window = application.createWindow( WindowSettings() );
        ASSERT_NE( nullptr, window );

        window->show();
        EXPECT_TRUE( window->isVisible() );

        window->hide();
        EXPECT_FALSE( window->isVisible() );

        // Hiding is not closing: the native window is still there to draw on.
        EXPECT_NE( nullptr, window->nativeHandle() );
    }

    //! Attaching a menu bar leaves the drawable client area exactly the size it was.
    //!
    //! The whole point of the requested size is that it is what WGL will present to. A menu bar
    //! lives outside the client area, so a bare SetMenu() would quietly take its height off the
    //! bottom of every frame; setMenu() grows the window instead.
    TEST( GuiWindowTest, AttachingAMenuKeepsTheClientAreaTheRequestedSize )
    {
        GuiApplication application;
        ASSERT_TRUE( application.hasPlatform() );

        WindowSettings settings;
        settings.mWidth  = 640;
        settings.mHeight = 480;

        Window* const window = application.createWindow( settings );
        ASSERT_NE( nullptr, window );
        ASSERT_EQ( 640, window->width() );
        ASSERT_EQ( 480, window->height() );

        const HMENU menu = CreateMenu();
        ASSERT_NE( nullptr, menu );
        const HMENU fileMenu = CreatePopupMenu();
        ASSERT_NE( nullptr, fileMenu );
        AppendMenu( fileMenu, MF_STRING, 1, TEXT( "E&xit" ) );
        AppendMenu( menu, MF_POPUP, reinterpret_cast<UINT_PTR>( fileMenu ), TEXT( "&File" ) );

        // The outer window must grow by the menu's height for the client area to stay put, so the
        // before/after window height is what proves the client area was preserved rather than
        // clamped.
        RECT before {};
        GetWindowRect( static_cast<HWND>( window->nativeHandle() ), &before );

        window->setMenu( menu );

        EXPECT_EQ( 640, window->width() );
        EXPECT_EQ( 480, window->height() );

        RECT after {};
        GetWindowRect( static_cast<HWND>( window->nativeHandle() ), &after );
        EXPECT_GT( after.bottom - after.top, before.bottom - before.top );

        // GetClientRect is the authority: it is what a renderer would size its viewport from.
        RECT client {};
        GetClientRect( static_cast<HWND>( window->nativeHandle() ), &client );
        EXPECT_EQ( 640, client.right - client.left );
        EXPECT_EQ( 480, client.bottom - client.top );

        // The menu belongs to the window now, and DestroyWindow frees it with the window.
    }

    //! setClientSize() produces exactly the client area asked for, and reports the change.
    TEST( GuiWindowTest, SetClientSizeProducesTheRequestedDrawable )
    {
        GuiApplication application;
        ASSERT_TRUE( application.hasPlatform() );

        Window* const window = application.createWindow( WindowSettings() );
        ASSERT_NE( nullptr, window );

        SignalRecorder recorder;
        QtLikeSignal::Object::connect( window->getResized(), &recorder, &SignalRecorder::onResized,
            QtLikeSignal::ConnectionType::Direct );

        window->setClientSize( 800, 600 );

        RECT client {};
        GetClientRect( static_cast<HWND>( window->nativeHandle() ), &client );
        EXPECT_EQ( 800, client.right - client.left );
        EXPECT_EQ( 600, client.bottom - client.top );

        // The window's own record follows, because SetWindowPos sends WM_SIZE and the resize
        // travels the ordinary path -- so a renderer needs no special case for a programmatic
        // resize.
        EXPECT_EQ( 800, window->width() );
        EXPECT_EQ( 600, window->height() );
        EXPECT_GE( recorder.mResizeCount, 1 );

        // Nonsense sizes are refused rather than producing a degenerate window.
        window->setClientSize( 0, 600 );
        EXPECT_EQ( 800, window->width() );
    }

    //! Reporting a resize records the new size before the signal, and emits it once.
    TEST( GuiWindowTest, ResizeRecordsTheSizeBeforeEmitting )
    {
        GuiApplication application;
        ASSERT_TRUE( application.hasPlatform() );

        Window* const window = application.createWindow( WindowSettings() );
        ASSERT_NE( nullptr, window );

        SignalRecorder recorder;
        QtLikeSignal::Object::connect( window->getResized(), &recorder, &SignalRecorder::onResized,
            QtLikeSignal::ConnectionType::Direct );

        WindowSystemInterface::handleResize( window, 320, 240 );

        EXPECT_EQ( 1, recorder.mResizeCount );
        EXPECT_EQ( 320, recorder.mLastWidth );
        EXPECT_EQ( 240, recorder.mLastHeight );

        // A slot asking the window for its size during the emission must get the new one, which is
        // why the write happens first. Checked afterwards for the same value.
        EXPECT_EQ( 320, window->width() );
        EXPECT_EQ( 240, window->height() );
    }

    //! A window starts at a usable ratio, whatever backend created it.
    //!
    //! The value every coordinate this library reports is implicitly multiplied by, so a window
    //! that started at zero or at something negative would make width() meaningless.
    //!
    //! **Not asserted to be 1.0, and that is the point of the test rather than a weakening of it.**
    //! 1.0 is the default a Window is constructed with, but X11 reads `Xft.dpi` and Win32 asks
    //! GetDpiForWindow while the window is still being built, so a desktop at 150 % legitimately
    //! hands back 1.5 here. An equality check would pass on an unscaled machine and fail on the
    //! developer's, which is a test measuring the desk it runs on.
    TEST( GuiWindowTest, ADevicePixelRatioStartsAtSomethingUsable )
    {
        GuiApplication application;
        ASSERT_TRUE( application.hasPlatform() );

        Window* const window = application.createWindow( WindowSettings() );
        ASSERT_NE( nullptr, window );

        EXPECT_GT( window->devicePixelRatio(), 0.0 );
    }

    //! Creating a window does not emit the change signal, whatever scale the backend found.
    //!
    //! The contract getDevicePixelRatioChanged() states. A backend that knows the scale during
    //! creation records it instead, because nothing can be connected to a window that has not been
    //! returned yet -- so an emission there reaches nobody while still costing the signal its
    //! meaning. Connecting afterwards and asking for the ratio is how a caller learns the starting
    //! value, and this test is written the way such a caller would be.
    TEST( GuiWindowTest, CreationReportsNoDevicePixelRatioChange )
    {
        GuiApplication application;
        ASSERT_TRUE( application.hasPlatform() );

        Window* const window = application.createWindow( WindowSettings() );
        ASSERT_NE( nullptr, window );

        const double atCreation = window->devicePixelRatio();

        int changes = 0;
        QtLikeSignal::Object::connect( window->getDevicePixelRatioChanged(), window,
            [&changes]( double )
            {
                ++changes;
            } );

        // Re-reporting the value the window already has is filtered, so a backend re-reading its
        // scale is not a change either.
        WindowSystemInterface::handleDevicePixelRatioChanged( window, atCreation );
        EXPECT_EQ( 0, changes );

        WindowSystemInterface::handleDevicePixelRatioChanged( window, atCreation * 2.0 );
        EXPECT_EQ( 1, changes );
        EXPECT_DOUBLE_EQ( atCreation * 2.0, window->devicePixelRatio() );
    }

    //! Reporting a ratio records it before the signal, and emits it once.
    //!
    //! The same contract handleResize() keeps, and for the same reason: a renderer's slot asks the
    //! window for the numbers it needs rather than trusting only what it was handed, so the window
    //! has to be right by the time the slot runs.
    TEST( GuiWindowTest, ADevicePixelRatioIsRecordedBeforeEmitting )
    {
        GuiApplication application;
        ASSERT_TRUE( application.hasPlatform() );

        Window* const window = application.createWindow( WindowSettings() );
        ASSERT_NE( nullptr, window );

        // Every ratio below is derived from the one creation found, never written as a literal.
        // X11 and Win32 read the desktop's scale while building the window, so a hard-coded 2.0
        // here is the same value the window already holds on a 200 % screen -- and an equal report
        // is filtered, so the signal this test is about would never be emitted. The test would
        // then fail on the developer's machine and pass on the build server.
        const double atCreation = window->devicePixelRatio();
        const double changed    = atCreation * 2.0;

        int count = 0;
        double seen = 0.0;
        double duringEmission = 0.0;

        QtLikeSignal::Object::connect( window->getDevicePixelRatioChanged(), window,
            [&]( double aRatio )
            {
                ++count;
                seen = aRatio;
                duringEmission = window->devicePixelRatio();
            }, QtLikeSignal::ConnectionType::Direct );

        WindowSystemInterface::handleDevicePixelRatioChanged( window, changed );

        EXPECT_EQ( 1, count );
        EXPECT_DOUBLE_EQ( changed, seen );
        EXPECT_DOUBLE_EQ( changed, duringEmission )
            << "a slot asking the window during the emission got the old ratio.";
        EXPECT_DOUBLE_EQ( changed, window->devicePixelRatio() );
    }

    //! A ratio equal to the current one, or not positive, changes nothing and says nothing.
    //!
    //! Both filters live in WindowSystemInterface rather than in four backends, so a backend may
    //! call it every time it re-reads the scale. Wayland does exactly that: wl_output sends its
    //! properties as a burst, and the ratio is recomputed at the end of every one.
    TEST( GuiWindowTest, ADevicePixelRatioIgnoresRepeatsAndNonsense )
    {
        GuiApplication application;
        ASSERT_TRUE( application.hasPlatform() );

        Window* const window = application.createWindow( WindowSettings() );
        ASSERT_NE( nullptr, window );

        // Relative to the ratio creation found, for the reason the test above says: a literal 1.0
        // is what an unscaled host starts at, so "report the value it already has" would become
        // "report a different one" on a 150 % screen and every count below would be out by one.
        // Zero and the negative stay literal -- they are rejected whatever the window holds.
        const double atCreation = window->devicePixelRatio();
        const double changed    = atCreation * 2.0;

        int count = 0;
        QtLikeSignal::Object::connect( window->getDevicePixelRatioChanged(), window,
            [&count]( double )
            {
                ++count;
            }, QtLikeSignal::ConnectionType::Direct );

        WindowSystemInterface::handleDevicePixelRatioChanged( window, atCreation );
        EXPECT_EQ( 0, count ) << "the ratio it already had was reported as a change.";

        WindowSystemInterface::handleDevicePixelRatioChanged( window, changed );
        EXPECT_EQ( 1, count );

        WindowSystemInterface::handleDevicePixelRatioChanged( window, changed );
        EXPECT_EQ( 1, count ) << "the same ratio twice was reported twice.";

        WindowSystemInterface::handleDevicePixelRatioChanged( window, 0.0 );
        WindowSystemInterface::handleDevicePixelRatioChanged( window, -2.0 );
        EXPECT_EQ( 1, count );
        EXPECT_DOUBLE_EQ( changed, window->devicePixelRatio() )
            << "a ratio that cannot be true was allowed to replace one that was.";
    }

    //! A press reaches the window's signal and updates the application-wide button set.
    TEST( GuiWindowTest, MousePressReachesTheSignalAndTheGlobalButtonState )
    {
        GuiApplication application;
        ASSERT_TRUE( application.hasPlatform() );

        Window* const window = application.createWindow( WindowSettings() );
        ASSERT_NE( nullptr, window );

        SignalRecorder recorder;
        QtLikeSignal::Object::connect( window->getMousePressed(), &recorder,
            &SignalRecorder::onMouse, QtLikeSignal::ConnectionType::Direct );

        MouseEvent event;
        event.mPos     = { 12, 34 };
        event.mButton  = MouseButton::Left;
        event.mButtons = MouseButton::Left;

        WindowSystemInterface::handleMousePressed( window, event );

        EXPECT_EQ( 1, recorder.mMouseCount );
        EXPECT_EQ( 12, recorder.mLastMouse.mPos.mX );
        EXPECT_EQ( 34, recorder.mLastMouse.mPos.mY );
        EXPECT_TRUE( GuiApplication::mouseButtons().test( MouseButton::Left ) );

        MouseEvent release = event;
        release.mButtons = MouseButtons();
        WindowSystemInterface::handleMouseReleased( window, release );

        EXPECT_FALSE( GuiApplication::mouseButtons().any() );
    }

    //! A touch frame with no window named goes to the window that took the last touch down.
    //!
    //! wl_touch reports frame against the seat rather than against a surface, so this routing is
    //! the only thing that makes the signal usable when more than one window exists.
    TEST( GuiWindowTest, TouchFrameFollowsTheWindowThatTookTheLastDown )
    {
        GuiApplication application;
        ASSERT_TRUE( application.hasPlatform() );

        Window* const first  = application.createWindow( WindowSettings() );
        Window* const second = application.createWindow( WindowSettings() );
        ASSERT_NE( nullptr, first );
        ASSERT_NE( nullptr, second );

        SignalRecorder firstRecorder;
        SignalRecorder secondRecorder;
        QtLikeSignal::Object::connect( first->getTouchFrame(), &firstRecorder,
            &SignalRecorder::onTouchFrame, QtLikeSignal::ConnectionType::Direct );
        QtLikeSignal::Object::connect( second->getTouchFrame(), &secondRecorder,
            &SignalRecorder::onTouchFrame, QtLikeSignal::ConnectionType::Direct );

        TouchDownEvent down;
        down.mId = 1;

        WindowSystemInterface::handleTouchDown( second, down );
        WindowSystemInterface::handleTouchFrame();

        EXPECT_EQ( 0, firstRecorder.mTouchFrameCount );
        EXPECT_EQ( 1, secondRecorder.mTouchFrameCount );
    }

    //! Destroying a window clears the touch focus, so a later frame does not reach freed memory.
    TEST( GuiWindowTest, DestroyingAWindowClearsTheTouchFocus )
    {
        GuiApplication application;
        ASSERT_TRUE( application.hasPlatform() );

        Window* const window = application.createWindow( WindowSettings() );
        ASSERT_NE( nullptr, window );

        TouchDownEvent down;
        down.mId = 1;
        WindowSystemInterface::handleTouchDown( window, down );

        delete window;

        // Would dereference the destroyed window if ~Window() had not cleared the focus.
        WindowSystemInterface::handleTouchFrame();
        WindowSystemInterface::handleTouchCancel();

        SUCCEED();
    }

    //! A close request emits the signal, and the default policy hides the window.
    //!
    //! Hidden rather than destroyed: the application may still hold a GL context bound to the
    //! handle, and the library must not pull that out from under it. See
    //! GuiApplication::handleCloseRequested().
    TEST( GuiWindowTest, CloseRequestEmitsAndHidesUnderTheDefaultPolicy )
    {
        GuiApplication application;
        ASSERT_TRUE( application.hasPlatform() );
        ASSERT_TRUE( application.quitOnLastWindowClosed() );

        Window* const window = application.createWindow( WindowSettings() );
        ASSERT_NE( nullptr, window );
        window->show();

        SignalRecorder recorder;
        QtLikeSignal::Object::connect( window->getCloseRequested(), &recorder,
            &SignalRecorder::onCloseRequested, QtLikeSignal::ConnectionType::Direct );

        WindowSystemInterface::handleCloseRequest( window );

        EXPECT_EQ( 1, recorder.mCloseCount );
        EXPECT_FALSE( window->isVisible() );
        EXPECT_NE( nullptr, window->nativeHandle() );
    }

    //! With the policy cleared, a close request is reported and nothing else happens.
    TEST( GuiWindowTest, CloseRequestLeavesTheWindowAloneWhenThePolicyIsCleared )
    {
        GuiApplication application;
        ASSERT_TRUE( application.hasPlatform() );
        application.setQuitOnLastWindowClosed( false );

        Window* const window = application.createWindow( WindowSettings() );
        ASSERT_NE( nullptr, window );
        window->show();

        SignalRecorder recorder;
        QtLikeSignal::Object::connect( window->getCloseRequested(), &recorder,
            &SignalRecorder::onCloseRequested, QtLikeSignal::ConnectionType::Direct );

        WindowSystemInterface::handleCloseRequest( window );

        EXPECT_EQ( 1, recorder.mCloseCount );
        EXPECT_TRUE( window->isVisible() );
    }

    //! Deleting the window from the close slot is survivable.
    //!
    //! The close policy runs after the signal, so without the lifetime check in
    //! handleCloseRequest() this is a use-after-free -- and destroying the window on close is an
    //! entirely reasonable thing for an application to do.
    TEST( GuiWindowTest, ClosingSlotMayDestroyTheWindow )
    {
        GuiApplication application;
        ASSERT_TRUE( application.hasPlatform() );

        Window* const window = application.createWindow( WindowSettings() );
        ASSERT_NE( nullptr, window );

        QtLikeSignal::Object context;
        QtLikeSignal::Object::connect( window->getCloseRequested(), &context,
            [window]()
            {
                delete window;
            },
            QtLikeSignal::ConnectionType::Direct );

        WindowSystemInterface::handleCloseRequest( window );

        EXPECT_EQ( 0u, application.windows().size() );
    }

#endif // _WIN32

#if defined( __linux__ )

    //! -p drm selects the DRM backend, which adopts rather than creates.
    //!
    //! Constructing the backend touches no device: libinput is opened by the first adoptWindow(),
    //! so this runs anywhere, including a container with no /dev/input at all.
    TEST( GuiDrmTest, PlatformArgumentSelectsTheDrmBackend )
    {
        QtLikeSignalGuiTest::FakeCommandLine commandLine( { "-p", "drm" } );
        GuiApplication application( commandLine.argc(), commandLine.argv() );

        if( !application.hasPlatform() )
        {
            GTEST_SKIP() << "this build has no drm backend (libinput-dev/libudev-dev missing)";
        }

        EXPECT_EQ( PlatformType::Drm, application.platformType() );

        // There is no window system, so there is nothing to create a window with -- the external
        // library sets the mode and makes the surface, and this backend adopts the result.
        EXPECT_EQ( nullptr, application.createWindow( WindowSettings() ) );
    }

    //! The DRM backend refuses an adoption that does not say how big the scanout is.
    //!
    //! Refused rather than defaulted: every coordinate this backend produces is clamped or
    //! transformed against that size, so guessing it would put the pointer and every touch point in
    //! the wrong place with nothing to indicate why. Checked before libinput is touched, so no
    //! device is needed.
    TEST( GuiDrmTest, AdoptionWithoutASizeIsRefused )
    {
        QtLikeSignalGuiTest::FakeCommandLine commandLine( { "-p", "drm" } );
        GuiApplication application( commandLine.argc(), commandLine.argv() );

        if( !application.hasPlatform() )
        {
            GTEST_SKIP() << "this build has no drm backend (libinput-dev/libudev-dev missing)";
        }

        NativeWindow native;
        native.mWidth  = 0;
        native.mHeight = 0;

        EXPECT_EQ( nullptr, application.adoptWindow( native ) );

        native.mWidth  = 1920;
        native.mHeight = 0;
        EXPECT_EQ( nullptr, application.adoptWindow( native ) );
    }

#endif // __linux__

#if defined( HAVE_WAYLAND_CLIENT )

    //! -p wayland selects the Wayland backend, and it creates its own windows.
    TEST( GuiWaylandTest, PlatformArgumentSelectsTheWaylandBackend )
    {
        QtLikeSignalGuiTest::FakeCommandLine commandLine( { "-p", "wayland" } );
        GuiApplication application( commandLine.argc(), commandLine.argv() );

        if( !application.hasPlatform() )
        {
            GTEST_SKIP() << "no Wayland compositor reachable (WAYLAND_DISPLAY unset?)";
        }

        EXPECT_EQ( PlatformType::Wayland, application.platformType() );
    }

    //! Asking for the connection is what opens it, before there is any window.
    //!
    //! That order is the whole reason nativeDisplay() is not a passive query: an EGL library has to
    //! reach eglGetPlatformDisplayEXT( EGL_PLATFORM_WAYLAND_EXT, display ) to choose a config, and
    //! it has to do that before there is a surface to give the config to.
    TEST( GuiWaylandTest, TheConnectionOpensBeforeAnyWindowExists )
    {
        QtLikeSignalGuiTest::FakeCommandLine commandLine( { "-p", "wayland" } );
        GuiApplication application( commandLine.argc(), commandLine.argv() );

        if( !application.hasPlatform() )
        {
            GTEST_SKIP() << "no Wayland compositor reachable (WAYLAND_DISPLAY unset?)";
        }

        void* const display = application.nativeDisplay();
        if( display == nullptr )
        {
            GTEST_SKIP() << "wl_display_connect() failed";
        }

        EXPECT_EQ( 0u, application.windows().size() );

        // The same connection every time: a second one would need a second registration with the
        // loop.
        EXPECT_EQ( display, application.nativeDisplay() );
    }

    //! A created window carries the surface and the connection an EGL library needs.
    TEST( GuiWaylandTest, CreatedWindowCarriesTheSurfaceAndTheDisplay )
    {
        QtLikeSignalGuiTest::FakeCommandLine commandLine( { "-p", "wayland" } );
        GuiApplication application( commandLine.argc(), commandLine.argv() );

        if( !application.hasPlatform() || application.nativeDisplay() == nullptr )
        {
            GTEST_SKIP() << "no Wayland compositor reachable (WAYLAND_DISPLAY unset?)";
        }

        WindowSettings settings;
        settings.mWidth  = 640;
        settings.mHeight = 480;
        settings.mTitle  = "created";
        settings.mAppId  = "com.example.qtlikesignalgui.test";

        Window* const window = application.createWindow( settings );
        ASSERT_NE( nullptr, window );

        // wl_surface* through nativeHandle(), wl_display* through nativeDisplay() -- the pair
        // wl_egl_window_create() and eglGetPlatformDisplayEXT() are given.
        EXPECT_NE( nullptr, window->nativeHandle() );
        EXPECT_EQ( application.nativeDisplay(), window->nativeDisplay() );

        // A window is a pointer on Wayland, not a resource id, so there is nothing for this to be.
        EXPECT_EQ( 0u, window->nativeWindowId() );

        EXPECT_EQ( 640, window->width() );
        EXPECT_EQ( 480, window->height() );
    }

    //! One window, and a second is refused rather than silently sharing the first one's focus.
    TEST( GuiWaylandTest, ASecondWindowIsRefused )
    {
        QtLikeSignalGuiTest::FakeCommandLine commandLine( { "-p", "wayland" } );
        GuiApplication application( commandLine.argc(), commandLine.argv() );

        if( !application.hasPlatform() || application.nativeDisplay() == nullptr )
        {
            GTEST_SKIP() << "no Wayland compositor reachable (WAYLAND_DISPLAY unset?)";
        }

        ASSERT_NE( nullptr, application.createWindow( WindowSettings() ) );
        EXPECT_EQ( nullptr, application.createWindow( WindowSettings() ) );
    }

    //! Destroying the window releases the surface but leaves the connection open.
    //!
    //! The connection outlives the window because the backend owns it: a program is free to destroy
    //! a window and make another, and reconnecting would drop every global that was bound.
    TEST( GuiWaylandTest, DestroyingTheWindowKeepsTheConnection )
    {
        QtLikeSignalGuiTest::FakeCommandLine commandLine( { "-p", "wayland" } );
        GuiApplication application( commandLine.argc(), commandLine.argv() );

        if( !application.hasPlatform() || application.nativeDisplay() == nullptr )
        {
            GTEST_SKIP() << "no Wayland compositor reachable (WAYLAND_DISPLAY unset?)";
        }

        Window* const window = application.createWindow( WindowSettings() );
        ASSERT_NE( nullptr, window );

        void* const display = application.nativeDisplay();
        delete window;

        EXPECT_EQ( 0u, application.windows().size() );
        EXPECT_EQ( display, application.nativeDisplay() );

        // And another window can be made on it, which is what "the connection outlives the window"
        // is for.
        EXPECT_NE( nullptr, application.createWindow( WindowSettings() ) );
    }

#endif // HAVE_WAYLAND_CLIENT
