// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! GoogleTest suite for the frame clock -- update pacing, and the seam a display reports frames
//! through.
//!
//! Driven against a test integration rather than a real display, so every case here runs on both
//! platforms and needs no compositor. The Wayland backend wires the same seam to wl_surface.frame;
//! what is portable is the contract, and that is what these check.

#include "QtLikeSignal-test-types.hpp"

#include "gtest/gtest.h"
#include "QtLikeSignalGui/PlatformIntegration.hpp"
#include "QtLikeSignalGui/Window.hpp"
#include "QtLikeSignalGui/WindowSystemInterface.hpp"
#include "QtLikeSignal/Object.hpp"
#include <memory>

using namespace QtLikeSignal;
using namespace QtLikeSignalGui;

namespace
{
    //! A backend with a frame clock and no display behind it.
    //!
    //! Does what the Wayland backend does with wl_surface.frame, minus the compositor: an update
    //! asked for while paced is parked, and released when a frame is reported. Everything the
    //! pacing contract promises is visible here, which is why these cases need no display.
    class FakeFrameClockIntegration : public PlatformIntegration
    {
    public:
        PlatformType type() const override
        {
            return PlatformType::Unknown;
        }

        bool canCreateWindows() const override
        {
            return true;
        }

        bool canAdoptWindows() const override
        {
            return false;
        }

        Window* createWindow
            (
            const WindowSettings& aSettings
            ) override
        {
            NativeWindow native;

            // Non-null so Window::hasNative() is true and requestUpdate() reaches this backend. It
            // is never dereferenced: nothing here talks to a window system.
            native.mSurface = this;

            mWindow = newWindow( this, native, aSettings.mWidth, aSettings.mHeight );
            return mWindow;
        }

        void releaseNativeWindow
            (
            Window* aWindow
            ) override
        {
            ( void )aWindow;
        }

        void setWindowTitle
            (
            Window* aWindow,
            const std::string& aTitle
            ) override
        {
            ( void )aWindow;
            ( void )aTitle;
        }

        void setWindowVisible
            (
            Window* aWindow,
            bool aVisible
            ) override
        {
            ( void )aWindow;
            ( void )aVisible;
        }

        void setClientSize
            (
            Window* aWindow,
            int aWidth,
            int aHeight
            ) override
        {
            ( void )aWindow;
            ( void )aWidth;
            ( void )aHeight;
        }

        bool hasFrameClock() const override
        {
            return mHasFrameClock;
        }

        void requestUpdate
            (
            Window* aWindow
            ) override
        {
            if( aWindow != mWindow )
            {
                return;
            }

            const bool paced = mHasFrameClock
                && ( aWindow->updatePacing() == Window::UpdatePacing::Display )
                && mHasDelivered;

            if( paced )
            {
                // Parked. One in flight: asking again before a frame arrives changes nothing.
                mPacedRequested = true;
                return;
            }

            deliver();
        }

        void deliverPacedUpdate
            (
            Window* aWindow
            ) override
        {
            if( aWindow != mWindow || !mPacedRequested )
            {
                return;
            }

            mPacedRequested = false;
            deliver();
        }

        void releasePacedUpdate
            (
            Window* aWindow
            ) override
        {
            if( aWindow != mWindow || !mPacedRequested )
            {
                return;
            }

            mPacedRequested = false;
            deliver();
        }

        //! Delivers one expose, and records that the bootstrap has happened.
        void deliver()
        {
            mHasDelivered = true;
            ++mDelivered;
            WindowSystemInterface::handleExpose( mWindow );
        }

        bool mHasFrameClock { true };   //!< Whether this backend paces. Flipped by one test.
        bool mPacedRequested { false }; //!< An update is parked waiting for a frame.
        bool mHasDelivered { false };   //!< The first update has gone out; see the Wayland backend.
        int mDelivered { 0 };           //!< Exposes delivered.
        Window* mWindow { nullptr };    //!< The one window this backend owns.
    };

    //! Builds a backend and a window, and counts the exposes the window emits.
    struct Fixture
    {
        Fixture()
        {
            WindowSettings settings;
            settings.mWidth = 64;
            settings.mHeight = 64;
            mWindow = mIntegration.createWindow( settings );

            QtLikeSignal::Object::connect( mWindow->getExposed(), &mCounter,
                &Counter::onExposed, QtLikeSignal::ConnectionType::Direct );
        }

        //! newWindow() hands back a raw Window this fixture owns; nothing else will free it.
        ~Fixture()
        {
            delete mWindow;
        }

        Fixture
            (
            const Fixture&
            ) = delete;

        Fixture& operator=
            (
            const Fixture&
            ) = delete;

        //! Counts expose signals.
        class Counter : public QtLikeSignal::Object
        {
        public:
            void onExposed()
            {
                ++mCount;
            }

            int mCount { 0 };
        };

        FakeFrameClockIntegration mIntegration;
        Window* mWindow { nullptr };
        Counter mCounter;
    };
}

//! Tests that the default is immediate delivery, which is what this library did before pacing.
TEST( FrameClockTest, TheDefaultIsImmediate )
{
    Fixture f;

    EXPECT_EQ( f.mWindow->updatePacing(), Window::UpdatePacing::Immediate );

    f.mWindow->requestUpdate();
    EXPECT_EQ( f.mCounter.mCount, 1 ) << "an unpaced update did not arrive.";

    f.mWindow->requestUpdate();
    EXPECT_EQ( f.mCounter.mCount, 2 ) << "an unpaced window stopped updating.";
}

//! Tests that a window reports whether its backend can pace at all.
TEST( FrameClockTest, AWindowSaysWhetherItHasAFrameClock )
{
    Fixture f;
    EXPECT_TRUE( f.mWindow->hasFrameClock() );

    f.mIntegration.mHasFrameClock = false;
    EXPECT_FALSE( f.mWindow->hasFrameClock() );
}

//! Tests that the first paced update still arrives without anything reporting a frame.
//!
//! The bootstrap. A frame request only reaches the compositor on the next commit, and before the
//! renderer has been given anything to draw there is no commit for one to ride on -- so the first
//! update goes out immediately whatever the pacing says, and every one after it is paced.
TEST( FrameClockTest, TheFirstPacedUpdateStillArrives )
{
    Fixture f;
    f.mWindow->setUpdatePacing( Window::UpdatePacing::Display );

    f.mWindow->requestUpdate();
    EXPECT_EQ( f.mCounter.mCount, 1 )
        << "the first paced update waited for a frame that nothing was going to send.";
}

//! Tests that a second paced update waits for the display.
TEST( FrameClockTest, ASecondPacedUpdateWaitsForTheDisplay )
{
    Fixture f;
    f.mWindow->setUpdatePacing( Window::UpdatePacing::Display );

    f.mWindow->requestUpdate();
    ASSERT_EQ( f.mCounter.mCount, 1 );

    f.mWindow->requestUpdate();
    EXPECT_EQ( f.mCounter.mCount, 1 ) << "a paced update did not wait for the display.";

    WindowSystemInterface::handleFrameReady( f.mWindow );
    EXPECT_EQ( f.mCounter.mCount, 2 ) << "reporting a frame did not release the update.";
}

//! Tests that repeated requests between frames collapse to one delivery.
//!
//! One in flight. A renderer that asks on every input event between two frames gets one repaint,
//! not one per event.
TEST( FrameClockTest, RepeatedRequestsCollapseToOneFrame )
{
    Fixture f;
    f.mWindow->setUpdatePacing( Window::UpdatePacing::Display );

    f.mWindow->requestUpdate();
    ASSERT_EQ( f.mCounter.mCount, 1 );

    for( int i = 0; i < 10; ++i )
    {
        f.mWindow->requestUpdate();
    }
    EXPECT_EQ( f.mCounter.mCount, 1 ) << "ten requests between frames were not collapsed.";

    WindowSystemInterface::handleFrameReady( f.mWindow );
    EXPECT_EQ( f.mCounter.mCount, 2 ) << "ten requests delivered more than one frame.";
}

//! Tests that a frame reported with nothing pending does not draw.
//!
//! A frame clock says the display is *ready*, which is not by itself a reason to draw. A window
//! that has stopped animating stays stopped.
TEST( FrameClockTest, AFrameWithNothingPendingDoesNotDraw )
{
    Fixture f;
    f.mWindow->setUpdatePacing( Window::UpdatePacing::Display );

    f.mWindow->requestUpdate();
    ASSERT_EQ( f.mCounter.mCount, 1 );

    for( int i = 0; i < 5; ++i )
    {
        WindowSystemInterface::handleFrameReady( f.mWindow );
    }
    EXPECT_EQ( f.mCounter.mCount, 1 ) << "a frame clock drew a window that had not asked.";
}

//! Tests that switching back to immediate releases a window that was waiting.
//!
//! Otherwise a program that turns pacing off while a frame is outstanding would be stuck, waiting
//! for a signal nothing is going to send -- and would then be handed the stale update later, when
//! some unrelated frame reported in and nothing had asked for one.
//!
//! **No requestUpdate() after the switch, deliberately.** An earlier version of this case had one,
//! which delivered a fresh immediate update and made the count come out right while the parked
//! request sat untouched in the backend. It passed before the release existed, which is the same
//! as not testing it.
TEST( FrameClockTest, SwitchingBackToImmediateReleasesAWaitingWindow )
{
    Fixture f;
    f.mWindow->setUpdatePacing( Window::UpdatePacing::Display );

    f.mWindow->requestUpdate();
    ASSERT_EQ( f.mCounter.mCount, 1 );

    f.mWindow->requestUpdate();
    ASSERT_EQ( f.mCounter.mCount, 1 ) << "the second update should be waiting on a frame.";

    f.mWindow->setUpdatePacing( Window::UpdatePacing::Immediate );

    EXPECT_EQ( f.mCounter.mCount, 2 ) << "turning pacing off left the window waiting.";

    // And nothing is left parked: a frame arriving afterwards has no request to answer.
    WindowSystemInterface::handleFrameReady( f.mWindow );
    EXPECT_EQ( f.mCounter.mCount, 2 )
        << "a stale request survived the switch and was delivered by a later frame.";
}

//! Tests that a backend without a frame clock ignores the pacing request.
//!
//! Asking to be paced where nothing will report a frame has to be harmless: the setting is kept,
//! and updates keep arriving as they would have. Anything else would make the same application
//! binary work on one backend and hang on another.
TEST( FrameClockTest, PacingIsHarmlessWithoutAFrameClock )
{
    Fixture f;
    f.mIntegration.mHasFrameClock = false;
    f.mWindow->setUpdatePacing( Window::UpdatePacing::Display );

    for( int i = 0; i < 4; ++i )
    {
        f.mWindow->requestUpdate();
    }

    EXPECT_EQ( f.mWindow->updatePacing(), Window::UpdatePacing::Display )
        << "the setting was discarded rather than kept.";
    EXPECT_EQ( f.mCounter.mCount, 4 )
        << "a backend with no frame clock stopped delivering updates.";
}

//! Tests that reporting a frame for a null window is ignored rather than a crash.
TEST( FrameClockTest, AFrameForNoWindowIsIgnored )
{
    WindowSystemInterface::handleFrameReady( nullptr );
}
