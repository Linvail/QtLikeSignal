// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignalGui::Window -- one on-screen window, and the signals its input arrives on.

#ifndef QT_LIKE_SIGNAL_GUI_WINDOW_HPP
#define QT_LIKE_SIGNAL_GUI_WINDOW_HPP

#include "QtLikeSignal/Object.hpp"
#include "QtLikeSignal/Signal.hpp"

#include "QtLikeSignalGui/InputEvents.hpp"

#include <string>

namespace QtLikeSignalGui
{
    class PlatformIntegration;

    //! A native window that already exists, for the backends that adopt one rather than create it.
    //!
    //! Three fields rather than one void*, because the platforms genuinely differ in what identifies
    //! a window. X11 needs both a connection and a resource id, and the id is an integer rather than
    //! a pointer -- squeezing an XID through a void* would be a cast at every use and a lie in the
    //! type. Each backend documents which fields it reads and ignores the rest.
    struct NativeWindow
    {
        //! The connection the window lives on: an X11 Display*, or a Wayland wl_display*.
        //!
        //! Required on X11 and Wayland, unused on Win32. On X11 this must be the very connection the
        //! window was created on: events for a window are delivered to the client that created it,
        //! so a second connection to the same server would see nothing.
        void* mDisplay { nullptr };

        //! The window itself where it is pointer-shaped: an HWND on Win32, a wl_surface* on Wayland.
        void* mSurface { nullptr };

        //! The window itself where it is an integer: the X11 Window id. Zero on the other platforms.
        unsigned long mWindowId { 0 };

        //! Width of the drawable in pixels, for the backends that cannot ask.
        //!
        //! **Required on DRM**, where there is no window system to query -- the scanout size is
        //! known only to the library that set the mode, and this backend needs it to clamp the
        //! pointer and to place touch points. Ignored on X11, where XGetWindowAttributes is the
        //! authority and answering from here could only disagree with it.
        int mWidth { 0 };

        //! Height of the drawable in pixels. See mWidth.
        int mHeight { 0 };
    };

    //! What a window should look like when it is first created.
    //!
    //! Only the three settings that cannot sensibly be changed after the fact without the caller
    //! noticing. Everything else -- title, visibility -- has a setter on Window instead, because a
    //! creation-time-only knob for something that is adjustable later is an API that has to be
    //! explained rather than read.
    struct WindowSettings
    {
        //! Width of the **client area** in pixels: what can be drawn on, not the outer frame.
        //!
        //! A caller asking for 1280 means 1280 pixels to render into -- the surface WGL or EGL will
        //! present to, with no border, caption or menu bar counted in it. The backend grows the
        //! rectangle by whatever the frame needs.
        //!
        //! A window created here has no menu bar. Attaching one afterwards takes its height out of
        //! the client area, so use Window::setMenu() rather than the raw SetMenu(): it puts the
        //! client area back to the size asked for. See that function.
        int mWidth { 1280 };

        //! Height of the client area in pixels.
        int mHeight { 800 };

        //! Caption text, in UTF-8. Converted to whatever the platform wants by the backend.
        std::string mTitle { "QtLikeSignal" };

        //! The X11 VisualID to create the window with, or 0 for the screen's default. **X11 only.**
        //!
        //! This is X11's answer to the pixel format, and it has the same one-shot character: a
        //! window's visual is fixed at creation and glXCreateWindow or eglCreateWindowSurface fails
        //! with BadMatch if it does not match the config the context was made for. So the ordering
        //! for a GL program is: get the connection from GuiApplication::nativeDisplay(), let the
        //! library choose its FBConfig or EGLConfig on it, ask that config for its visual, and pass
        //! the id here.
        //!
        //! @code
        //!   Display* display = static_cast<Display*>( app.nativeDisplay() );
        //!   GLXFBConfig config = myLibrary.chooseFbConfig( display );
        //!   XVisualInfo* visual = glXGetVisualFromFBConfig( display, config );
        //!
        //!   WindowSettings settings;
        //!   settings.mVisualId = visual->visualid;
        //! @endcode
        //!
        //! Zero is right for anything that does not care -- software rendering, or a GL config the
        //! default visual already satisfies, which is most of them on a modern compositing desktop.
        //! Ignored on every other platform.
        unsigned long mVisualId { 0 };

        //! The application id to report to the compositor. **Wayland only.**
        //!
        //! A reverse-DNS name such as "com.example.myapp". Compositors use it to match a window to
        //! its desktop entry, which is what decides the icon and how the window is grouped in a
        //! task switcher. Empty means none is set, which is legal and merely leaves the window
        //! unmatched. Ignored on every other platform.
        std::string mAppId;

        //! The ivi-shell surface id to create the window with, or 0 for xdg-shell. **Wayland only.**
        //!
        //! ivi-shell is the automotive shell: there is no window manager, no decoration and no
        //! close button, and a surface is identified to the controller by this number rather than
        //! by a title. Non-zero selects it, provided the compositor offers ivi_application at all;
        //! zero, or a compositor without it, falls back to xdg-shell.
        //!
        //! Left at zero, the WAYLAND_IVI_ID environment variable is consulted instead, which is how
        //! the same binary runs on a desktop and on a head unit without being rebuilt.
        int mIviId { 0 };
    };

    //! One on-screen window: its native handle, its size, and the input that lands on it.
    //!
    //! This is the portable half of the pair Qt splits into QWindow and QPlatformWindow. Everything
    //! here is window-system independent; the PlatformIntegration that created it holds the native
    //! side and performs every operation this class forwards to it.
    //!
    //! **Windows are created through GuiApplication::createWindow(), never directly.** The
    //! constructor is private for that reason: a Window without a live native handle behind it has
    //! nothing to report and nothing to draw on, and there is no state in which one legitimately
    //! exists.
    //!
    //! **Lifetime.** Created windows are children of the GuiApplication, so they are destroyed with
    //! it if the caller has not destroyed them first. Destroying a Window destroys the native window
    //! with it -- so on Win32, any WGL context made current against nativeHandle() must be torn down
    //! **before** the Window goes away, or the context outlives the HWND it is bound to.
    //!
    //! **Thread affinity.** A Window belongs to the thread that created it, which is the thread
    //! running the loop. Every method here must be called from that thread: a native window is owned
    //! by its creating thread on Win32, and the connection is used from one thread on X11 and
    //! Wayland. The signals may of course be connected to slots living on other threads, which is
    //! what ConnectionType::Queued is for.
    class Window : public QtLikeSignal::Object
    {
    public:
        virtual ~Window() override;

        Window
            (
            const Window&
            ) = delete;

        Window& operator=
            (
            const Window&
            ) = delete;

        //! Gets the pointer-shaped native handle: the HWND on Win32, the wl_surface* on Wayland.
        //!
        //! **Null on X11**, where a window is an integer resource id rather than a pointer; use
        //! nativeWindowId() and nativeDisplay() there.
        //!
        //! void* rather than the real type so this header does not pull <windows.h> or <X11/Xlib.h>,
        //! and their macros, into every translation unit that includes it -- the same choice
        //! EventDispatcherWin32 makes for its own message window. Cast it back at the call site.
        //!
        //! Valid from the moment the window is created or adopted until this Window is destroyed, and
        //! stable for that whole span: the handle is never recreated underneath a caller holding it.
        //!
        //! This is what an external OpenGL library is handed to initialise WGL against.
        void* nativeHandle() const
        {
            return mNative.mSurface;
        }

        //! Gets the connection the window lives on: the X11 Display*, or the Wayland wl_display*.
        //!
        //! Null on Win32, which has no such thing.
        void* nativeDisplay() const
        {
            return mNative.mDisplay;
        }

        //! Gets the X11 Window id. Zero on every other platform.
        unsigned long nativeWindowId() const
        {
            return mNative.mWindowId;
        }

        //! Gets the current client-area width in pixels, as of the last resize reported.
        int width() const
        {
            return mWidth;
        }

        //! Gets the current client-area height in pixels, as of the last resize reported.
        int height() const
        {
            return mHeight;
        }

        //! Returns true if the window has been shown and not hidden since.
        //!
        //! Tracks what this library asked for, not what the user or the window manager did with it
        //! afterwards -- a minimised or fully occluded window still reports true.
        bool isVisible() const
        {
            return mVisible;
        }

        void show();

        void hide();

        void setTitle
            (
            const std::string& aTitle
            );

        void setClientSize
            (
            int aWidth,
            int aHeight
            );

        void setMenu
            (
            void* aMenuHandle
            );

        void requestUpdate();

        //! Emitted when a mouse button goes down over the window.
        QtLikeSignal::SignalView<MouseEvent>& getMousePressed() const
        {
            return mMousePressed.view();
        }

        //! Emitted when a mouse button comes up.
        //!
        //! Also delivered when the release happens outside the window, as long as the press was
        //! inside it: the backend holds the mouse for the duration of a drag so the matching release
        //! is never lost. MouseEvent::mPos may therefore be outside the client area.
        QtLikeSignal::SignalView<MouseEvent>& getMouseReleased() const
        {
            return mMouseReleased.view();
        }

        //! Emitted when the mouse moves over the window, or anywhere during a drag.
        //!
        //! MouseEvent::mButton is None; MouseEvent::mButtons says whether this is a drag.
        QtLikeSignal::SignalView<MouseEvent>& getMouseMoved() const
        {
            return mMouseMoved.view();
        }

        //! Emitted when the mouse wheel turns over the window.
        QtLikeSignal::SignalView<WheelEvent>& getWheel() const
        {
            return mWheel.view();
        }

        //! Emitted when the mouse enters the client area, with the position it entered at.
        QtLikeSignal::SignalView<MouseEvent>& getMouseEntered() const
        {
            return mMouseEntered.view();
        }

        //! Emitted when the mouse leaves the client area. Carries no position: the platforms that
        //! report a leave at all do not agree on where it happened.
        QtLikeSignal::SignalView<>& getMouseLeft() const
        {
            return mMouseLeft.view();
        }

        //! Emitted when a key goes down, including every auto-repeat while it is held.
        //!
        //! Delivered to the window with keyboard focus. A key held down produces one event with
        //! KeyEvent::mAutoRepeat false and then a stream with it true, at whatever rate the
        //! platform repeats at -- this library does not synthesise repeats of its own, so a backend
        //! whose platform does not repeat simply does not send any.
        //!
        //! **For text, read KeyEvent::mText rather than the key.** See that field.
        QtLikeSignal::SignalView<KeyEvent>& getKeyPressed() const
        {
            return mKeyPressed.view();
        }

        //! Emitted when a key comes up. Never auto-repeats.
        QtLikeSignal::SignalView<KeyEvent>& getKeyReleased() const
        {
            return mKeyReleased.view();
        }

        //! Emitted when a finger touches down. **Wayland only** -- see the note below.
        //!
        //! The five touch signals mirror wl_touch_listener one for one, down to which fields each
        //! event carries. They are declared on every platform so application code compiles
        //! unchanged everywhere, but only the Wayland path ever emits them: the Win32 backend does
        //! not translate WM_TOUCH and the X11 backend does not use XInput2. That is the documented
        //! reach of the feature, not a stub waiting to be filled in.
        //!
        //! Act on getTouchFrame(), not on this. See that signal for why.
        QtLikeSignal::SignalView<TouchDownEvent>& getTouchDown() const
        {
            return mTouchDown.view();
        }

        //! Emitted when a finger lifts. Wayland only. Carries no position; see TouchUpEvent::mId.
        QtLikeSignal::SignalView<TouchUpEvent>& getTouchUp() const
        {
            return mTouchUp.view();
        }

        //! Emitted when a finger moves. Wayland only.
        QtLikeSignal::SignalView<TouchMotionEvent>& getTouchMotion() const
        {
            return mTouchMotion.view();
        }

        //! Emitted when the touch points just reported form a consistent set. Wayland only.
        //!
        //! **This is the signal to act on.** down, up and motion are deltas describing an
        //! intermediate state that may be internally inconsistent -- two fingers moving at once
        //! arrive as two separate motions, and rendering between them shows a frame that never
        //! happened. frame says "everything reported since the last frame belongs together, act
        //! now". Treating each down as immediately actionable is the standard way multi-touch
        //! handling goes wrong.
        //!
        //! Delivered to the window holding touch focus: the one that received the most recent down.
        QtLikeSignal::SignalView<>& getTouchFrame() const
        {
            return mTouchFrame.view();
        }

        //! Emitted when the compositor takes the touch sequence away, e.g. because a system gesture
        //! claimed it. Wayland only. Every point in flight is void; discard the whole gesture.
        QtLikeSignal::SignalView<>& getTouchCancel() const
        {
            return mTouchCancel.view();
        }

        //! Emitted when the client area changes size, with the new width and height in pixels.
        //!
        //! An OpenGL renderer resizes its viewport and any size-dependent buffers here.
        QtLikeSignal::SignalView<int, int>& getResized() const
        {
            return mResized.view();
        }

        //! Emitted when the window needs redrawing.
        //!
        //! Carries nothing: the drawable belongs to whatever created the GL context, and this
        //! library never has one to hand over. Render and present in the slot.
        QtLikeSignal::SignalView<>& getExposed() const
        {
            return mExposed.view();
        }

        //! Emitted when the user asks to close the window -- the close box, or Alt+F4.
        //!
        //! **The window is not closed or destroyed by this.** It is a request, and what happens next
        //! is the application's decision. If GuiApplication::quitOnLastWindowClosed() is set, which
        //! it is by default, the loop is asked to quit right after this signal returns.
        //!
        //! There is no way to veto from the slot. Signals carry no result, so a veto would need a
        //! mutable event object this library deliberately does not have; to keep a window open on
        //! close, clear quitOnLastWindowClosed() and drive the shutdown from the slot instead.
        QtLikeSignal::SignalView<>& getCloseRequested() const
        {
            return mCloseRequested.view();
        }

        //! Emitted when the window gains (true) or loses (false) keyboard focus.
        QtLikeSignal::SignalView<bool>& getFocusChanged() const
        {
            return mFocusChanged.view();
        }

        //! Emitted when a menu item is chosen, with the command id it was appended with.
        //!
        //! **Win32 only**, and deliberately so: it is the other half of setMenu(), which is itself
        //! Win32 only because Wayland has no server-side menus and X11 draws them in the client.
        //! A menu attached with setMenu() would otherwise be a menu nothing could react to.
        //!
        //! There is no menu model here, no actions and no shortcuts -- the id is whatever was
        //! passed to AppendMenu(), passed straight back. That is the whole of the feature, and
        //! matching it to a command is the application's.
        QtLikeSignal::SignalView<int>& getMenuCommand() const
        {
            return mMenuCommand.view();
        }

    private:
        Window
            (
            PlatformIntegration* aIntegration,
            const NativeWindow& aNative,
            int aWidth,
            int aHeight
            );

        //! Returns true while there is still a native window behind this object.
        //!
        //! False once the backend has released it, which happens during destruction and is the state
        //! every operation below checks for before touching the window system.
        bool hasNative() const
        {
            return mNative.mSurface != nullptr || mNative.mWindowId != 0;
        }

        //! The backend that created or adopted this window and performs every native operation on it.
        //!
        //! Not owned: the GuiApplication owns the integration, and it outlives every window it
        //! made. Never null while this object exists.
        PlatformIntegration* mIntegration;

        //! The native window, or all-zero once the backend has released it.
        NativeWindow mNative;

        int mWidth;              //!< Client-area width in pixels.
        int mHeight;             //!< Client-area height in pixels.
        bool mVisible { false }; //!< Whether show() has been called more recently than hide().

        QtLikeSignal::Signal<MouseEvent> mMousePressed;        //!< See getMousePressed().
        QtLikeSignal::Signal<MouseEvent> mMouseReleased;       //!< See getMouseReleased().
        QtLikeSignal::Signal<MouseEvent> mMouseMoved;          //!< See getMouseMoved().
        QtLikeSignal::Signal<MouseEvent> mMouseEntered;        //!< See getMouseEntered().
        QtLikeSignal::Signal<> mMouseLeft;                     //!< See getMouseLeft().
        QtLikeSignal::Signal<WheelEvent> mWheel;               //!< See getWheel().
        QtLikeSignal::Signal<KeyEvent> mKeyPressed;            //!< See getKeyPressed().
        QtLikeSignal::Signal<KeyEvent> mKeyReleased;           //!< See getKeyReleased().
        QtLikeSignal::Signal<TouchDownEvent> mTouchDown;       //!< See getTouchDown().
        QtLikeSignal::Signal<TouchUpEvent> mTouchUp;           //!< See getTouchUp().
        QtLikeSignal::Signal<TouchMotionEvent> mTouchMotion;   //!< See getTouchMotion().
        QtLikeSignal::Signal<> mTouchFrame;                    //!< See getTouchFrame().
        QtLikeSignal::Signal<> mTouchCancel;                   //!< See getTouchCancel().
        QtLikeSignal::Signal<int, int> mResized;               //!< See getResized().
        QtLikeSignal::Signal<> mExposed;                       //!< See getExposed().
        QtLikeSignal::Signal<> mCloseRequested;                //!< See getCloseRequested().
        QtLikeSignal::Signal<bool> mFocusChanged;              //!< See getFocusChanged().
        QtLikeSignal::Signal<int> mMenuCommand;                //!< See getMenuCommand().

        //! Constructs windows and writes back the state only the native side knows.
        friend class PlatformIntegration;

        //! Emits every signal above. The one path by which input reaches a Window.
        friend class WindowSystemInterface;
    };
}

#endif // QT_LIKE_SIGNAL_GUI_WINDOW_HPP
