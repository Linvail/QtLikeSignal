// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignalGui::PlatformIntegrationDrm -- the DRM/KMS backend, with input from libinput.

#ifndef QT_LIKE_SIGNAL_GUI_PLATFORMINTEGRATIONDRM_HPP
#define QT_LIKE_SIGNAL_GUI_PLATFORMINTEGRATIONDRM_HPP

#include "QtLikeSignalGui/PlatformIntegration.hpp"

#include <memory>

namespace QtLikeSignalGui
{
    //! The DRM/KMS backend: no window system at all, and input straight off the evdev devices.
    //!
    //! **There is no window system here, and that changes what a backend is for.** On DRM the
    //! external library owns the card, the mode and the GBM/EGL surface, and scans out directly.
    //! Nothing arranges windows, nothing draws a pointer, and nothing hands out input -- so this
    //! backend has no window to manage and exactly one job: read the input devices and turn what
    //! they say into the same signals X11 and Win32 produce.
    //!
    //! **libinput is what makes that one job tractable.** The kernel's evdev interface reports
    //! millimetre-scale tablet ranges, per-slot multitouch protocol A/B differences, and mouse
    //! buttons as raw key codes; libinput is the layer that already knows how to read all of it and
    //! is what every Wayland compositor uses. It hands out a single file descriptor, which joins the
    //! loop exactly the way the X11 connection does:
    //!
    //! @code
    //!   poll( [ eventfd, libinput fd ], timeout-until-next-timer )
    //!          ^^^^^^^^  ^^^^^^^^^^^^   ^^^^^^^^^^^^^^^^^^^^^^^^^
    //!          our own    the input     the timers
    //!           events     devices
    //! @endcode
    //!
    //! **The pointer position is this backend's own invention, and it has to be.** libinput reports
    //! a mouse as a *relative* movement, because on real hardware that is what a mouse produces --
    //! there is no screen, no cursor and no clamping until somebody decides there is. With no window
    //! system to decide, this backend keeps the position itself and clamps it to the drawable, which
    //! is why NativeWindow::mWidth and mHeight are required here and ignored on X11. Touchscreens
    //! and tablets report absolute positions and are transformed into the same space.
    //!
    //! **Access to the devices is the practical obstacle.** /dev/input/event* is not world readable:
    //! a program using this backend runs as root, or belongs to the `input` group, or is handed
    //! descriptors by logind or seatd. What is opened is decided as follows:
    //!
    //!   - `QTLIKESIGNAL_INPUT_DEVICES`, a colon-separated list of device paths, opens exactly those
    //!     through libinput's path interface. This is the escape hatch for a container, a test rig,
    //!     or any system where udev is not running.
    //!   - Otherwise udev enumerates the seat named by `QTLIKESIGNAL_SEAT`, defaulting to `seat0`, which
    //!     is what an ordinary machine wants.
    //!
    //! **Touch is fully supported here**, unlike X11 and Win32: libinput reports down, up, motion,
    //! frame and cancel, which is the same vocabulary wl_touch uses and therefore the same five
    //! signals Window already declares.
    //!
    //! **Keyboard arrives here too, as key identities without text.** libinput reports evdev codes,
    //! which name a place on the keyboard; turning one into a character needs the user's keymap and
    //! therefore xkbcommon, which this library does not depend on. KeyEvent::mText is left empty
    //! and KeyEvent::mKey is filled in -- see handleKeyboardKey().
    //!
    //! Used only from the thread running the loop, like the descriptor it reads.
    class PlatformIntegrationDrm : public PlatformIntegration
    {
    public:
        PlatformIntegrationDrm();

        virtual ~PlatformIntegrationDrm() override;

        virtual PlatformType type() const override;

        virtual bool canCreateWindows() const override;

        virtual bool canAdoptWindows() const override;

        virtual Window* adoptWindow
            (
            const NativeWindow& aNative
            ) override;

        virtual void releaseNativeWindow
            (
            Window* aWindow
            ) override;

        virtual void setWindowTitle
            (
            Window* aWindow,
            const std::string& aTitle
            ) override;

        virtual void setWindowVisible
            (
            Window* aWindow,
            bool aVisible
            ) override;

        virtual void setClientSize
            (
            Window* aWindow,
            int aWidth,
            int aHeight
            ) override;

        virtual void requestUpdate
            (
            Window* aWindow
            ) override;

    private:
        bool openInput();

        void closeInput();

        void pumpInput
            (
            short aEvents
            );

        void dispatchNativeEvent
            (
            void* aEvent
            );

        void handlePointerMotion
            (
            void* aEvent,
            bool aAbsolute
            );

        void handlePointerButton
            (
            void* aEvent
            );

        void handlePointerScroll
            (
            void* aEvent
            );

        void handleKeyboardKey
            (
            void* aEvent
            );

        void handleTouch
            (
            void* aEvent,
            int aType
            );

        void movePointerTo
            (
            double aX,
            double aY
            );

        //! The libinput context, as a struct libinput*. Null until the first window is adopted.
        void* mInput { nullptr };

        //! The udev context, as a struct udev*, when the udev interface is in use. Null otherwise.
        void* mUdev { nullptr };

        //! libinput's descriptor while it is registered with the dispatcher, else -1.
        int mInputFd { -1 };

        //! The one adopted surface, or null. Not owned; the GuiApplication owns it.
        //!
        //! One, not a list: a DRM scanout is a single surface covering the whole display, so there
        //! is no second window for an event to belong to and no routing question to answer.
        Window* mWindow { nullptr };

        //! Where the pointer is, in drawable coordinates. Kept here because nothing else keeps it.
        double mPointerX { 0.0 };

        //! See mPointerX.
        double mPointerY { 0.0 };

        //! Every mouse button currently held.
        //!
        //! Accumulated from the press and release events, because libinput reports the change rather
        //! than the resulting state -- the opposite of what MouseEvent::mButtons has to carry.
        MouseButtons mButtons;

        //! Modifiers held, accumulated from the modifier keys going down and up.
        //!
        //! libinput reports which key changed, not what is held, and this backend takes no
        //! xkbcommon dependency -- so the set is built here, the way KeyTranslation.hpp describes.
        KeyModifiers mModifiers;

        //! True while a repaint has been posted to the loop and not yet run. See requestUpdate().
        bool mUpdatePending { false };

        //! Kept alive exactly as long as this backend is, so a posted repaint can ask whether it is.
        //!
        //! requestUpdate() puts a task on the loop that touches this object when it runs, and the
        //! two are not otherwise tied together: the task outlives the backend if the application is
        //! torn down between the post and the pass that would have run it. The task holds a weak
        //! reference to this and does nothing when it cannot lock it, which is the same trick
        //! Object uses for a queued call to a receiver that may already be gone.
        //!
        //! shared_ptr<int> rather than shared_ptr<bool> or a flag: nothing reads the value, only
        //! whether it still exists.
        std::shared_ptr<int> mLifeToken { std::make_shared<int>( 0 ) };
    };
}

#endif // QT_LIKE_SIGNAL_GUI_PLATFORMINTEGRATIONDRM_HPP
