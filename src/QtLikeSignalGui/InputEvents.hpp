// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! The input events QtLikeSignalGui delivers: mouse, wheel and touch, as plain data.
//!
//! Every type here is deliberately free of platform types. A backend translates a WM_LBUTTONDOWN,
//! an XButtonPressedEvent or a wl_touch down into one of these, so everything downstream is
//! ordinary C++ that knows nothing about any window system. That is the same split Qt makes between
//! QWindowSystemInterface and QMouseEvent.

#ifndef QT_LIKE_SIGNAL_GUI_INPUTEVENTS_HPP
#define QT_LIKE_SIGNAL_GUI_INPUTEVENTS_HPP

namespace QtLikeSignalGui
{
    //! A position in whole pixels.
    //!
    //! Integer rather than floating point: all three window systems report mouse positions in whole
    //! pixels, and a double would only invite the impression that sub-pixel information exists.
    //! Touch is the exception and carries its own doubles; see TouchDownEvent.
    struct Point
    {
        int mX { 0 };   //!< Pixels from the left edge.
        int mY { 0 };   //!< Pixels from the top edge.
    };

    //! One mouse button.
    //!
    //! Powers of two so MouseButtons can hold a set of them. Extra1/Extra2 are the two side buttons
    //! most mice have; Windows calls them XBUTTON1 and XBUTTON2, X11 numbers them 8 and 9, and
    //! neither name would travel, so they are named for what they are.
    enum class MouseButton : unsigned int
    {
        None   = 0x00,   //!< No button. What a move reports for the button that changed.
        Left   = 0x01,   //!< Left button.
        Middle = 0x02,   //!< Middle button, usually the wheel pressed.
        Right  = 0x04,   //!< Right button.
        Extra1 = 0x08,   //!< First side button (Windows XBUTTON1, X11 button 8).
        Extra2 = 0x10    //!< Second side button (Windows XBUTTON2, X11 button 9).
    };

    //! A set of mouse buttons: which ones are held down.
    //!
    //! A small class rather than a raw unsigned or bare arithmetic on the enum, so that "which
    //! buttons are down" cannot be silently confused with "which button changed" -- they are
    //! different questions and both appear on MouseEvent. Qt draws the same distinction with
    //! QFlags, and for the same reason: a drag is a changed button of None with a non-empty set,
    //! and code that mixes the two reports every drag as an idle move.
    class MouseButtons
    {
    public:
        //! Constructs an empty set: no buttons held.
        MouseButtons() = default;

        //! Constructs a set holding exactly @p aButton.
        //!
        //! Implicit, so a single button may be passed wherever a set is expected.
        MouseButtons
            (
            MouseButton aButton   //!< The one button in the set; None yields an empty set.
            )
            : mBits( static_cast<unsigned int>( aButton ) )
        {
        }

        //! Returns true if @p aButton is in the set. Always false for None.
        bool test
            (
            MouseButton aButton   //!< The button to look for.
            ) const
        {
            const unsigned int bit = static_cast<unsigned int>( aButton );
            return bit != 0 && ( mBits & bit ) == bit;
        }

        //! Returns true if any button at all is held.
        bool any() const
        {
            return mBits != 0;
        }

        //! Gets the raw bits, for logging or for storing the set compactly.
        unsigned int bits() const
        {
            return mBits;
        }

        //! Adds @p aButton to the set.
        MouseButtons& operator|=
            (
            MouseButton aButton   //!< The button to add.
            )
        {
            mBits |= static_cast<unsigned int>( aButton );
            return *this;
        }

        //! Takes @p aButton out of the set, if it was in it.
        //!
        //! Needed because the two window systems disagree about what a release message carries. Win32
        //! reports the state *after* the change, so a release already excludes the button; X11
        //! reports the state just *prior* to the event, so a ButtonRelease still has it set and the
        //! backend has to take it out to produce the same answer. Without this the button would look
        //! permanently held after the first click on X11.
        MouseButtons& remove
            (
            MouseButton aButton   //!< The button to take out.
            )
        {
            mBits &= ~static_cast<unsigned int>( aButton );
            return *this;
        }

        //! Returns the union of a set and one more button.
        friend MouseButtons operator|
            (
            MouseButtons aLeft,
            MouseButton aRight
            )
        {
            aLeft |= aRight;
            return aLeft;
        }

        //! Returns true if both sets hold exactly the same buttons.
        friend bool operator==
            (
            MouseButtons aLeft,
            MouseButtons aRight
            )
        {
            return aLeft.mBits == aRight.mBits;
        }

        //! Returns true if the sets differ.
        friend bool operator!=
            (
            MouseButtons aLeft,
            MouseButtons aRight
            )
        {
            return aLeft.mBits != aRight.mBits;
        }

    private:
        unsigned int mBits { 0 };   //!< One bit per MouseButton value.
    };

    //! One mouse press, release, move, enter or leave.
    struct MouseEvent
    {
        //! Position in client coordinates: pixels from the top-left of the drawable area.
        Point mPos;

        //! Position in screen coordinates.
        //!
        //! Carried alongside the local one because a drag that leaves the window still reports
        //! meaningful global coordinates while the local ones go negative or past the far edge.
        Point mGlobalPos;

        //! The button whose state just changed. None for a move, an enter or a leave.
        MouseButton mButton { MouseButton::None };

        //! Every button held down at the moment the event was generated -- the changed one included
        //! for a press and excluded for a release. This is what tells a move apart from a drag.
        MouseButtons mButtons;

        //! When the window system generated the event, in milliseconds on an unspecified origin.
        //!
        //! Only differences between two timestamps mean anything. The origin is whatever the
        //! platform uses (time since boot on Windows, the server's clock on X11), so it is not
        //! comparable across platforms nor with any wall clock.
        unsigned long mTimestampMs { 0 };
    };

    //! One turn of a mouse wheel.
    struct WheelEvent
    {
        Point mPos;         //!< Position in client coordinates.
        Point mGlobalPos;   //!< Position in screen coordinates.

        //! Horizontal rotation in eighths of a degree; positive scrolls to the right.
        //!
        //! The unit is Windows' WHEEL_DELTA, so one ordinary notch is 120. Reported rather than
        //! pre-divided into notches because high-resolution wheels and touchpads send fractions of
        //! a notch, and rounding those to zero here would discard the whole gesture.
        int mAngleDeltaX { 0 };

        //! Vertical rotation in eighths of a degree; positive scrolls away from the user.
        int mAngleDeltaY { 0 };

        MouseButtons mButtons;              //!< Buttons held while the wheel turned.
        unsigned long mTimestampMs { 0 };   //!< As MouseEvent::mTimestampMs.
    };

    //! One keyboard modifier.
    //!
    //! Powers of two so KeyModifiers can hold a set of them, exactly as MouseButton does for
    //! MouseButtons. Meta is the Windows key on a PC keyboard and Command on a Mac one; it is named
    //! for the role rather than for either vendor's key cap, because neither name travels.
    enum class KeyModifier : unsigned int
    {
        None     = 0x00,   //!< No modifier held.
        Shift    = 0x01,   //!< Either Shift.
        Control  = 0x02,   //!< Either Control.
        Alt      = 0x04,   //!< Either Alt; the right one is AltGr on many layouts.
        Meta     = 0x08,   //!< The Windows/Command key.
        Keypad   = 0x10,   //!< The key came from the numeric keypad.
        CapsLock = 0x20,   //!< Caps Lock is latched on. A state, not a key being held.
        NumLock  = 0x40    //!< Num Lock is latched on. A state, not a key being held.
    };

    //! A set of keyboard modifiers: which ones were active when a key changed.
    //!
    //! The same shape as MouseButtons and for the same reason -- a set is a different thing from
    //! the one key that changed, and giving them different types is what stops the two being
    //! confused.
    class KeyModifiers
    {
    public:
        //! Constructs an empty set: no modifiers active.
        KeyModifiers() = default;

        //! Constructs a set holding exactly @p aModifier.
        //!
        //! Implicit, so a single modifier may be passed wherever a set is expected.
        KeyModifiers
            (
            KeyModifier aModifier   //!< The one modifier in the set; None yields an empty set.
            )
            : mBits( static_cast<unsigned int>( aModifier ) )
        {
        }

        //! Returns true if @p aModifier is in the set. Always false for None.
        bool test
            (
            KeyModifier aModifier   //!< The modifier to look for.
            ) const
        {
            const unsigned int bit = static_cast<unsigned int>( aModifier );
            return bit != 0 && ( mBits & bit ) == bit;
        }

        //! Returns true if any modifier at all is active.
        bool any() const
        {
            return mBits != 0;
        }

        //! Gets the raw bits, for logging or for storing the set compactly.
        unsigned int bits() const
        {
            return mBits;
        }

        //! Adds @p aModifier to the set.
        KeyModifiers& operator|=
            (
            KeyModifier aModifier   //!< The modifier to add.
            )
        {
            mBits |= static_cast<unsigned int>( aModifier );
            return *this;
        }

        //! Takes @p aModifier out of the set, if it was in it.
        KeyModifiers& remove
            (
            KeyModifier aModifier   //!< The modifier to take out.
            )
        {
            mBits &= ~static_cast<unsigned int>( aModifier );
            return *this;
        }

        //! Returns the union of a set and one more modifier.
        friend KeyModifiers operator|
            (
            KeyModifiers aLeft,
            KeyModifier aRight
            )
        {
            KeyModifiers result = aLeft;
            result |= aRight;
            return result;
        }

        //! Returns true if the sets hold the same modifiers.
        friend bool operator==
            (
            KeyModifiers aLeft,
            KeyModifiers aRight
            )
        {
            return aLeft.mBits == aRight.mBits;
        }

        //! Returns true if the sets differ.
        friend bool operator!=
            (
            KeyModifiers aLeft,
            KeyModifiers aRight
            )
        {
            return aLeft.mBits != aRight.mBits;
        }

    private:
        unsigned int mBits { 0 };   //!< One bit per KeyModifier value.
    };

    //! A key, identified by what it means rather than by what any one platform calls it.
    //!
    //! **The numbering is Qt's**, deliberately: printable ASCII keys take their own ASCII code, so
    //! Key::A is 0x41 and Key::Space is 0x20, and everything else starts at 0x01000000. Anyone who
    //! has used Qt::Key already knows the scheme, and a program being ported from Qt can compare
    //! values directly instead of translating through a table it has no way to check.
    //!
    //! **Unshifted identities only.** Key::Digit1 is the "1" key whether or not Shift makes it "!",
    //! because a key is a place on a keyboard and the character it produces is a separate question
    //! -- answered by KeyEvent::mText, which is what text input should read.
    //!
    //! Deliberately not exhaustive. Media keys, browser keys, international keys and the rest of
    //! the long tail report Key::Unknown, and KeyEvent::mNativeCode still carries what the platform
    //! said, so nothing is lost for a program that needs one of them.
    enum class Key : unsigned int
    {
        Unknown = 0,   //!< No portable name for this key. See KeyEvent::mNativeCode.

        Space        = 0x20,
        Apostrophe   = 0x27,
        Comma        = 0x2c,
        Minus        = 0x2d,
        Period       = 0x2e,
        Slash        = 0x2f,

        Digit0 = 0x30, Digit1, Digit2, Digit3, Digit4,
        Digit5, Digit6, Digit7, Digit8, Digit9,

        Semicolon    = 0x3b,
        Equal        = 0x3d,

        A = 0x41, B, C, D, E, F, G, H, I, J, K, L, M,
        N, O, P, Q, R, S, T, U, V, W, X, Y, Z,

        BracketLeft  = 0x5b,
        Backslash    = 0x5c,
        BracketRight = 0x5d,
        Grave        = 0x60,

        Escape    = 0x01000000,
        Tab,
        Backspace,
        Return,      //!< The main Return/Enter key.
        Enter,       //!< The keypad's Enter, which every platform reports separately.
        Insert,
        Delete,
        Pause,
        Print,

        Home = 0x01000010,
        End,
        Left,
        Up,
        Right,
        Down,
        PageUp,
        PageDown,

        Shift = 0x01000020,
        Control,
        Meta,
        Alt,
        CapsLock,
        NumLock,
        ScrollLock,

        F1 = 0x01000030, F2, F3, F4, F5, F6,
        F7, F8, F9, F10, F11, F12,

        Menu = 0x01000055
    };

    //! One key going down or coming up.
    struct KeyEvent
    {
        //! Which key it was, or Key::Unknown if this library has no name for it.
        Key mKey { Key::Unknown };

        //! Every modifier active when the key changed.
        //!
        //! For a modifier key's own press this *includes* the modifier being pressed, and for its
        //! release it excludes it -- the same before/after rule MouseEvent::mButtons follows, so
        //! the two read alike.
        KeyModifiers mModifiers;

        //! The text this keystroke produces, UTF-8 and NUL-terminated. Empty for a non-printing key.
        //!
        //! **Read this for text input, never mKey.** Which character a key produces depends on the
        //! layout, the modifiers and any dead key before it, and reconstructing that from a key
        //! identity is how a program ends up working only on the keyboard its author owned.
        //!
        //! A fixed array rather than a std::string, because every other type in this file is plain
        //! data a backend can fill without allocating. Eight bytes holds any single UTF-8 code
        //! point, which is at most four, with room to spare.
        char mText[8] { 0 };

        //! What the platform called the key: a Win32 virtual-key code, an X11 keysym, or a Linux
        //! evdev code on Wayland and DRM.
        //!
        //! The escape hatch for everything Key does not name. Platform-specific by definition, so
        //! code that reads it should say which platform it is reading it on.
        unsigned int mNativeCode { 0 };

        //! True if this press came from the key being held rather than from a fresh press.
        //!
        //! Always false on a release. A text field wants repeats; a game usually does not, and this
        //! is what lets it tell them apart without timing them itself.
        bool mAutoRepeat { false };

        //! When the window system generated the event. As MouseEvent::mTimestampMs.
        unsigned long mTimestampMs { 0 };
    };

    //! A finger touching down, as wl_touch_listener::down reports it.
    //!
    //! The touch events mirror Wayland's wl_touch one for one, because Wayland is the only platform
    //! QtLikeSignalGui reports touch on -- see the touch signals on Window. Inventing a different shape
    //! would have meant translating into it and back out of it again for no reader's benefit.
    struct TouchDownEvent
    {
        //! The compositor's serial for this event.
        //!
        //! Opaque here, but a Wayland client needs it to ask for a move, a resize or a popup grab:
        //! each of those requests must quote the serial of the input event that justified it.
        //! Dropping it would make every such request impossible to build from what we deliver.
        unsigned int mSerial { 0 };

        unsigned int mTimeMs { 0 };   //!< Compositor timestamp in milliseconds.
        int mId { 0 };                //!< Identifies this finger until it lifts; reused afterwards.
        double mX { 0.0 };            //!< X in surface coordinates.
        double mY { 0.0 };            //!< Y in surface coordinates.
    };

    //! A finger lifting, as wl_touch_listener::up reports it.
    struct TouchUpEvent
    {
        unsigned int mSerial { 0 };   //!< As TouchDownEvent::mSerial.
        unsigned int mTimeMs { 0 };   //!< Compositor timestamp in milliseconds.

        //! The finger that lifted.
        //!
        //! There is deliberately no position here: the protocol does not carry one on up. A slot
        //! that needs the release point has to remember the last motion for this id.
        int mId { 0 };
    };

    //! A finger moving, as wl_touch_listener::motion reports it. No serial; the protocol has none.
    struct TouchMotionEvent
    {
        unsigned int mTimeMs { 0 };   //!< Compositor timestamp in milliseconds.
        int mId { 0 };                //!< The finger that moved.
        double mX { 0.0 };            //!< X in surface coordinates.
        double mY { 0.0 };            //!< Y in surface coordinates.
    };
}

#endif // QT_LIKE_SIGNAL_GUI_INPUTEVENTS_HPP
