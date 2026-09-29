// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! A real Wayland program on QtLikeSignalGui: the library creates the surface and delivers the
//! seat's input as signals, and this program draws into it.
//!
//! **The split is the point, and it is the same one every backend makes.** QtLikeSignalGui connects
//! to the compositor, binds the globals, gives the surface a role on xdg-shell or ivi-shell, and
//! turns wl_pointer and wl_touch into Window's signals. It never attaches a buffer, because a
//! Wayland surface's contents belong to whatever renders -- here a shared-memory buffer this
//! program fills itself, in a real program an EGLSurface built from the same wl_display and
//! wl_surface.
//!
//! That is why this file binds a wl_shm of its own from the connection QtLikeSignalGui opened: it
//! is standing in for the renderer, and a renderer is entitled to the connection and the surface
//! and nothing else.
//!
//! There is no event loop here and no wl_display_dispatch call. exec() is the only loop, and
//! PlatformIntegrationWayland registers the compositor socket with EventDispatcherLinux, so one
//! poll() waits on that socket, the dispatcher's eventfd and the timer deadline together. When
//! nothing is happening the process uses no CPU at all -- watch it in top while the window sits
//! idle, then move the pointer over it.
//!
//! Controls: move, click, scroll or touch anywhere in the window; the close button quits, and so
//! does Ctrl+C -- a SignalWatcher turns it into a signal on the loop, so the teardown runs.
//!
//! A click also sends the top bar across. That is a PropertyAnimation: it moves a value, the value
//! reports the change, and the report is what asks for the repaint. See the comment beside it in
//! main() for why the animation is not driven from the frame instead, which is the arrangement
//! that looks right and is not.

#include "QtLikeSignalGui/GuiApplication.hpp"
#include "QtLikeSignalGui/Window.hpp"

#include "QtLikeSignal/CoreApplication.hpp"
#include "QtLikeSignal/Object.hpp"
#include "QtLikeSignal/Property.hpp"
#include "QtLikeSignal/PropertyAnimation.hpp"
#include "QtLikeSignal/SignalWatcher.hpp"
#include "QtLikeSignal/Timer.hpp"

#include "QtLikeSignal/Log.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>

//! This demo's category.
QTLIKESIGNAL_DEFINE_LOG_CATEGORY( gLogDemo, "demo.gui.wayland", "DGWA" )

namespace Gui = QtLikeSignalGui;

namespace
{
    //! Background, in XRGB8888.
    const std::uint32_t kBackground = 0xff181a20;

    //! The crosshair that tracks the pointer.
    const std::uint32_t kCrosshair = 0xff78c8ff;

    //! The block that lights up while a mouse button is held.
    const std::uint32_t kPressed = 0xffa0e6a0;

    //! The block that lights up while a finger is down.
    const std::uint32_t kTouched = 0xffe6a0a0;

    //! Creates an anonymous file of @p aSize bytes to share with the compositor.
    int createSharedFile
        (
        std::size_t aSize   //!< Bytes the file should hold.
        )
    {
        const int fd = ::memfd_create( "qtlikesignalgui-demo", MFD_CLOEXEC );
        if( fd < 0 )
        {
            return -1;
        }

        if( ::ftruncate( fd, static_cast<off_t>( aSize ) ) < 0 )
        {
            ::close( fd );
            return -1;
        }

        return fd;
    }

    //! Draws into the window, standing in for the rendering library.
    //!
    //! An ordinary QtLikeSignal::Object. It was handed a wl_display and a wl_surface and it paints
    //! them; it never sees a wl_pointer, a listener or an event queue, because everything it reacts
    //! to arrives as a signal.
    class Renderer : public QtLikeSignal::Object
    {
    public:
        //! Constructs the renderer against the window it draws in.
        Renderer
            (
            Gui::Window* aWindow   //!< The window. Not owned; the application owns it.
            )
            : mWindow( aWindow )
            , mDisplay( static_cast<wl_display*>( aWindow->nativeDisplay() ) )
            , mSurface( static_cast<wl_surface*>( aWindow->nativeHandle() ) )
            , mWidth( aWindow->width() )
            , mHeight( aWindow->height() )
        {
        }

        //! Releases the buffer and its mapping.
        ~Renderer() override
        {
            if( mBuffer != nullptr )
            {
                wl_buffer_destroy( mBuffer );
            }
            if( mPixels != nullptr )
            {
                ::munmap( mPixels, mPixelBytes );
            }
            if( mShm != nullptr )
            {
                wl_shm_destroy( mShm );
            }
            if( mRegistry != nullptr )
            {
                wl_registry_destroy( mRegistry );
            }
        }

        //! Binds wl_shm and makes the buffer the surface will show.
        //!
        //! A registry of this renderer's own, on the connection QtLikeSignalGui opened. Two
        //! registries on one display is ordinary Wayland: each client object is independent, and
        //! this is exactly the position an EGL library is in when it calls
        //! eglGetPlatformDisplayEXT() on a display somebody else connected.
        bool initialise()
        {
            if( mDisplay == nullptr || mSurface == nullptr )
            {
                qCWarning( gLogDemo ) << "no wl_display or wl_surface from the window";
                return false;
            }

            mRegistry = wl_display_get_registry( mDisplay );
            wl_registry_add_listener( mRegistry, &kRegistryListener, this );
            wl_display_roundtrip( mDisplay );

            if( mShm == nullptr )
            {
                qCWarning( gLogDemo ) << "the compositor offers no wl_shm";
                return false;
            }

            return createBuffer();
        }

        //! Records a mouse press.
        void onMousePressed
            (
            Gui::MouseEvent aEvent   //!< The press.
            )
        {
            ++mPressCount;
            mLast = aEvent;
            std::printf( "press    button %u at %4d,%4d  held now %#x\n",
                static_cast<unsigned>( aEvent.mButton ), aEvent.mPos.mX, aEvent.mPos.mY,
                aEvent.mButtons.bits() );
            mWindow->requestUpdate();
        }

        //! Records a mouse release.
        void onMouseReleased
            (
            Gui::MouseEvent aEvent   //!< The release.
            )
        {
            ++mReleaseCount;
            mLast = aEvent;
            std::printf( "release  button %u at %4d,%4d  held now %#x\n",
                static_cast<unsigned>( aEvent.mButton ), aEvent.mPos.mX, aEvent.mPos.mY,
                aEvent.mButtons.bits() );
            mWindow->requestUpdate();
        }

        //! Records a move. Only the crosshair follows it; the console would drown in these.
        void onMouseMoved
            (
            Gui::MouseEvent aEvent   //!< The move.
            )
        {
            ++mMoveCount;
            mLast = aEvent;
            mWindow->requestUpdate();
        }

        //! Records the pointer arriving.
        void onMouseEntered
            (
            Gui::MouseEvent aEvent   //!< Where it entered.
            )
        {
            mInside = true;
            mLast   = aEvent;
            std::printf( "enter    at %4d,%4d\n", aEvent.mPos.mX, aEvent.mPos.mY );
            mWindow->requestUpdate();
        }

        //! Records the pointer leaving.
        void onMouseLeft()
        {
            mInside = false;
            std::printf( "leave\n" );
            mWindow->requestUpdate();
        }

        //! Records a scroll.
        void onWheel
            (
            Gui::WheelEvent aEvent   //!< The rotation.
            )
        {
            mWheelTotal += aEvent.mAngleDeltaY;
            std::printf( "wheel    dx %5d  dy %5d  total %d\n", aEvent.mAngleDeltaX,
                aEvent.mAngleDeltaY, mWheelTotal );
            mWindow->requestUpdate();
        }

        //! Records a finger going down.
        void onTouchDown
            (
            Gui::TouchDownEvent aEvent   //!< The touch.
            )
        {
            ++mTouchCount;
            mTouchX = static_cast<int>( aEvent.mX );
            mTouchY = static_cast<int>( aEvent.mY );
            mTouching = true;
            std::printf( "touch    down id %d at %.1f,%.1f\n", aEvent.mId, aEvent.mX, aEvent.mY );
        }

        //! Records a finger moving.
        void onTouchMotion
            (
            Gui::TouchMotionEvent aEvent   //!< The motion.
            )
        {
            mTouchX = static_cast<int>( aEvent.mX );
            mTouchY = static_cast<int>( aEvent.mY );
        }

        //! Records a finger lifting.
        void onTouchUp
            (
            Gui::TouchUpEvent aEvent   //!< The release.
            )
        {
            mTouching = false;
            std::printf( "touch    up   id %d\n", aEvent.mId );
        }

        //! Redraws once the points reported since the last frame form a consistent set.
        //!
        //! This is the signal to act on, not the individual down and motion events: they describe
        //! an intermediate state that may be internally inconsistent, and frame is what says the
        //! set is complete.
        void onTouchFrame()
        {
            mWindow->requestUpdate();
        }

        //! Rebuilds the buffer at the new size.
        void onResized
            (
            int aWidth,   //!< New width.
            int aHeight   //!< New height.
            )
        {
            std::printf( "resize   %d x %d\n", aWidth, aHeight );

            mWidth  = aWidth;
            mHeight = aHeight;

            if( mBuffer != nullptr )
            {
                wl_buffer_destroy( mBuffer );
                mBuffer = nullptr;
            }
            if( mPixels != nullptr )
            {
                ::munmap( mPixels, mPixelBytes );
                mPixels = nullptr;
            }

            createBuffer();
            mWindow->requestUpdate();
        }

        //! Counts a second of uptime and repaints.
        //!
        //! A QtLikeSignal::Timer sharing the loop with the compositor socket. It keeps ticking
        //! while the pointer floods the connection, which is the point: one poll() services both.
        void onSecond()
        {
            ++mSeconds;
            mWindow->requestUpdate();
        }

        //! @return the bar's position, which is what the animation in main() drives.
        QtLikeSignal::Property<int>& sweepPosition()
        {
            return mSweepPosition;
        }

        //! Paints and presents a frame.
        //!
        //! Nothing here advances the animation. It runs on the library's own clock, and this is
        //! called because the value it moves reported a change -- see the comment beside the
        //! animation in main() for why that direction, and not the other one.
        void onExposed()
        {
            if( mPixels == nullptr || mBuffer == nullptr )
            {
                return;
            }

            std::uint32_t* const pixels = static_cast<std::uint32_t*>( mPixels );
            std::fill( pixels, pixels + ( static_cast<std::size_t>( mWidth ) * mHeight ),
                kBackground );

            // A bar per counter, so the window shows something without a font to draw with.
            drawBar( 0, mPressCount, 0xff78c8ff );
            drawBar( 1, mReleaseCount, 0xff5aa0dc );
            drawBar( 2, mMoveCount / 10, 0xff96a0aa );
            drawBar( 3, mSeconds, 0xffa0e6a0 );

            if( mInside )
            {
                drawCross( mLast.mPos.mX, mLast.mPos.mY, kCrosshair );
            }

            if( Gui::GuiApplication::mouseButtons().any() )
            {
                drawBlock( 16, mHeight - 48, 32, 32, kPressed );
            }

            // The animated bar. Its position is a Property, so nothing here reads the
            // animation -- the renderer reads the value, exactly as it would if a slider had set
            // it.
            drawBlock( ( mSweepPosition.get() * std::max( 0, mWidth - 48 ) ) / 1000, 8, 40, 10,
                kCrosshair );

            if( mTouching )
            {
                drawCross( mTouchX, mTouchY, kTouched );
                drawBlock( 64, mHeight - 48, 32, 32, kTouched );
            }

            // Attach, damage, commit -- the three calls that put a frame on screen, and the ones
            // QtLikeSignalGui deliberately never makes. This is where an EGL renderer would call
            // eglSwapBuffers instead.
            wl_surface_attach( mSurface, mBuffer, 0, 0 );
            wl_surface_damage( mSurface, 0, 0, mWidth, mHeight );
            wl_surface_commit( mSurface );
        }

    private:
        //! Binds wl_shm when the registry announces it.
        static void registryGlobal
            (
            void* aData,
            wl_registry* aRegistry,
            std::uint32_t aName,
            const char* aInterface,
            std::uint32_t aVersion
            )
        {
            static_cast<void>( aVersion );

            Renderer* const self = static_cast<Renderer*>( aData );
            if( std::strcmp( aInterface, wl_shm_interface.name ) == 0 )
            {
                self->mShm = static_cast<wl_shm*>( wl_registry_bind( aRegistry, aName,
                    &wl_shm_interface, 1 ) );
            }
        }

        //! A global went away. This renderer holds none whose loss it could survive anyway.
        static void registryGlobalRemove
            (
            void* aData,
            wl_registry* aRegistry,
            std::uint32_t aName
            )
        {
            static_cast<void>( aData );
            static_cast<void>( aRegistry );
            static_cast<void>( aName );
        }

        //! Makes the shared-memory buffer the surface shows.
        bool createBuffer()
        {
            const int stride = mWidth * 4;
            const std::size_t bytes = static_cast<std::size_t>( stride )
                * static_cast<std::size_t>( mHeight );

            const int fd = createSharedFile( bytes );
            if( fd < 0 )
            {
                qCWarning( gLogDemo )
                    << "could not create a buffer of" << bytes << "bytes; errno" << errno;
                return false;
            }

            void* const pixels = ::mmap( nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                0 );
            if( pixels == MAP_FAILED )
            {
                qCWarning( gLogDemo ) << "mmap() of the buffer failed; errno" << errno;
                ::close( fd );
                return false;
            }

            mPixels     = pixels;
            mPixelBytes = bytes;

            wl_shm_pool* const pool = wl_shm_create_pool( mShm, fd,
                static_cast<std::int32_t>( bytes ) );
            mBuffer = wl_shm_pool_create_buffer( pool, 0, mWidth, mHeight, stride,
                WL_SHM_FORMAT_XRGB8888 );

            // The pool and the descriptor have both done their job: the buffer holds its own
            // reference to the mapping, and keeping either open would only leak.
            wl_shm_pool_destroy( pool );
            ::close( fd );

            return mBuffer != nullptr;
        }

        //! Fills a rectangle, clipped to the buffer.
        void drawBlock
            (
            int aX,                //!< Left edge.
            int aY,                //!< Top edge.
            int aWidth,            //!< Width in pixels.
            int aHeight,           //!< Height in pixels.
            std::uint32_t aColour  //!< XRGB8888 colour.
            )
        {
            std::uint32_t* const pixels = static_cast<std::uint32_t*>( mPixels );

            const int left   = std::max( 0, aX );
            const int top    = std::max( 0, aY );
            const int right  = std::min( mWidth, aX + aWidth );
            const int bottom = std::min( mHeight, aY + aHeight );

            for( int y = top; y < bottom; ++y )
            {
                for( int x = left; x < right; ++x )
                {
                    pixels[( static_cast<std::size_t>( y ) * mWidth ) + x] = aColour;
                }
            }
        }

        //! Draws one counter as a horizontal bar.
        void drawBar
            (
            int aRow,              //!< Which bar, from the top.
            int aValue,            //!< Length in pixels, clamped to the window.
            std::uint32_t aColour  //!< XRGB8888 colour.
            )
        {
            drawBlock( 16, 16 + ( aRow * 24 ), std::min( aValue, mWidth - 32 ), 16, aColour );
        }

        //! Draws a crosshair.
        void drawCross
            (
            int aX,                //!< Centre X.
            int aY,                //!< Centre Y.
            std::uint32_t aColour  //!< XRGB8888 colour.
            )
        {
            drawBlock( aX - 12, aY, 25, 1, aColour );
            drawBlock( aX, aY - 12, 1, 25, aColour );
        }

        static const wl_registry_listener kRegistryListener;

        Gui::Window* mWindow;             //!< The window drawn in. Not owned.
        wl_display* mDisplay;             //!< The connection QtLikeSignalGui opened. Not owned.
        wl_surface* mSurface;             //!< The surface it created. Not owned.
        wl_registry* mRegistry { nullptr };   //!< This renderer's own registry.
        wl_shm* mShm { nullptr };         //!< Shared memory, for the buffer below.
        wl_buffer* mBuffer { nullptr };   //!< What the surface shows.
        void* mPixels { nullptr };        //!< The buffer's mapping.
        std::size_t mPixelBytes { 0 };    //!< Its length.
        int mWidth;                       //!< Buffer width in pixels.
        int mHeight;                      //!< Buffer height in pixels.
        Gui::MouseEvent mLast;            //!< Most recent pointer position.
        bool mInside { false };           //!< Whether the pointer is over the surface.
        bool mTouching { false };         //!< Whether a finger is down.
        int mTouchX { 0 };                //!< Most recent touch X.
        int mTouchY { 0 };                //!< Most recent touch Y.
        int mPressCount { 0 };            //!< Total presses seen.
        int mReleaseCount { 0 };          //!< Total releases seen.
        int mMoveCount { 0 };             //!< Total moves seen.
        int mTouchCount { 0 };            //!< Total touches seen.
        int mWheelTotal { 0 };            //!< Accumulated vertical rotation.
        int mSeconds { 0 };               //!< Seconds since start, from the timer.

        //! Where the animated bar is, from 0 to 1000. Driven by a PropertyAnimation in main().
        QtLikeSignal::Property<int> mSweepPosition { 0 };

    };

    const wl_registry_listener Renderer::kRegistryListener =
    {
        &Renderer::registryGlobal,
        &Renderer::registryGlobalRemove
    };
}

//! Creates the window through the library, wires its signals, and runs the one loop there is.
int main
    (
    int argc,     //!< Argument count.
    char** argv   //!< Argument vector; -p wayland selects this backend.
    )
{
    Gui::GuiApplication app( argc, argv );

    if( !app.hasPlatform() || app.platformType() != Gui::PlatformType::Wayland )
    {
        qCCritical( gLogDemo )
            << "this demo needs the wayland backend; pass -p wayland. The platform is"
            << app.platformName();
        return 1;
    }

    Gui::WindowSettings settings;
    settings.mWidth  = 1280;
    settings.mHeight = 720;
    settings.mTitle  = "QtLikeSignalGui demo -- Wayland";
    settings.mAppId  = "com.example.qtlikesignalgui.demo";

    // Left at zero, so WAYLAND_IVI_ID decides. Set it and this same binary asks for an ivi surface
    // instead of an xdg toplevel, which is how it would run on a head unit.
    settings.mIviId = 0;

    Gui::Window* const window = app.createWindow( settings );
    if( window == nullptr )
    {
        qCWarning( gLogDemo ) << "failed to create the window";
        return 1;
    }

    Renderer renderer( window );
    if( !renderer.initialise() )
    {
        return 1;
    }

    // Direct connections: the compositor's events are dispatched on this thread, inside this
    // thread's dispatch pass, so the slots run there too. Auto would resolve to Direct for the same
    // reason.
    const QtLikeSignal::ConnectionType direct = QtLikeSignal::ConnectionType::Direct;

    QtLikeSignal::Object::connect( window->getMousePressed(), &renderer, &Renderer::onMousePressed,
        direct );
    QtLikeSignal::Object::connect( window->getMouseReleased(), &renderer,
        &Renderer::onMouseReleased, direct );
    QtLikeSignal::Object::connect( window->getMouseMoved(), &renderer, &Renderer::onMouseMoved,
        direct );
    QtLikeSignal::Object::connect( window->getMouseEntered(), &renderer, &Renderer::onMouseEntered,
        direct );
    QtLikeSignal::Object::connect( window->getMouseLeft(), &renderer, &Renderer::onMouseLeft, direct
                                 );
    QtLikeSignal::Object::connect( window->getWheel(), &renderer, &Renderer::onWheel, direct );
    QtLikeSignal::Object::connect( window->getTouchDown(), &renderer, &Renderer::onTouchDown, direct
                                 );
    QtLikeSignal::Object::connect( window->getTouchMotion(), &renderer, &Renderer::onTouchMotion,
        direct );
    QtLikeSignal::Object::connect( window->getTouchUp(), &renderer, &Renderer::onTouchUp, direct );
    QtLikeSignal::Object::connect( window->getTouchFrame(), &renderer, &Renderer::onTouchFrame,
        direct );
    QtLikeSignal::Object::connect( window->getResized(), &renderer, &Renderer::onResized, direct );
    QtLikeSignal::Object::connect( window->getExposed(), &renderer, &Renderer::onExposed, direct );

    QtLikeSignal::Timer second;
    QtLikeSignal::Object::connect( second.getTimeout(), &renderer, &Renderer::onSecond, direct );
    second.start( 1000 );

    // **The animation is advanced by the library's own 16 ms clock, and the repaint follows the
    // value rather than the other way round.** The tempting arrangement is the opposite one --
    // switch the internal clock off with AbstractAnimation::tickExternally(), advance from inside
    // the paint, and ask for another frame while the animation runs. It does not work for a
    // program shaped like this one, in two different ways worth knowing before trying it:
    //
    //   * On immediate delivery, which is what a window has unless it asks otherwise, a paint that
    //     asks for another frame is a loop with nothing throttling it. It repaints thousands of
    //     times a second, and it looks like the window flashing rather than like an animation.
    //   * With Display pacing it deadlocks instead. requestUpdate() arms a wl_surface.frame
    //     callback, and that callback fires only after the surface is committed again -- but this
    //     program commits only inside its paint, and the paint runs only when the callback fires.
    //     Starting an animation from outside a paint therefore arms a callback nothing will ever
    //     trigger. Measured: exactly one frame for the whole run.
    //
    // Driving animation from the frame clock is still the better arrangement for a renderer that
    // presents every frame whatever happens -- a game does -- and tickExternally() exists for it.
    // It is the wrong shape for a program that draws only when something changed, which is what
    // this demo is.

    // Only the end is set. Leaving the start unset is what makes the animation begin from
    // wherever the bar currently is, so clicking again part way through picks it up rather than
    // snapping it back -- which is the whole reason PropertyAnimation reads the property at
    // start() when it was given no start value.
    QtLikeSignal::PropertyAnimation<int> sweepAnimation( renderer.sweepPosition() );
    sweepAnimation.setDuration( 900 );
    sweepAnimation.setEasing( QtLikeSignal::Easing::Cubic_InOut );

    // Started by a click rather than looping forever, so this demo keeps the property it was
    // written to show: at rest it uses no CPU at all. An animation running for ever would repaint
    // sixty times a second whether or not anything was looking.
    QtLikeSignal::Property<int>& bar = renderer.sweepPosition();
    QtLikeSignal::Object::connect( window->getMousePressed(), &renderer,
        [&sweepAnimation, &bar]( Gui::MouseEvent )
        {
            // **The target is chosen from where the bar is, not from where it was last sent.**
            // Toggling the previous end value looks equivalent and is not: the first click would
            // toggle away from the end set at start-up and send the bar to the position it
            // already held, so the first click did nothing and every one after it worked.
            //
            // Only the end is set. setRange() would fix the start as well, and the bar would jump
            // to it before moving; leaving the start unset is what makes PropertyAnimation read
            // the property at start(), so a click part way through picks the bar up where it is.
            sweepAnimation.setEndValue( bar.get() >= 500 ? 0 : 1000 );
            sweepAnimation.start();
        }, direct );

    // Each step of the sweep changes the property, and that is what asks for a repaint. The
    // animation keeps advancing on its own clock whether or not a frame was drawn, so a step that
    // leaves the value where it was simply does not repaint -- it stalls nothing.
    QtLikeSignal::Object::connect( renderer.sweepPosition().getChanged(), &renderer,
        [window]( int )
        {
            window->requestUpdate();
        }, direct );

    window->show();

    // The first frame, which is also what maps the surface: a Wayland surface has no contents until
    // a buffer is attached, so nothing appears on screen until the renderer commits one.
    //
    // Asked for rather than drawn directly. Painting here would queue the commit outside any
    // dispatch pass, and nothing would write it to the socket -- requestUpdate() routes it through
    // the loop, which draws and then flushes.
    window->requestUpdate();

    // Ctrl+C, and the SIGTERM a service manager sends, come back here as an ordinary signal on this
    // thread instead of stopping the process where it stands. That is what lets the compositor
    // connection be torn down in order, so this demo shows the whole shutdown path and not only the
    // close button. Press Ctrl+C two times and the second one stops the process, whatever the first
    // one is still doing.
    QtLikeSignal::SignalWatcher shutdown;
    QtLikeSignal::Object::connect( shutdown.getTriggered(), &renderer, []( int aSignal )
        {
            std::printf( "signal %d received; leaving the loop\n", aSignal );
            QtLikeSignal::CoreApplication::quit();
        }, direct );

    std::printf( "QtLikeSignalGui Wayland demo running.\n" );
    std::printf( "  platform        : %s\n", app.platformName() );
    std::printf( "  surface         : %p on fd %d\n", window->nativeHandle(),
        wl_display_get_fd( static_cast<wl_display*>( window->nativeDisplay() ) ) );
    std::printf( "  event loop      : GuiApplication::exec()\n" );
    std::printf( "  compositor sock : in the dispatcher's poll() set\n" );
    std::printf( "  device pixels   : %.2f per unit, so %d x %d real pixels\n",
        window->devicePixelRatio(),
        static_cast<int>( window->width() * window->devicePixelRatio() ),
        static_cast<int>( window->height() * window->devicePixelRatio() ) );
    std::printf( "  animation       : advanced by the internal 16 ms clock\n" );
    std::printf( "  shutdown        : %s\n",
        shutdown.isWatching() ? "SIGINT and SIGTERM watched" : "not watched" );
    std::printf( "  no wl_display_dispatch loop in this program\n" );

    // Flushed, because the loop below can run for hours and stdout is block-buffered the
    // moment it is redirected to a file -- so without this the status above appears only
    // when the program exits, which is exactly when it stops being useful.
    std::fflush( stdout );

    const int result = app.exec();

    std::printf( "loop finished, exit code %d\n", result );
    return result;
}
